/*
 * Licensed under GPLv2, see file LICENSE in this app's directory.
 */
int vi_main(int argc, char** argv);
void bb_free_all(void);

int main(int argc, char* argv[]) {
    int result = vi_main(argc, argv);
    /* vi leaves its buffers to process exit, which an app instance doesn't have */
    bb_free_all();
    return result;
}
