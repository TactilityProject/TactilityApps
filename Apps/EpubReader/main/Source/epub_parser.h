#ifndef EPUB_PARSER_H
#define EPUB_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EPUB_OK = 0,
    EPUB_ERROR_FILE_NOT_FOUND,          /**< The EPUB file itself could not be opened */
    EPUB_ERROR_NOT_AN_EPUB,             /**< File exists but is not a valid ZIP/EPUB */
    EPUB_ERROR_CORRUPTED,               /**< Archive structure is damaged */
    EPUB_ERROR_OUT_OF_MEMORY,           /**< A memory allocation failed */
    EPUB_ERROR_INVALID_PARAM,           /**< A required pointer argument was NULL */
    EPUB_ERROR_DECOMPRESSION_FAILED,    /**< Deflate decompression error */
    EPUB_ERROR_INTERNAL_FILE_NOT_FOUND  /**< A named file inside the EPUB archive was not found */
} epub_parser_error;

typedef struct epub_reader_c epub_reader_c;
typedef struct epub_stream_context_c epub_stream_context_c;

/**
 * Open an EPUB file for reading.
 * On success, EPUB_OK is returned and *out_reader is set to a new reader instance.
 * The reader must be freed with epub_close_c when no longer needed.
 * Returns an error code on failure; *out_reader is not modified.
 */
epub_parser_error epub_open_c(const char* filepath, epub_reader_c** out_reader);

/**
 * Close and free an epub_reader_c.
 * Safe to call with NULL (no-op).
 */
void epub_close_c(epub_reader_c* reader);

/**
 * Return the number of files in the EPUB archive.
 * Returns 0 if reader is NULL.
 */
uint32_t epub_get_file_count_c(epub_reader_c* reader);

/**
 * Return the filename of the archive entry at the given index.
 * The returned string is owned by the reader and is valid until epub_close_c is called.
 * Returns NULL if reader is NULL or index >= epub_get_file_count_c(reader).
 */
const char* epub_get_filename_c(epub_reader_c* reader, uint32_t index);

/**
 * Open a named file inside the EPUB archive for streaming.
 * Returns a stream context on success, or NULL if:
 *   - reader or filename is NULL
 *   - the file is not found in the archive
 *   - a memory allocation fails
 * The caller must call epub_stream_close_c when done.
 */
epub_stream_context_c* epub_stream_open_c(epub_reader_c* reader, const char* filename);

/**
 * Read up to size bytes from the open stream into buffer.
 * Returns the number of bytes actually read.
 * Returns 0 at end-of-file or on a decompression error (the two cases are not distinguished).
 */
size_t epub_stream_read_c(epub_stream_context_c* ctx, void* buffer, size_t size);

/**
 * Close and free a stream context.
 * Safe to call with NULL (no-op).
 */
void epub_stream_close_c(epub_stream_context_c* ctx);

#ifdef __cplusplus
}
#endif

#endif
