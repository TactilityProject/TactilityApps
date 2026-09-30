/* vi: set sw=4 ts=4: */
/*
 * Minimal subset of BusyBox's libbb: just what editors/vi.c and libbb/read_key.c need.
 * Based on libbb/{xfuncs,xfuncs_printf,compare_string_array,read,safe_poll,llist}.c.
 *
 * Licensed under GPLv2, see file LICENSE in this app's directory.
 */
#include "libbb.h"

/* This file implements bb_free() itself, so free() means libc's own below */
#undef free

struct globals* ptr_to_globals;
const char* applet_name = "vi";

/* region Linked list */

void llist_add_to_end(llist_t** list_head, void* data) {
    while (*list_head) {
        list_head = &(*list_head)->link;
    }
    *list_head = xzalloc(sizeof(llist_t));
    (*list_head)->data = data;
}

void* llist_pop(llist_t** head) {
    llist_t* temp = *head;
    if (temp == NULL) {
        return NULL;
    }
    void* data = temp->data;
    *head = temp->link;
    bb_free(temp);
    return data;
}

/* endregion */

/* region Memory and strings */

/*
 * Every block handed out below is tracked, so bb_free_all() can release what vi itself never frees
 * (it relies on process exit, which an app instance doesn't have).
 */
typedef union AllocHeader {
    struct {
        union AllocHeader* prev;
        union AllocHeader* next;
    } links;
    max_align_t alignment;
} AllocHeader;

static AllocHeader* allocations = NULL;

static void* track(AllocHeader* header) {
    header->links.prev = NULL;
    header->links.next = allocations;
    if (allocations != NULL) {
        allocations->links.prev = header;
    }
    allocations = header;
    return header + 1;
}

static AllocHeader* untrack(void* ptr) {
    AllocHeader* header = (AllocHeader*)ptr - 1;
    if (header->links.prev != NULL) {
        header->links.prev->links.next = header->links.next;
    } else {
        allocations = header->links.next;
    }
    if (header->links.next != NULL) {
        header->links.next->links.prev = header->links.prev;
    }
    return header;
}

void* xmalloc(size_t size) {
    AllocHeader* header = malloc(sizeof(AllocHeader) + size);
    if (header == NULL) {
        bb_simple_error_msg_and_die("out of memory");
    }
    return track(header);
}

void* xzalloc(size_t size) {
    void* ptr = xmalloc(size);
    memset(ptr, 0, size);
    return ptr;
}

void* xrealloc(void* old, size_t size) {
    if (old == NULL) {
        return xmalloc(size);
    }
    AllocHeader* header = realloc(untrack(old), sizeof(AllocHeader) + size);
    if (header == NULL) {
        bb_simple_error_msg_and_die("out of memory");
    }
    return track(header);
}

void bb_free(void* ptr) {
    if (ptr != NULL) {
        free(untrack(ptr));
    }
}

void bb_free_all(void) {
    while (allocations != NULL) {
        AllocHeader* next = allocations->links.next;
        free(allocations);
        allocations = next;
    }
}

char* xstrdup(const char* s) {
    size_t size = strlen(s) + 1;
    char* t = xmalloc(size);
    memcpy(t, s, size);
    return t;
}

char* xstrndup(const char* s, int n) {
    size_t len = 0;
    while (len < (size_t)n && s[len] != '\0') {
        len++;
    }
    char* t = xmalloc(len + 1);
    memcpy(t, s, len);
    t[len] = '\0';
    return t;
}

char* xasprintf(const char* format, ...) {
    va_list p;
    va_start(p, format);
    int needed = vsnprintf(NULL, 0, format, p);
    va_end(p);
    if (needed < 0) {
        bb_simple_error_msg_and_die("out of memory");
    }
    char* string_ptr = xmalloc((size_t)needed + 1);
    va_start(p, format);
    vsnprintf(string_ptr, (size_t)needed + 1, format, p);
    va_end(p);
    return string_ptr;
}

