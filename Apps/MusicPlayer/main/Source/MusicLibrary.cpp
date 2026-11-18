#include "MusicPlayer.h"

#include <tactility/log.h>

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <utility>

namespace {

bool hasMp3Extension(const char* name) {
    const size_t length = strlen(name);
    return length >= 4 && name[length - 4] == '.' && tolower(static_cast<unsigned char>(name[length - 3])) == 'm' &&
           tolower(static_cast<unsigned char>(name[length - 2])) == 'p' &&
           tolower(static_cast<unsigned char>(name[length - 1])) == '3';
}

uint32_t readBigEndian32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

uint32_t readSyncSafe32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0] & 0x7f) << 21) | (static_cast<uint32_t>(bytes[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(bytes[2] & 0x7f) << 7) | static_cast<uint32_t>(bytes[3] & 0x7f);
}

void appendUtf8(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string decodeText(const uint8_t* data, size_t length) {
    if (length == 0) {
        return {};
    }

    const uint8_t encoding = data[0];
    std::string output;
    if (encoding == 3) {
        for (size_t i = 1; i < length && data[i] != 0; i++) {
            output.push_back(static_cast<char>(data[i]));
        }
    } else if (encoding == 1 || encoding == 2) {
        size_t offset = 1;
        bool littleEndian = encoding == 1;
        if (encoding == 1 && length >= 3 && data[1] == 0xff && data[2] == 0xfe) {
            littleEndian = true;
            offset = 3;
        } else if (encoding == 1 && length >= 3 && data[1] == 0xfe && data[2] == 0xff) {
            littleEndian = false;
            offset = 3;
        }

        while (offset + 1 < length) {
            uint32_t codepoint = littleEndian ? static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8))
                                              : static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
            offset += 2;
            if (codepoint == 0) {
                break;
            }
            if (codepoint >= 0xd800 && codepoint <= 0xdbff && offset + 1 < length) {
                const uint16_t low = littleEndian ? static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8))
                                                  : static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    offset += 2;
                }
            }
            appendUtf8(output, codepoint);
        }
    } else {
        for (size_t i = 1; i < length && data[i] != 0; i++) {
            appendUtf8(output, data[i]);
        }
    }

    while (!output.empty() && (output.back() == ' ' || output.back() == '\r' || output.back() == '\n')) {
        output.pop_back();
    }
    return output;
}

void assignTextFrame(const char* id, const uint8_t* data, size_t length, MusicTrack& track) {
    if (length == 0) {
        return;
    }
    if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0) {
        track.title = decodeText(data, length);
    } else if (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0) {
        track.artist = decodeText(data, length);
    } else if (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0) {
        track.album = decodeText(data, length);
    } else if (strcmp(id, "TLEN") == 0 || strcmp(id, "TLE") == 0) {
        const std::string milliseconds = decodeText(data, length);
        if (!milliseconds.empty()) {
            char* end = nullptr;
            const unsigned long value = strtoul(milliseconds.c_str(), &end, 10);
            if (end != milliseconds.c_str() && *end == '\0') {
                track.durationSeconds = static_cast<uint32_t>((value + 500) / 1000);
            }
        }
    }
}

void readId3v2(FILE* file, MusicTrack& track) {
    uint8_t header[10];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
        return;
    }

    const uint8_t version = header[3];
    if (version < 2 || version > 4) {
        return;
    }

    uint32_t tagSize = readSyncSafe32(header + 6);
    if (tagSize > 1024 * 1024) {
        return;
    }

    uint32_t position = 0;
    if ((header[5] & 0x40) != 0 && version >= 3 && tagSize >= 4) {
        uint8_t extendedSize[4];
        if (fread(extendedSize, 1, sizeof(extendedSize), file) != sizeof(extendedSize)) {
            return;
        }
        position = version == 4 ? readSyncSafe32(extendedSize) : readBigEndian32(extendedSize);
        if (version == 3) {
            position += 4;
        }
        if (position > tagSize) {
            return;
        }
        if (fseek(file, 10 + position, SEEK_SET) != 0) {
            return;
        }
    }

    const uint32_t frameHeaderSize = version == 2 ? 6 : 10;
    while (position + frameHeaderSize <= tagSize) {
        uint8_t frameHeader[10] = {};
        if (fread(frameHeader, 1, frameHeaderSize, file) != frameHeaderSize) {
            break;
        }
        bool empty = true;
        for (uint32_t i = 0; i < (version == 2 ? 3u : 4u); i++) {
            empty = empty && frameHeader[i] == 0;
        }
        if (empty) {
            break;
        }

        char frameId[5] = {};
        memcpy(frameId, frameHeader, version == 2 ? 3 : 4);
        const uint32_t frameSize = version == 2
                                       ? (static_cast<uint32_t>(frameHeader[3]) << 16) |
                                             (static_cast<uint32_t>(frameHeader[4]) << 8) | frameHeader[5]
                                       : (version == 4 ? readSyncSafe32(frameHeader + 4)
                                                       : readBigEndian32(frameHeader + 4));
        if (frameSize == 0 || frameSize > tagSize - position - frameHeaderSize) {
            break;
        }

        const bool interesting = frameId[0] == 'T' &&
                                 (strcmp(frameId, "TIT2") == 0 || strcmp(frameId, "TPE1") == 0 ||
                                  strcmp(frameId, "TALB") == 0 || strcmp(frameId, "TLEN") == 0 ||
                                  strcmp(frameId, "TT2") == 0 || strcmp(frameId, "TP1") == 0 ||
                                  strcmp(frameId, "TAL") == 0 || strcmp(frameId, "TLE") == 0);
        if (interesting) {
            uint8_t text[512];
            const size_t textSize = std::min<size_t>(frameSize, sizeof(text));
            if (fread(text, 1, textSize, file) != textSize) {
                break;
            }
            assignTextFrame(frameId, text, textSize, track);
            if (frameSize > textSize && fseek(file, static_cast<long>(frameSize - textSize), SEEK_CUR) != 0) {
                break;
            }
        } else if (fseek(file, static_cast<long>(frameSize), SEEK_CUR) != 0) {
            break;
        }
        position += frameHeaderSize + frameSize;
    }
}

