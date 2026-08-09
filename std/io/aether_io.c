#include "aether_io.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/aether_resource_caps.h"

#if !AETHER_HAS_FILESYSTEM
// Console I/O always works; file ops return errors
#include <stdio.h>
#include <stdlib.h>
void io_print(const char* s) { if (s) fputs(s, stdout); }
void io_print_line(const char* s) { if (s) puts(s); else puts(""); }
void io_print_int(int v) { printf("%d", v); }
void io_print_float(double v) { printf("%g", v); }
char* io_read_file_raw(const char* p) { (void)p; return NULL; }
int io_write_file_raw(const char* p, const char* c) { (void)p; (void)c; return 0; }
int io_append_file_raw(const char* p, const char* c) { (void)p; (void)c; return 0; }
int io_file_exists(const char* p) { (void)p; return 0; }
int io_delete_file_raw(const char* p) { (void)p; return 0; }
FileInfo* io_file_info_raw(const char* p) { (void)p; return NULL; }
void io_file_info_free(FileInfo* i) { (void)i; }
char* io_getenv(const char* n) { (void)n; return NULL; }
int io_setenv_raw(const char* n, const char* v) { (void)n; (void)v; return 0; }
int io_unsetenv_raw(const char* n) { (void)n; return 0; }
int io_stderr_write_raw(const char* d, int n) {
    if (!d || n <= 0) return 0;
    fflush(stderr);
    size_t w = fwrite(d, 1, (size_t)n, stderr);
    fflush(stderr);
    return (int)w;
}
int io_stdout_write_raw(const char* d, int n) {
    if (!d || n <= 0) return 0;
    fflush(stdout);
    size_t w = fwrite(d, 1, (size_t)n, stdout);
    fflush(stdout);
    return (int)w;
}
void io_perror_raw(const char* name) { (void)name; }
char* io_errno_message_raw(void) { return NULL; }
_tuple_int_string_io io_fd_open_read_tuple(const char* p) { (void)p;  _tuple_int_string_io t; t._0 = -1; t._1 = "filesystem disabled"; return t; }
_tuple_int_string_io io_fd_open_write_tuple(const char* p) { (void)p; _tuple_int_string_io t; t._0 = -1; t._1 = "filesystem disabled"; return t; }
const char* io_fd_close_raw(int fd) { (void)fd; return "filesystem disabled"; }
int io_fd_write_n(int fd, const char* d, int n) { (void)fd; (void)d; (void)n; return -1; }
_tuple_ptrintstr_io io_fd_read_n_tuple(int fd, int n) { (void)fd; (void)n; _tuple_ptrintstr_io t; t._0 = NULL; t._1 = 0; t._2 = "filesystem disabled"; return t; }
int io_fd_read_into_raw(int fd, void* buf, int length) { (void)fd; (void)buf; (void)length; return -1; }
_tuple_ptrstr_io   io_fd_read_line_tuple(int fd) { (void)fd; _tuple_ptrstr_io t; t._0 = NULL; t._1 = "filesystem disabled"; return t; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>       // _write, _read, _open, _close
#include <fcntl.h>    // _O_*
#include <sys/types.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef stat
#define stat _stat
#endif
#define AE_FD_WRITE(fd, buf, n) _write((fd), (buf), (unsigned int)(n))
#define AE_FD_READ(fd, buf, n)  _read ((fd), (buf), (unsigned int)(n))
#define AE_FD_CLOSE(fd)         _close((fd))
#define AE_O_RDONLY   _O_RDONLY
#define AE_O_WRONLY   _O_WRONLY
#define AE_O_CREAT    _O_CREAT
#define AE_O_TRUNC    _O_TRUNC
#define AE_O_BINARY   _O_BINARY
static int ae_open_for_read (const char* p) { return _open(p, AE_O_RDONLY | AE_O_BINARY); }
static int ae_open_for_write(const char* p) { return _open(p, AE_O_WRONLY | AE_O_CREAT | AE_O_TRUNC | AE_O_BINARY, _S_IREAD | _S_IWRITE); }
#else
#include <unistd.h>   // write, read, close
#include <fcntl.h>    // open, O_*
#define AE_FD_WRITE(fd, buf, n) write((fd), (buf), (size_t)(n))
#define AE_FD_READ(fd, buf, n)  read ((fd), (buf), (size_t)(n))
#define AE_FD_CLOSE(fd)         close((fd))
static int ae_open_for_read (const char* p) { return open(p, O_RDONLY); }
static int ae_open_for_write(const char* p) { return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
#endif

#include "../string/aether_string.h"  // string_new_with_length / string_empty

// Console I/O
void io_print(const char* str) {
    if (str) {
        printf("%s", str);
        fflush(stdout);
    }
}

void io_print_line(const char* str) {
    if (str) {
        printf("%s\n", str);
    } else {
        printf("\n");
    }
    fflush(stdout);
}

void io_print_int(int value) {
    printf("%d\n", value);
    fflush(stdout);
}

void io_print_float(double value) {
    printf("%g\n", value);
    fflush(stdout);
}

// File I/O
/* Grow-and-read an open stream to EOF into a caller-owned, NUL-terminated
 * buffer. Fallback for when the size-based path can't work: `/proc` and `/sys`
 * seq-files report size 0 from ftell, pipes/sockets aren't seekable — the
 * size-based path would silently return an empty string (#1116). Returns NULL
 * on OOM or a read error. Cap-aware; caller frees with plain libc free. */
static char* io_read_stream_to_eof(FILE* fp) {
    size_t cap = 65536;
    size_t len = 0;
    char* buffer = (char*)aether_caps_malloc(cap);
    if (!buffer) return NULL;
    for (;;) {
        if (len + 4096 > cap) {
            size_t new_cap = cap * 2;
            char* grown = (char*)aether_caps_realloc(buffer, cap, new_cap);
            if (!grown) { aether_caps_free(buffer, cap); return NULL; }
            buffer = grown;
            cap = new_cap;
        }
        size_t n = fread(buffer + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0) break;
    }
    if (ferror(fp)) { aether_caps_free(buffer, cap); return NULL; }
    buffer[len] = '\0';
    return buffer;
}

char* io_read_file_raw(const char* path) {
    if (!path) return NULL;

    FILE* file = fopen(path, "rb");
    if (!file) return NULL;

    /* Fast path for regular, seekable files: size the buffer and read once.
     * Fall through to the EOF loop when the file isn't seekable (pipe/socket)
     * or reports size 0 (any /proc or /sys seq-file) — the old code returned
     * an empty string with no error there (#1116, the /proc silent-truncation
     * bug). Cap-aware (#343): OS-supplied size is gated; caller frees with
     * plain libc free() per the caller-owned-return contract. */
    char* buffer = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size > 0 && fseek(file, 0, SEEK_SET) == 0) {
            buffer = (char*)aether_caps_malloc((size_t)size + 1);
            if (!buffer) { fclose(file); return NULL; }
            size_t read = fread(buffer, 1, (size_t)size, file);
            buffer[read] = '\0';
            fclose(file);
            return buffer;
        }
        fseek(file, 0, SEEK_SET);   /* best-effort rewind before streaming */
    }
    buffer = io_read_stream_to_eof(file);
    fclose(file);
    return buffer;
}