char* skip_whitespace(const char* s) {
    /* In POSIX/C locale (the only locale we care about: do we REALLY want
     * to allow Unicode whitespace in, say, .conf files? nuts!)
     * isspace is only these chars: "\t\n\v\f\r" and space.
     * "\t\n\v\f\r" happen to have ASCII codes 9,10,11,12,13.
     * Use that.
     */
    while (*s == ' ' || (unsigned char)(*s - 9) <= (13 - 9)) {
        s++;
    }
    return (char*)s;
}

char* skip_non_whitespace(const char* s) {
    while (*s != '\0' && *s != ' ' && (unsigned char)(*s - 9) > (13 - 9)) {
        s++;
    }
    return (char*)s;
}

char* bb_strchrnul(const char* s, int c) {
    while (*s != '\0' && *s != c) {
        s++;
    }
    return (char*)s;
}

int index_in_strings(const char* strings, const char* key) {
    int j, idx = 0;

    while (*strings) {
        /* Do we see "key\0" at current position in strings? */
        for (j = 0; *strings == key[j]; ++j) {
            if (*strings++ == '\0') {
                return idx; /* yes */
            }
        }
        /* No.  Move to the start of the next string. */
        while (*strings++ != '\0') {
            continue;
        }
        idx++;
    }
    return -1;
}

unsigned bb_strtou(const char* arg, char** endp, int base) {
    char* endptr;
    if (endp == NULL) {
        endp = &endptr;
    }
    if (!isalnum((unsigned char)arg[0])) {
        *endp = (char*)arg;
        errno = ERANGE;
        return UINT32_MAX;
    }
    errno = 0;
    unsigned long v = strtoul(arg, endp, base);
    if (v > UINT32_MAX) {
        errno = ERANGE;
        return UINT32_MAX;
    }
    if (**endp != '\0') {
        errno = EINVAL;
    }
    return (unsigned)v;
}

char* concat_path_file(const char* path, const char* filename) {
    size_t len = strlen(path);
    const char* separator = (len > 0 && path[len - 1] == '/') ? "" : "/";
    while (*filename == '/') {
        filename++;
    }
    return xasprintf("%s%s%s", path, separator, filename);
}

/* endregion */

/* region I/O */

