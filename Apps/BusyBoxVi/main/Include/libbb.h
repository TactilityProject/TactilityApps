/* vi: set sw=4 ts=4: */
/*
 * Minimal subset of BusyBox's libbb.h: just what editors/vi.c and libbb/read_key.c need.
 *
 * Licensed under GPLv2, see file LICENSE in this app's directory.
 */
#pragma once

/* memrchr() and other GNU extensions */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/poll.h>
#include <regex.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif

/* region Configuration */

#define BB_VER "1.37.0"

#define ENABLE_VI 1
#define IF_VI(...) __VA_ARGS__
#define CONFIG_FEATURE_VI_MAX_LEN 4096
#define ENABLE_FEATURE_VI_8BIT 0
#define ENABLE_FEATURE_VI_COLON 1
#define IF_FEATURE_VI_COLON(...) __VA_ARGS__
#define ENABLE_FEATURE_VI_COLON_EXPAND 1
#define ENABLE_FEATURE_VI_YANKMARK 1
#define ENABLE_FEATURE_VI_SEARCH 1
#define IF_FEATURE_VI_SEARCH(...) __VA_ARGS__
/* vi.c uses the GNU re_search() API, which newlib doesn't provide */
#define ENABLE_FEATURE_VI_REGEX_SEARCH 0
#define ENABLE_FEATURE_VI_USE_SIGNALS 0
#define ENABLE_FEATURE_VI_DOT_CMD 1
#define ENABLE_FEATURE_VI_READONLY 1
#define IF_FEATURE_VI_READONLY(...) __VA_ARGS__
#define ENABLE_FEATURE_VI_SETOPTS 1
#define IF_FEATURE_VI_SETOPTS(...) __VA_ARGS__
#define ENABLE_FEATURE_VI_SET 1
#define ENABLE_FEATURE_VI_WIN_RESIZE 1
#define ENABLE_FEATURE_VI_ASK_TERMINAL 0
#define IF_FEATURE_VI_ASK_TERMINAL(...)
#define ENABLE_FEATURE_VI_UNDO 1
#define ENABLE_FEATURE_VI_UNDO_QUEUE 1
#define CONFIG_FEATURE_VI_UNDO_QUEUE_MAX 256
#define ENABLE_FEATURE_VI_VERBOSE_STATUS 1
#define ENABLE_FEATURE_VI_CRASHME 0
#define IF_FEATURE_VI_CRASHME(...)
#define ENABLE_FEATURE_ALLOW_EXEC 0
#define ENABLE_LOCALE_SUPPORT 0

/* read_key.c */
#define ENABLE_FEATURE_EDITING_ASK_TERMINAL 0
#define ENABLE_FEATURE_LESS_ASK_TERMINAL 0

/* endregion */

/* region Compiler helpers */

#define FAST_FUNC
#define ALIGN1
#define UNUSED_PARAM __attribute__((unused))
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define MAIN_EXTERNALLY_VISIBLE
#define ARRAY_SIZE(x) ((unsigned)(sizeof(x) / sizeof((x)[0])))

typedef signed char smallint;
typedef unsigned char smalluint;

#define TRUE 1
#define FALSE 0

#define STRERROR_FMT "%s"
#define STRERROR_ERRNO , strerror(errno)

extern struct globals* ptr_to_globals;
#define SET_PTR_TO_GLOBALS(x) (ptr_to_globals = (struct globals*)(x))

extern const char* applet_name;

/* endregion */

/* region Linked list (only what getopt32's "::*" argument needs) */

typedef struct llist_t {
    struct llist_t* link;
    char* data;
} llist_t;

void llist_add_to_end(llist_t** list_head, void* data);
void* llist_pop(llist_t** elm);

/* endregion */

/* region Memory and strings */

/* Allocations from the x*() functions are tracked, see bb_free_all() */
void* xmalloc(size_t size);
void* xzalloc(size_t size);
void* xrealloc(void* old, size_t size);
char* xstrdup(const char* s);
char* xstrndup(const char* s, int n);
char* xasprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
/* vi frees x*() allocations with plain free() */
void bb_free(void* ptr);
#define free bb_free
/* Frees every x*() allocation that is still live */
void bb_free_all(void);

char* skip_whitespace(const char* s);
char* skip_non_whitespace(const char* s);
char* bb_strchrnul(const char* s, int c);
#define strchrnul bb_strchrnul
int index_in_strings(const char* strings, const char* key);
unsigned bb_strtou(const char* arg, char** endp, int base);
char* concat_path_file(const char* path, const char* filename);

/* endregion */

/* region I/O */

ssize_t safe_read(int fd, void* buf, size_t count);
ssize_t full_read(int fd, void* buf, size_t count);
ssize_t full_write(int fd, const void* buf, size_t count);
int safe_poll(struct pollfd* ufds, nfds_t nfds, int timeout_ms);
void* xmalloc_open_read_close(const char* filename, size_t* maxsz_p);
void bb_putchar(int ch);
#define fflush_all() fflush(NULL)
#define fputs_stdout(s) fputs((s), stdout)

/* endregion */

/* region Errors */

void bb_show_usage(void) __attribute__((noreturn));
void bb_simple_error_msg_and_die(const char* s) __attribute__((noreturn));

/* endregion */

/* region Options */

uint32_t getopt32(char** argv, const char* applet_opts, ...);

/* endregion */

/* region Terminal */

int get_terminal_width_height(int fd, unsigned* width, unsigned* height);
int tcsetattr_stdin_TCSANOW(const struct termios* tp);
#define TERMIOS_CLEAR_ISIG      (1 << 0)
#define TERMIOS_RAW_CRNL_INPUT  (1 << 1)
#define TERMIOS_RAW_CRNL_OUTPUT (1 << 2)
#define TERMIOS_RAW_CRNL        (TERMIOS_RAW_CRNL_INPUT | TERMIOS_RAW_CRNL_OUTPUT)
#define TERMIOS_RAW_INPUT       (1 << 3)
int set_termios_to_raw(int fd, struct termios* oldterm, int flags);

/* "Keycodes" that report an escape sequence.
 * We use something which fits into signed char,
 * yet doesn't represent any valid Unicode character.
 * Also, -1 is reserved for error indication and we don't use it. */
enum {
    KEYCODE_UP        =  -2,
    KEYCODE_DOWN      =  -3,
    KEYCODE_RIGHT     =  -4,
    KEYCODE_LEFT      =  -5,
    KEYCODE_HOME      =  -6,
    KEYCODE_END       =  -7,
    KEYCODE_INSERT    =  -8,
    KEYCODE_DELETE    =  -9,
    KEYCODE_PAGEUP    = -10,
    KEYCODE_PAGEDOWN  = -11,
    KEYCODE_BACKSPACE = -12,
    KEYCODE_D         = -13,
    KEYCODE_CTRL_RIGHT    = KEYCODE_RIGHT & ~0x40,
    KEYCODE_CTRL_LEFT     = KEYCODE_LEFT  & ~0x40,
    KEYCODE_ALT_RIGHT     = KEYCODE_RIGHT & ~0x20,
    KEYCODE_ALT_LEFT      = KEYCODE_LEFT  & ~0x20,
    KEYCODE_ALT_BACKSPACE = KEYCODE_BACKSPACE & ~0x20,
    KEYCODE_ALT_D         = KEYCODE_D     & ~0x20,
    KEYCODE_CURSOR_POS = -0x100,
    KEYCODE_BUFFER_SIZE = 16
};
int64_t read_key(int fd, char* buffer, int timeout);
int64_t safe_read_key(int fd, char* buffer, int timeout);

/* endregion */
