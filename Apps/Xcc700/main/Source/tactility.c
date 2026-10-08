/*
 * xcc700.c is built with main() and open() renamed (see main/CMakeLists.txt):
 * - main() here requires an input file, and names the output after it when there's no "-o".
 * - open() translates the Linux flags that xcc700 hardcodes to the platform's own.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XCC700_O_CREAT 64
#define XCC700_O_TRUNC 512

int xcc700_main(int argc, char** argv);

int xcc700_open(char* path, int flags, int mode) {
    int platform_flags = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (flags & XCC700_O_CREAT) {
        platform_flags |= O_CREAT;
    }
    if (flags & XCC700_O_TRUNC) {
        platform_flags |= O_TRUNC;
    }
    return open(path, platform_flags, mode);
}

/** @return the input path with its extension replaced by ".elf", NULL when out of memory */
static char* get_default_output_path(const char* input_path) {
    const char* name = strrchr(input_path, '/');
    name = (name != NULL) ? name + 1 : input_path;
    const char* extension = strrchr(name, '.');
    // A leading dot is part of the name (e.g. ".c" becomes ".c.elf")
    const size_t base_length = (extension != NULL && extension != name) ? (size_t)(extension - input_path) : strlen(input_path);
    char* output_path = malloc(base_length + sizeof(".elf"));
    if (output_path != NULL) {
        memcpy(output_path, input_path, base_length);
        memcpy(output_path + base_length, ".elf", sizeof(".elf"));
    }
    return output_path;
}

int main(int argc, char* argv[]) {
    // xcc700 only takes the "-o" option after the input file
    if (argc == 4 && strcmp(argv[2], "-o") == 0) {
        return xcc700_main(argc, argv);
    }
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "Usage: xcc700 FILE.c [-o OUTPUT]\n");
        return EXIT_FAILURE;
    }

    char* output_path = get_default_output_path(argv[1]);
    if (output_path == NULL) {
        fprintf(stderr, "xcc700: out of memory\n");
        return EXIT_FAILURE;
    }
    char* forwarded_argv[] = { argv[0], argv[1], "-o", output_path, NULL };
    const int result = xcc700_main(4, forwarded_argv);
    free(output_path);
    return result;
}