int io_write_file_raw(const char* path, const char* content) {
    if (!path || !content) return 0;

    FILE* file = fopen(path, "wb");
    if (!file) return 0;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, file);
    fclose(file);

    return written == len ? 1 : 0;
}

int io_append_file_raw(const char* path, const char* content) {
    if (!path || !content) return 0;

    FILE* file = fopen(path, "ab");
    if (!file) return 0;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, file);
    fclose(file);

    return written == len ? 1 : 0;
}

int io_file_exists(const char* path) {
    if (!path) return 0;

    FILE* file = fopen(path, "r");
    if (file) {
        fclose(file);
        return 1;
    }
    return 0;
}

int io_delete_file_raw(const char* path) {
    if (!path) return 0;
    return remove(path) == 0 ? 1 : 0;
}

// File info
FileInfo* io_file_info_raw(const char* path) {
    if (!path) return NULL;

    struct stat st;
    if (stat(path, &st) != 0) return NULL;

    /* Cap-aware (#462): internal-lifetime struct (freed by
     * io_file_info_free), so the matching free knows the exact size. */
    FileInfo* info = (FileInfo*)aether_caps_malloc(sizeof(FileInfo));
    if (!info) return NULL;
    info->size = st.st_size;
    info->is_directory = S_ISDIR(st.st_mode) ? 1 : 0;
    info->modified_time = (long)st.st_mtime;
    return info;
}

