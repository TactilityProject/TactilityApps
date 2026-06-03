#include "epub_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <miniz.h>

/* Prefer PSRAM for large EPUB buffers when available (ESP32 with SPIRAM).
 * Falls back to regular malloc so the code compiles on hosts without PSRAM. */
#ifdef CONFIG_SPIRAM
#  include <esp_heap_caps.h>
static void* epub_malloc_large(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}
#else
#  define epub_malloc_large(sz) malloc(sz)
#endif

#define ZIP_CENTRAL_HEADER_SIG 0x02014b50
#define ZIP_END_CENTRAL_SIG 0x06054b50

#pragma pack(push, 1)
typedef struct {
    uint32_t signature;
    uint16_t version_made;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint16_t disk_start;
    uint16_t internal_attr;
    uint32_t external_attr;
    uint32_t local_header_offset;
} zip_central_dir_entry;

typedef struct {
    uint32_t signature;
    uint16_t disk_num;
    uint16_t central_dir_disk;
    uint16_t entries_this_disk;
    uint16_t total_entries;
    uint32_t central_dir_size;
    uint32_t central_dir_offset;
    uint16_t comment_len;
} zip_end_central_dir;
#pragma pack(pop)

typedef struct {
    char* filename;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t compression;
} file_entry;

struct epub_reader_c {
    FILE* fp;
    file_entry* files;
    uint32_t file_count;
};

/* Stream context holds the fully decompressed file in memory.
 * This avoids the multi-call tinfl dictionary problem that occurs when
 * pOut_buf_next is reset between calls (losing LZ77 backreference history).
 * Peak memory during open = compressed_size + uncompressed_size; after open
 * only uncompressed_size is held. */
struct epub_stream_context_c {
    uint8_t* data;   /* decompressed (or stored) file bytes */
    uint32_t size;   /* total bytes */
    uint32_t pos;    /* current read position */
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

static int find_eocd(FILE* fp, zip_end_central_dir* eocd) {
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0) return 0;
    long search_range = (size > 1024) ? 1024 : size;
    if (search_range < 22) return 0; /* EOCD minimum size */
    fseek(fp, -search_range, SEEK_END);
    uint8_t* buf = (uint8_t*)malloc((size_t)search_range);
    if (!buf) return 0;
    size_t n = fread(buf, 1, (size_t)search_range, fp);
    if (n < 22) { free(buf); return 0; }
    for (int i = (int)n - 22; i >= 0; i--) {
        uint32_t sig;
        memcpy(&sig, &buf[i], sizeof(sig)); /* avoid unaligned pointer cast UB */
        if (sig == ZIP_END_CENTRAL_SIG) {
            memcpy(eocd, &buf[i], sizeof(zip_end_central_dir));
            free(buf);
            return 1;
        }
    }
    free(buf);
    return 0;
}

