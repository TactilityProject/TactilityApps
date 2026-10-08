/*
 * Licensed under GPLv2, see file LICENSE in this app's directory.
 */
#include <setjmp.h>
#include <stdlib.h>

int vi_main(int argc, char** argv);
void bb_free_all(void);
void vi_cleanup(void);
extern jmp_buf bb_die_jmp;

int main(int argc, char* argv[]) {
    int result;
    if (setjmp(bb_die_jmp) == 0) {
        result = vi_main(argc, argv);
    } else {
        vi_cleanup();
        result = EXIT_FAILURE;
    }
    /* vi leaves its buffers to process exit, which an app instance doesn't have */
    bb_free_all();
    return result;
}