void io_file_info_free(FileInfo* info) {
    if (info) aether_caps_free(info, sizeof(FileInfo));
}

// Environment variables
char* io_getenv(const char* name) {
    if (!name) return NULL;
    const char* value = getenv(name);
    if (!value) return NULL;
    /* Cap-aware (#462): gate the allocation against the memory cap, same
     * caller-owned-return contract as file_read_all_raw above — the
     * caller frees with plain libc free(), so the counter drifts up on
     * this cold path. The gate is fail-safe: a plugin accumulating env
     * copies is denied once it would cross the cap, never allowed past. */
    size_t n = strlen(value);
    char* out = (char*)aether_caps_malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, value, n + 1);
    return out;
}

int io_setenv_raw(const char* name, const char* value) {
    if (!name || !value) return 0;
#ifdef _WIN32
    // Windows uses _putenv_s
    return _putenv_s(name, value) == 0 ? 1 : 0;
#else
    // POSIX setenv (1 = overwrite existing)
    return setenv(name, value, 1) == 0 ? 1 : 0;
#endif
}

int io_unsetenv_raw(const char* name) {
    if (!name) return 0;
#ifdef _WIN32
    return _putenv_s(name, "") == 0 ? 1 : 0;
#else
    return unsetenv(name) == 0 ? 1 : 0;
#endif
}

/* Loop write to fd until all bytes are out, retrying on EINTR.
 * Returns total bytes written, or -1 on a non-EINTR error. */
