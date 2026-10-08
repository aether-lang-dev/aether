/* One line of a text file, whatever its length.
 *
 * fgets into a fixed buffer splits a longer line, and the next call returns
 * the rest as a line of its own: a value cut short, a path that names the
 * wrong file, a header scan that stops at a chunk that does not start the
 * way a line does (#2535, #2536). Header-only, so the `ae` binary and apkg
 * share the one reader with no build change. */
#ifndef AE_LINE_H
#define AE_LINE_H

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads one line into *buf, grown as needed. The caller owns the buffer:
 * start with NULL and 0, reuse it for every line, free it when done. The
 * newline stays when the line has one, so a caller can tell a last line the
 * file ended in the middle of. 1 for a line, 0 at the end of the file, -1
 * when out of memory. */
static inline int ae_read_line(FILE* f, char** buf, size_t* cap) {
    size_t len = 0;
    for (;;) {
        if (len + 1 >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 512;
            char* nb = (char*)realloc(*buf, ncap);
            if (!nb) return -1;
            *buf = nb;
            *cap = ncap;
        }
        size_t room = *cap - len;
        if (room > INT_MAX) room = INT_MAX;
        if (!fgets(*buf + len, (int)room, f)) {
            (*buf)[len] = '\0';   /* a read error leaves the array undefined */
            return len > 0;
        }
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len - 1] == '\n') return 1;
        if (feof(f)) return 1;
    }
}

#endif /* AE_LINE_H */
