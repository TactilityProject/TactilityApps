// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>

namespace maps {

/**
 * Puts a finished temporary file in the place of `path`. FAT cannot rename onto an existing file, so the old one is
 * removed first: a power cut in between leaves only the temporary file, which recoverFile() puts in place again.
 * Call it only when the temporary file was written and closed without an error.
 */
inline bool replaceFile(const char* temporary, const char* path) {
    if (rename(temporary, path) == 0) {
        return true;
    }
    remove(path);
    return rename(temporary, path) == 0;
}

/** Before reading `path`: when it is missing and `temporary` is there (see replaceFile), the temporary file becomes it. */
inline void recoverFile(const char* path, const char* temporary) {
    FILE* file = fopen(path, "rb");
    if (file != nullptr) {
        fclose(file);
        return;
    }
    rename(temporary, path);
}

/** The same with the usual name of the temporary file, `<path>.tmp`. */
inline void recoverFile(const char* path) {
    char temporary[320];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) < static_cast<int>(sizeof(temporary))) {
        recoverFile(path, temporary);
    }
}

} // namespace maps