static int fd_write_all(int fd, const char* data, int length) {
    if (!data) return -1;
    if (length <= 0) return 0;
    int total = 0;
    while (total < length) {
#ifdef _WIN32
        int w = AE_FD_WRITE(fd, data + total, length - total);
#else
        long w = AE_FD_WRITE(fd, data + total, length - total);
#endif
        if (w < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        total += (int)w;
    }
    return total;
}

int io_stderr_write_raw(const char* data, int length) {
    /* fflush stdio's buffer first so interleaved println / fprintf
     * output isn't reordered around the unbuffered write. */
    fflush(stderr);
    return fd_write_all(2, data, length);
}

int io_stdout_write_raw(const char* data, int length) {
    fflush(stdout);
    return fd_write_all(1, data, length);
}

/* perror(3): print "name: <errno message>\n" to stderr. Must be
 * called immediately after the failing libc call, while errno is
 * still live — same constraint as C's perror. */
void io_perror_raw(const char* name) {
    fflush(stderr);
    perror(name && *name ? name : "error");
}

/* strerror(errno) for the *current* errno, as a heap string the
 * caller owns. Same "call right after the failure" constraint.
 * Returns a fresh copy so the static strerror buffer can't be
 * clobbered under the caller. NULL only on OOM. */
char* io_errno_message_raw(void) {
    const char* m = strerror(errno);
    if (!m) m = "unknown error";
    size_t n = strlen(m);
    /* Cap-aware (#462): gate via the resource cap, caller-owned-return
     * contract (libc free, fail-safe upward drift) — same as io_getenv. */
    char* out = (char*)aether_caps_malloc(n + 1);
    if (out) memcpy(out, m, n + 1);
    return out;
}

/* ─── Full fd surface (Section 1 of nuther-ask-of-aether-team.md) ──
 * Lifecycle: open_read / open_write return (fd, err); close returns
 * "" or err. The bulk-read variant (fd_read_n) returns (bytes, count,
 * err) — bytes is a refcounted AetherString carrying explicit length
 * so embedded NULs survive. fd_read_line strips a trailing '\n' (and
 * a '\r' before it for CRLF input). All read/write loops handle
 * EINTR + partial transfers.
 *
 * fd_write_n returns 0 success / -1 failure (matches the existing
 * 0/-1 convention used by io_setenv_raw etc.). The looped-write
 * helper fd_write_all above already handled this; we expose it
 * under a more discoverable name plus the standardised return code.
 * ────────────────────────────────────────────────────────────────── */

_tuple_int_string_io io_fd_open_read_tuple(const char* path) {
    _tuple_int_string_io out;
    if (!path) { out._0 = -1; out._1 = "null path"; return out; }
    int fd = ae_open_for_read(path);
    if (fd < 0) {
        out._0 = -1;
        out._1 = "cannot open for read";
        return out;
    }
    out._0 = fd;
    out._1 = "";
    return out;
}

_tuple_int_string_io io_fd_open_write_tuple(const char* path) {
    _tuple_int_string_io out;
    if (!path) { out._0 = -1; out._1 = "null path"; return out; }
    int fd = ae_open_for_write(path);
    if (fd < 0) {
        out._0 = -1;
        out._1 = "cannot open for write";
        return out;
    }
    out._0 = fd;
    out._1 = "";
    return out;
}

const char* io_fd_close_raw(int fd) {
    if (fd < 0) return "invalid fd";
    /* Retry close on EINTR is platform-dependent: Linux says you must
     * not retry (the descriptor is already gone); BSD/macOS says you
     * may. Single attempt is portable and matches stdlib's general
     * stance. */
    if (AE_FD_CLOSE(fd) != 0) return "close failed";
    return "";
}

int io_fd_write_n(int fd, const char* data, int length) {
    if (fd < 0) return -1;
    int n = fd_write_all(fd, data, length);
    return n < 0 ? -1 : 0;
}

_tuple_ptrintstr_io io_fd_read_n_tuple(int fd, int n) {
    _tuple_ptrintstr_io out;
    out._0 = (void*)string_empty();
    out._1 = 0;
    out._2 = "";
    if (fd < 0) { out._2 = "invalid fd"; return out; }
    if (n <= 0)   return out;  /* empty read is a no-op */

    /* Cap-aware (#343): `n` is caller-supplied (untrusted in plugin-
     * host scenarios) — gate the allocation. Buffer is freed inside
     * this function via the matched aether_caps_free path; the cap
     * counter stays at current-usage. */
    size_t buf_cap = (size_t)n;
    char* buf = (char*)aether_caps_malloc(buf_cap);
    if (!buf) { out._2 = "out of memory"; return out; }

    int total = 0;
    while (total < n) {
#ifdef _WIN32
        int r = AE_FD_READ(fd, buf + total, n - total);
#else
        long r = AE_FD_READ(fd, buf + total, n - total);
#endif
        if (r == 0) break;          /* EOF */
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            aether_caps_free(buf, buf_cap);
            out._2 = "read failed";
            return out;
        }
        total += (int)r;
    }

    /* Replace the placeholder empty AetherString with one carrying
     * the actual bytes (binary-safe via explicit length). */
    AetherString* s = string_new_with_length(buf, (size_t)total);
    aether_caps_free(buf, buf_cap);
    if (!s) { out._2 = "out of memory"; return out; }
    /* Drop the placeholder — string_empty() returned a refcount=1
     * object we'd otherwise leak. */
    string_release(out._0);
    out._0 = (void*)s;
    out._1 = total;
    return out;
}