/* Seek fp past the local file header to the first byte of (compressed) data. */
static int seek_to_local_data(FILE* fp, uint32_t local_header_offset) {
    if (fseek(fp, (long)local_header_offset, SEEK_SET) != 0) return 0;
    uint32_t sig;
    if (fread(&sig, 4, 1, fp) != 1) return 0;
    /* Skip: version_needed(2) flags(2) compression(2) mod_time(2) mod_date(2)
             crc32(4) compressed_size(4) uncompressed_size(4) = 22 bytes */
    if (fseek(fp, 22, SEEK_CUR) != 0) return 0;
    uint16_t nlen = 0, elen = 0;
    if (fread(&nlen, 2, 1, fp) != 1) return 0;
    if (fread(&elen, 2, 1, fp) != 1) return 0;
    if (fseek(fp, nlen + elen, SEEK_CUR) != 0) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

epub_parser_error epub_open_c(const char* filepath, epub_reader_c** out_reader) {
    if (!filepath || !out_reader) return EPUB_ERROR_INVALID_PARAM;
    FILE* fp = fopen(filepath, "rb");
    if (!fp) return EPUB_ERROR_FILE_NOT_FOUND;
    zip_end_central_dir eocd;
    if (!find_eocd(fp, &eocd)) {
        fclose(fp);
        return EPUB_ERROR_NOT_AN_EPUB;
    }
    epub_reader_c* reader = (epub_reader_c*)calloc(1, sizeof(epub_reader_c));
    if (!reader) { fclose(fp); return EPUB_ERROR_OUT_OF_MEMORY; }
    reader->fp = fp;
    reader->file_count = eocd.total_entries;
    reader->files = (file_entry*)calloc(reader->file_count, sizeof(file_entry));
    if (!reader->files) { fclose(fp); free(reader); return EPUB_ERROR_OUT_OF_MEMORY; }
    fseek(fp, eocd.central_dir_offset, SEEK_SET);
    for (uint32_t i = 0; i < reader->file_count; i++) {
        zip_central_dir_entry zcde;
        if (fread(&zcde, sizeof(zcde), 1, fp) != 1) {
            for (uint32_t j = 0; j < i; j++) free(reader->files[j].filename);
            free(reader->files);
            fclose(fp);
            free(reader);
            return EPUB_ERROR_NOT_AN_EPUB;
        }
        char* name = (char*)malloc(zcde.filename_len + 1u);
        if (!name) {
            for (uint32_t j = 0; j < i; j++) free(reader->files[j].filename);
            free(reader->files);
            fclose(fp);
            free(reader);
            return EPUB_ERROR_OUT_OF_MEMORY;
        }
        if (fread(name, 1, zcde.filename_len, fp) != zcde.filename_len) {
            free(name);
            for (uint32_t j = 0; j < i; j++) free(reader->files[j].filename);
            free(reader->files);
            fclose(fp);
            free(reader);
            return EPUB_ERROR_NOT_AN_EPUB;
        }
        name[zcde.filename_len] = '\0';
        if (fseek(fp, zcde.extra_len + zcde.comment_len, SEEK_CUR) != 0) {
            free(name);
            for (uint32_t j = 0; j < i; j++) free(reader->files[j].filename);
            free(reader->files);
            fclose(fp);
            free(reader);
            return EPUB_ERROR_NOT_AN_EPUB;
        }
        reader->files[i].filename = name;
        reader->files[i].compressed_size = zcde.compressed_size;
        reader->files[i].uncompressed_size = zcde.uncompressed_size;
        reader->files[i].local_header_offset = zcde.local_header_offset;
        reader->files[i].compression = zcde.compression;
    }
    *out_reader = reader;
    return EPUB_OK;
}

void epub_close_c(epub_reader_c* reader) {
    if (!reader) return;
    if (reader->fp) fclose(reader->fp);
    for (uint32_t i = 0; i < reader->file_count; i++) {
        free(reader->files[i].filename);
    }
    free(reader->files);
    free(reader);
}

uint32_t epub_get_file_count_c(epub_reader_c* reader) {
    return reader ? reader->file_count : 0;
}

const char* epub_get_filename_c(epub_reader_c* reader, uint32_t index) {
    if (!reader || index >= reader->file_count) return NULL;
    return reader->files[index].filename;
}

epub_stream_context_c* epub_stream_open_c(epub_reader_c* reader, const char* filename) {
    if (!reader || !filename) return NULL;

    /* Find the entry */
    file_entry* entry = NULL;
    for (uint32_t i = 0; i < reader->file_count; i++) {
        if (strcmp(reader->files[i].filename, filename) == 0) {
            entry = &reader->files[i];
            break;
        }
    }
    if (!entry) return NULL;

    epub_stream_context_c* ctx = (epub_stream_context_c*)calloc(1, sizeof(epub_stream_context_c));
    if (!ctx) return NULL;

    if (!seek_to_local_data(reader->fp, entry->local_header_offset)) {
        free(ctx);
        return NULL;
    }

    if (entry->compression == 0) {
        /* Stored - copy raw bytes directly */
        ctx->size = entry->uncompressed_size;
        ctx->data = (uint8_t*)epub_malloc_large(ctx->size);
        if (!ctx->data) { free(ctx); return NULL; }
        if (fread(ctx->data, 1, ctx->size, reader->fp) != ctx->size) {
            free(ctx->data); free(ctx); return NULL;
        }
    } else if (entry->compression == 8) {
        /* Deflate - buffer all compressed bytes then decompress in one shot.
         * We call tinfl_decompress directly (rather than the _mem_to_mem wrapper)
         * so we can place the ~10 KB tinfl_decompressor on the heap instead of
         * the stack, which would overflow the LVGL task.
         * Both the compressed and decompressed buffers can be hundreds of KB,
         * so allocate them in PSRAM when available. */
        uint8_t* compressed = (uint8_t*)epub_malloc_large(entry->compressed_size);
        if (!compressed) { free(ctx); return NULL; }
        if (fread(compressed, 1, entry->compressed_size, reader->fp) != entry->compressed_size) {
            free(compressed); free(ctx); return NULL;
        }

        ctx->size = entry->uncompressed_size;
        if (ctx->size > SIZE_MAX - 1) { free(compressed); free(ctx); return NULL; }
        ctx->data = (uint8_t*)epub_malloc_large((size_t)ctx->size + 1u); /* +1 so callers can NUL-terminate if needed */
        if (!ctx->data) { free(compressed); free(ctx); return NULL; }

        /* tinfl_decompressor is ~10 KB - must live on the heap, not the stack,
         * or it overflows the LVGL task's stack. */
        tinfl_decompressor* decomp = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
        if (!decomp) { free(compressed); free(ctx->data); free(ctx); return NULL; }
        tinfl_init(decomp);

        size_t out_len = ctx->size;
        size_t in_len  = entry->compressed_size;
        tinfl_status status = tinfl_decompress(
            decomp,
            (const mz_uint8*)compressed, &in_len,
            ctx->data, ctx->data, &out_len,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF /* single-pass, sequential output */
        );

        free(decomp);
        free(compressed);

        if (status != TINFL_STATUS_DONE) {
            free(ctx->data); free(ctx); return NULL;
        }
        ctx->size = (uint32_t)out_len;
    } else {
        /* Unsupported compression method */
        free(ctx);
        return NULL;
    }

    ctx->pos = 0;
    return ctx;
}

size_t epub_stream_read_c(epub_stream_context_c* ctx, void* buffer, size_t size) {
    if (!ctx || !ctx->data || !buffer || ctx->pos >= ctx->size) return 0;
    size_t available = ctx->size - ctx->pos;
    size_t to_copy = (available < size) ? available : size;
    memcpy(buffer, ctx->data + ctx->pos, to_copy);
    ctx->pos += (uint32_t)to_copy;
    return to_copy;
}

void epub_stream_close_c(epub_stream_context_c* ctx) {
    if (!ctx) return;
    free(ctx->data);
    free(ctx);
}