ssize_t safe_read(int fd, void* buf, size_t count) {
    ssize_t n;
    do {
        n = read(fd, buf, count);
    } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t full_read(int fd, void* buf, size_t len) {
    ssize_t total = 0;
    while (len) {
        ssize_t cc = safe_read(fd, buf, len);
        if (cc < 0) {
            if (total) {
                /* we already have some! */
                /* user can do another read to know the error code */
                return total;
            }
            return cc; /* read() returns -1 on failure. */
        }
        if (cc == 0) {
            break;
        }
        buf = ((char*)buf) + cc;
        total += cc;
        len -= (size_t)cc;
    }
    return total;
}

ssize_t full_write(int fd, const void* buf, size_t len) {
    ssize_t total = 0;
    while (len) {
        ssize_t cc = write(fd, buf, len);
        if (cc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (total) {
                /* we already wrote some! */
                /* user can do another write to know the error code */
                return total;
            }
            return cc; /* write() returns -1 on failure. */
        }
        total += cc;
        buf = ((const char*)buf) + cc;
        len -= (size_t)cc;
    }
    return total;
}

int safe_poll(struct pollfd* ufds, nfds_t nfds, int timeout) {
    while (1) {
        int n = poll(ufds, nfds, timeout);
        if (n >= 0) {
            return n;
        }
        /* Make sure we inch towards completion */
        if (timeout > 0) {
            timeout--;
        }
        /* E.g. strace causes poll to return this */
        if (errno == EINTR) {
            continue;
        }
        return n;
    }
}

void* xmalloc_open_read_close(const char* filename, size_t* maxsz_p) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t size = 0;
    size_t capacity = 1024;
    char* buf = xmalloc(capacity);
    while (1) {
        if (size + 1 >= capacity) {
            capacity *= 2;
            buf = xrealloc(buf, capacity);
        }
        ssize_t n = safe_read(fd, buf + size, capacity - size - 1);
        if (n < 0) {
            bb_free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        size += (size_t)n;
    }
    close(fd);
    buf[size] = '\0';
    if (maxsz_p) {
        *maxsz_p = size;
    }
    return buf;
}

void bb_putchar(int ch) {
    putchar(ch);
}

/* endregion */

/* region Errors */

void bb_show_usage(void) {
    fputs("Usage: vi [-c CMD] [-R] [-H] [FILE]...\n", stderr);
    exit(EXIT_FAILURE);
}

void bb_simple_error_msg_and_die(const char* s) {
    fprintf(stderr, "%s: %s\n", applet_name, s);
    exit(EXIT_FAILURE);
}

/* endregion */

/* region Options */

/* Subset of libbb's getopt32(): plain flags, "x:" options with a required argument and "x:*"
 * options that collect every occurrence into an llist_t. Each ':' option consumes one pointer
 * argument, in order. Returns a bitmask with bit N set for the N-th option character. */
uint32_t getopt32(char** argv, const char* applet_opts, ...) {
    char optstring[64];
    char letters[32];
    void* targets[32];
    bool collect[32];
    int count = 0;
    size_t optstring_len = 0;

    va_list p;
    va_start(p, applet_opts);
    for (const char* s = applet_opts; *s != '\0' && count < 32; s++) {
        letters[count] = *s;
        targets[count] = NULL;
        collect[count] = false;
        optstring[optstring_len++] = *s;
        if (s[1] == ':') {
            optstring[optstring_len++] = ':';
            targets[count] = va_arg(p, void*);
            s++;
            if (s[1] == '*') {
                collect[count] = true;
                s++;
            }
        }
        count++;
    }
    va_end(p);
    optstring[optstring_len] = '\0';

    int argc = 0;
    while (argv[argc] != NULL) {
        argc++;
    }

    uint32_t result = 0;
    int c;
    /* getopt() state is shared with every other program: 0 fully resets it (1 keeps the position within an argument) */
    optind = 0;
    while ((c = getopt(argc, argv, optstring)) != -1) {
        int i = 0;
        while (i < count && letters[i] != c) {
            i++;
        }
        if (i == count) {
            bb_show_usage();
        }
        result |= (uint32_t)1 << i;
        if (targets[i] != NULL) {
            if (collect[i]) {
                llist_add_to_end((llist_t**)targets[i], optarg);
            } else {
                *(char**)targets[i] = optarg;
            }
        }
    }
    return result;
}

/* endregion */

/* region Terminal */

int get_terminal_width_height(int fd, unsigned* width, unsigned* height) {
    struct winsize win;
    win.ws_row = 0;
    win.ws_col = 0;
    /* I've seen ioctl returning 0, but row/col is (still?) 0.
     * We treat that as an error too.  */
    int err = ioctl(fd, TIOCGWINSZ, &win) != 0 || win.ws_row == 0;
    if (height) {
        *height = win.ws_row ? win.ws_row : 24;
    }
    if (width) {
        *width = win.ws_col ? win.ws_col : 80;
    }
    return err;
}

int tcsetattr_stdin_TCSANOW(const struct termios* tp) {
    return tcsetattr(STDIN_FILENO, TCSANOW, tp);
}

int set_termios_to_raw(int fd, struct termios* oldterm, int flags) {
    struct termios newterm;

    memset(oldterm, 0, sizeof(*oldterm)); /* paranoia */
    tcgetattr(fd, oldterm);
    newterm = *oldterm;

    newterm.c_lflag &= ~(ICANON | ECHO | ECHONL);
    if (flags & TERMIOS_CLEAR_ISIG) {
        newterm.c_lflag &= ~ISIG;
    }
    newterm.c_cc[VMIN] = 1;
    newterm.c_cc[VTIME] = 0;
    if (flags & TERMIOS_RAW_CRNL_INPUT) {
        newterm.c_iflag &= ~(IXON | ICRNL);
    }
    if (flags & TERMIOS_RAW_CRNL_OUTPUT) {
        newterm.c_oflag &= ~(ONLCR);
    }
    return tcsetattr(fd, TCSANOW, &newterm);
}

/* endregion */