/* Zero-allocation sibling of io_fd_read_n_tuple: read into a CALLER-owned
 * buffer instead of minting a fresh AetherString per call.
 *
 * Why it exists (#1471): the streaming-decode case reads one video frame per
 * iteration. At 1080p a frame is ~8 MB, so allocating a fresh string per frame
 * at 30fps is ~250 MB/s of pure allocator churn that a reused buffer avoids
 * entirely. `fs.pread_into` is the same shape for files; this is the fd
 * equivalent, so a caller streaming from a pipe gets the same option as one
 * reading a file.
 *
 * Returns the byte count, or -1 on error. Like read(2) and unlike
 * io_fd_read_n_tuple, this returns as soon as ANY bytes are available rather
 * than looping to fill the buffer — that is what makes it usable on a live
 * pipe, where waiting to fill would stall until the producer sent a full
 * buffer's worth. n == 0 means EOF; 0 < n < length is a normal short read, not
 * an error. EINTR is retried.
 *
 * The buffer is caller-owned (std.bytes) and its capacity is trusted the same
 * way fs_pread_into_raw trusts its own — the Aether wrapper is the guard. */
int io_fd_read_into_raw(int fd, void* buf, int length) {
    if (fd < 0 || !buf || length <= 0) return -1;
    for (;;) {
#ifdef _WIN32
        int r = AE_FD_READ(fd, (char*)buf, length);
#else
        long r = AE_FD_READ(fd, (char*)buf, length);
#endif
        if (r >= 0) return (int)r;   /* 0 == EOF */
#ifndef _WIN32
        if (errno == EINTR) continue;
#endif
        return -1;
    }
}

_tuple_ptrstr_io io_fd_read_line_tuple(int fd) {
    _tuple_ptrstr_io out;
    out._0 = (void*)string_empty();
    out._1 = "";
    if (fd < 0) { out._1 = "invalid fd"; return out; }

    /* Grow-on-demand line buffer. Initial 256 bytes covers the common
     * case (svn dump record lines are short); we double when full.
     * Cap-aware (#343): line length is unbounded from the wire; gate
     * each grow. Frees go through aether_caps_free with the most-
     * recent capacity so the counter tracks current-usage. */
    size_t cap = 256;
    char* buf = (char*)aether_caps_malloc(cap);
    if (!buf) { out._1 = "out of memory"; return out; }
    size_t len = 0;

    for (;;) {
        char c;
#ifdef _WIN32
        int r = AE_FD_READ(fd, &c, 1);
#else
        long r = AE_FD_READ(fd, &c, 1);
#endif
        if (r == 0) {
            /* Clean EOF. If we haven't read anything yet, return the
             * empty-string / empty-error pair (the porter doc spells
             * this out as the EOF signal). Otherwise return the
             * partial line — server-side dump streams sometimes end
             * without a trailing '\n' on the last record. */
            if (len == 0) {
                aether_caps_free(buf, cap);
                return out;  /* both fields already "" */
            }
            break;
        }
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            aether_caps_free(buf, cap);
            out._1 = "read failed";
            return out;
        }
        if (c == '\n') break;
        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char* nb = (char*)aether_caps_realloc(buf, cap, new_cap);
            if (!nb) { aether_caps_free(buf, cap); out._1 = "out of memory"; return out; }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = c;
    }

    /* Strip a trailing '\r' for CRLF input. */
    if (len > 0 && buf[len - 1] == '\r') len--;

    AetherString* s = string_new_with_length(buf, len);
    aether_caps_free(buf, cap);
    if (!s) { out._1 = "out of memory"; return out; }
    string_release(out._0);
    out._0 = (void*)s;
    return out;
}

#endif // AETHER_HAS_FILESYSTEM