std::string decodeId3v1Text(const uint8_t* data, size_t length) {
    while (length > 0 && (data[length - 1] == 0 || data[length - 1] == ' ')) {
        length--;
    }
    std::string value;
    for (size_t i = 0; i < length && data[i] != 0; i++) {
        appendUtf8(value, data[i]);
    }
    return value;
}

void readId3v1(FILE* file, MusicTrack& track) {
    if (fseek(file, -128, SEEK_END) != 0) {
        return;
    }
    uint8_t tag[128];
    if (fread(tag, 1, sizeof(tag), file) != sizeof(tag) || memcmp(tag, "TAG", 3) != 0) {
        return;
    }
    if (track.title.empty()) {
        track.title = decodeId3v1Text(tag + 3, 30);
    }
    if (track.artist.empty()) {
        track.artist = decodeId3v1Text(tag + 33, 30);
    }
    if (track.album.empty()) {
        track.album = decodeId3v1Text(tag + 63, 30);
    }
}

void readTrackMetadata(MusicTrack& track) {
    FILE* file = fopen(track.path.c_str(), "rb");
    if (file == nullptr) {
        return;
    }
    readId3v2(file, track);
    readId3v1(file, track);
    fclose(file);

    if (track.title.empty()) {
        const size_t slash = track.path.find_last_of('/');
        const size_t dot = track.path.find_last_of('.');
        const size_t start = slash == std::string::npos ? 0 : slash + 1;
        const size_t end = dot == std::string::npos || dot < start ? track.path.size() : dot;
        track.title = track.path.substr(start, end - start);
    }
}

bool isValidSdFolder(const std::string& path) {
    if (path.compare(0, 8, "/sdcard/") != 0 || path.find_first_of("\r\n\\") != std::string::npos) {
        return false;
    }

    size_t componentStart = 8;
    while (componentStart < path.size()) {
        const size_t componentEnd = path.find('/', componentStart);
        const size_t componentSize = componentEnd == std::string::npos ? std::string::npos : componentEnd - componentStart;
        if (path.compare(componentStart, componentSize, "..") == 0) {
            return false;
        }
        if (componentEnd == std::string::npos) {
            break;
        }
        componentStart = componentEnd + 1;
    }
    return true;
}

} // namespace

void musicPlayerScanFolder(MusicPlayerContext* context, const std::string& path) {
    if (!isValidSdFolder(path)) {
        {
            std::lock_guard lock(context->tracksMutex);
            context->tracks.clear();
            context->selectedTrackIndex = -1;
        }
        {
            std::lock_guard lock(context->statusMutex);
            context->statusText = "Choose a folder under /sdcard";
        }
        context->folderPath = path;
        return;
    }

    context->folderPath = path;
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr && errno == ENOENT && path == "/sdcard/Music") {
        mkdir(path.c_str(), 0755);
        directory = opendir(path.c_str());
    }

    if (directory == nullptr) {
        {
            std::lock_guard lock(context->tracksMutex);
            context->tracks.clear();
            context->selectedTrackIndex = -1;
        }
        {
            std::lock_guard lock(context->statusMutex);
            context->statusText = "Cannot open folder (check SD card)";
        }
        LOG_W(MUSIC_PLAYER_TAG, "Cannot open music folder %s: errno %d", path.c_str(), errno);
        return;
    }

    std::vector<MusicTrack> scanned;
    struct dirent* entry;
    while ((entry = readdir(directory)) != nullptr) {
        if (!hasMp3Extension(entry->d_name)) {
            continue;
        }

        MusicTrack track;
        track.path = path + "/" + entry->d_name;
        struct stat status {};
        if (stat(track.path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
            continue;
        }
        readTrackMetadata(track);
        scanned.push_back(std::move(track));
    }
    closedir(directory);

    std::sort(scanned.begin(), scanned.end(), [](const MusicTrack& left, const MusicTrack& right) {
        return strcasecmp(left.title.c_str(), right.title.c_str()) < 0;
    });

    const size_t count = scanned.size();
    {
        std::lock_guard lock(context->tracksMutex);
        context->tracks = std::move(scanned);
        context->selectedTrackIndex = context->tracks.empty() ? -1 : 0;
    }
    {
        std::lock_guard lock(context->statusMutex);
        context->statusText = count == 0 ? "No MP3 files found" : "Ready";
    }
}
