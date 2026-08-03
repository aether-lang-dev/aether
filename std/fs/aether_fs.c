#include "aether_fs.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/utils/aether_compiler.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"
#include "../bytes/aether_bytes.h"   /* fs_pread_into_raw fills a caller's bytes buffer (#1102) */

#if !AETHER_HAS_FILESYSTEM
// Stubs when filesystem is unavailable (WASM, embedded)
File* file_open_raw(const char* p, const char* m) { (void)p; (void)m; return NULL; }
char* file_read_all_raw(File* f) { (void)f; return NULL; }
int file_write_raw(File* f, const char* d, int l) { (void)f; (void)d; (void)l; return 0; }
int file_close(File* f) { (void)f; return 0; }
int file_fd_raw(File* f) { (void)f; return -1; }
int file_exists(const char* p) { (void)p; return 0; }
int fs_path_exists(const char* p) { (void)p; return 0; }
int file_delete_raw(const char* p) { (void)p; return 0; }
int64_t file_size_raw(const char* p) { (void)p; return -1; }
int64_t file_mtime(const char* p) { (void)p; return 0; }
int dir_exists(const char* p) { (void)p; return 0; }
int dir_create_raw(const char* p) { (void)p; return 0; }
int dir_delete_raw(const char* p) { (void)p; return 0; }
int fs_mkdir_p_raw(const char* p) { (void)p; return 0; }
int fs_symlink_raw(const char* t, const char* l) { (void)t; (void)l; return 0; }
char* fs_readlink_raw(const char* p) { (void)p; return NULL; }
int fs_is_symlink(const char* p) { (void)p; return 0; }
int fs_is_socket(const char* p) { (void)p; return 0; }
int fs_unlink_raw(const char* p) { (void)p; return 0; }
int fs_write_binary_raw(const char* p, const char* d, int l) {
    (void)p; (void)d; (void)l; return 0;
}
int fs_write_atomic_raw(const char* p, const char* d, int l) {
    (void)p; (void)d; (void)l; return 0;
}
int fs_rename_raw(const char* f, const char* t) { (void)f; (void)t; return 0; }
int fs_stat_raw(const char* p, int* k, int64_t* s, int64_t* m) {
    (void)p;
    if (k) *k = 0; if (s) *s = 0; if (m) *m = 0;
    return 0;
}
int fs_try_stat(const char* p) { (void)p; return 0; }
int     fs_get_stat_kind(void)  { return 0; }
int64_t fs_get_stat_size(void)  { return 0; }
int64_t fs_get_stat_mtime(void) { return 0; }
int fs_try_statvfs(const char* p) { (void)p; return 0; }
int64_t fs_get_statvfs_total(void) { return 0; }
int64_t fs_get_statvfs_free(void)  { return 0; }
int64_t fs_get_statvfs_avail(void) { return 0; }
int fs_try_mounts(void) { return -1; }
int fs_get_mount_count(void) { return 0; }
const char* fs_get_mount_source(int i)  { (void)i; return ""; }
const char* fs_get_mount_point(int i)   { (void)i; return ""; }
const char* fs_get_mount_fstype(int i)  { (void)i; return ""; }
const char* fs_get_mount_options(int i) { (void)i; return ""; }
void fs_release_mounts(void) {}
int fs_try_block_info(const char* d) { (void)d; return 0; }
int64_t fs_get_block_size_bytes(void) { return 0; }
int fs_get_block_removable(void) { return -1; }
const char* fs_get_block_transport(void) { return ""; }
char* fs_read_binary_raw(const char* p, int* n) {
    (void)p; if (n) *n = 0; return NULL;
}
int fs_try_read_binary(const char* p) { (void)p; return 0; }
const char* fs_get_read_binary(void) { return NULL; }
int fs_get_read_binary_length(void) { return 0; }
void fs_release_read_binary(void) {}
typedef struct { void* _0; int _1; const char* _2; } _tuple_ptr_int_string;
_tuple_ptr_int_string fs_read_binary_tuple(const char* p) {
    (void)p; _tuple_ptr_int_string out = { NULL, 0, "fs unavailable" }; return out;
}
typedef struct { int _0; int _1; const char* _2; } _tuple_int_int_string;
_tuple_int_int_string fs_copy_raw(const char* s, const char* d) {
    (void)s; (void)d;
    _tuple_int_int_string out = { 0, AETHER_FS_KIND_UNAVAILABLE, "fs unavailable" };
    return out;
}
_tuple_int_int_string fs_move_raw(const char* s, const char* d) {
    (void)s; (void)d;
    _tuple_int_int_string out = { 0, AETHER_FS_KIND_UNAVAILABLE, "fs unavailable" };
    return out;
}
_tuple_int_int_string fs_chmod_raw(const char* p, int m) {
    (void)p; (void)m;
    _tuple_int_int_string out = { 0, AETHER_FS_KIND_UNAVAILABLE, "fs unavailable" };
    return out;
}
typedef struct { const char* _0; int _1; const char* _2; } _tuple_string_int_string;
_tuple_string_int_string fs_realpath_raw(const char* p) {
    (void)p;
    _tuple_string_int_string out = { "", AETHER_FS_KIND_UNAVAILABLE, "fs unavailable" };
    return out;
}
char* path_join(const char* a, const char* b) { (void)a; (void)b; return NULL; }
char* path_dirname(const char* p) { (void)p; return NULL; }
char* path_basename(const char* p) { (void)p; return NULL; }
char* path_extension(const char* p) { (void)p; return NULL; }
int path_is_absolute(const char* p) { (void)p; return 0; }
const char* path_separator(void) {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}
char* path_clean(const char* p) { (void)p; return NULL; }
int path_is_within_base(const char* base, const char* target) { (void)base; (void)target; return 0; }
char* path_rel(const char* base, const char* target) { (void)base; (void)target; return NULL; }
int64_t fs_pwrite_raw(File* f, const char* d, int l, int64_t o) { (void)f; (void)d; (void)l; (void)o; return -1; }
int fs_pread_raw(File* f, int l, int64_t o) { (void)f; (void)l; (void)o; return 0; }
int fs_pread_into_raw(File* f, AetherBytes* b, int l, int64_t o) { (void)f; (void)b; (void)l; (void)o; return -1; }
const char* fs_get_pread(void) { return ""; }
int fs_get_pread_length(void) { return 0; }
void fs_release_pread(void) {}
const char* fs_ftruncate_raw(File* f, int64_t l) { (void)f; (void)l; return "fs unavailable"; }
const char* fs_fsync_raw(File* f) { (void)f; return "fs unavailable"; }
DirList* dir_list_raw(const char* p) { (void)p; return NULL; }
int dir_list_count(DirList* l) { (void)l; return 0; }
const char* dir_list_get(DirList* l, int i) { (void)l; (void)i; return NULL; }
int dir_list_kind(DirList* l, int i) { (void)l; (void)i; return 0; }
void dir_list_free(DirList* l) { (void)l; }
DirList* fs_glob_raw(const char* p) { (void)p; return NULL; }
DirList* fs_glob_multi_raw(void* l) { (void)l; return NULL; }
int fs_walk_raw(const char* r, void* c) { (void)r; (void)c; return -1; }
void* fs_watch_open_raw(const char* p) { (void)p; return NULL; }
int fs_watch_wait(void* w, int t) { (void)w; (void)t; return -1; }
void fs_watch_close(void* w) { (void)w; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>            // open() / O_CREAT / O_EXCL / O_NOFOLLOW
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/statvfs.h>     // statvfs() for fs_try_statvfs (#1117)
#endif
#include <stddef.h>           // offsetof for the mount-table getters (#1118)
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>        // getmntinfo() for fs_try_mounts
#endif

#ifdef _WIN32
    #include <direct.h>
    #include <io.h>            // _open / _unlink for atomic-write + delete
    #include <process.h>       // _getpid (for fs_write_atomic_raw tmp path)
    #include <windows.h>
    #define mkdir(path, mode) _mkdir(path)
    #define rmdir _rmdir
    #define stat _stat
    #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
    // Windows stat result has no S_ISREG / S_ISLNK macros; emulate the
    // former from the mode bits. S_ISLNK stays undefined here because
    // the lstat branch in fs_stat_raw is guarded by #ifndef _WIN32.
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
    #endif
#else
    #include <dirent.h>
    #include <unistd.h>
    /* Per-platform zero-copy primitives for fs.copy.
     * Linux  : sys/sendfile.h + SYS_copy_file_range via syscall (no
     *          _GNU_SOURCE needed; avoids the feature-test macro
     *          dance for the whole TU).
     * macOS  : <copyfile.h> for fcopyfile(COPYFILE_DATA) — APFS clone
     *          on same-volume copies, kernel-level block copy
     *          otherwise. Always works since macOS 10.12.
     * Other  : pure POSIX read/write fallback (BSDs, illumos, etc.).
     */
    #if defined(__linux__)
        #include <sys/sendfile.h>
        #include <sys/syscall.h>
    #elif defined(__APPLE__)
        #include <copyfile.h>
    #endif
    #include <limits.h>           // INT_MAX (saturate the byte count)
#endif

// Unwrap the payload+length from a value that may be either an
// AetherString* (from fs.read_binary, string_new_with_length, etc.)
// or a plain C string literal. Extern fn signatures say `const char*`
// but Aether passes whichever pointer the variable holds — without
// this dispatch, AetherString inputs end up writing the struct
// header (magic 0xAE57C0DE, refcount, length, capacity, data-ptr)
// to disk instead of the intended bytes. When `explicit_len` is
// non-negative the caller's length wins (used by write_binary and
// write_atomic, which take an explicit-length param for binary safety).
static inline const char* fs_unwrap_bytes(const char* data, int explicit_len, size_t* out_len) {
    if (!data) { *out_len = 0; return NULL; }
    if (is_aether_string(data)) {
        const AetherString* s = (const AetherString*)data;
        *out_len = (explicit_len >= 0) ? (size_t)explicit_len : s->length;
        return s->data;
    }
    *out_len = (explicit_len >= 0) ? (size_t)explicit_len : strlen(data);
    return data;
}

// File operations
File* file_open_raw(const char* path, const char* mode) {
    if (!path || !mode) return NULL;

    // Sandbox check: determine read vs write from mode
    if (mode[0] == 'r') {
        if (!aether_sandbox_check("fs_read", path)) return NULL;
    } else {
        if (!aether_sandbox_check("fs_write", path)) return NULL;
    }

    FILE* fp = fopen(path, mode);
    if (!fp) return NULL;

    File* file = (File*)aether_caps_malloc(sizeof(File));
    if (!file) { fclose(fp); return NULL; }
    file->handle = fp;
    file->is_open = 1;
    /* #462: cap-account the retained path copy — a sandboxed caller can
     * craft an enormous filename to inflate filesystem-driven memory.
     * Freed with the matching length in file_close. */
    size_t plen = strlen(path) + 1;
    char* pcopy = (char*)aether_caps_malloc(plen);
    if (pcopy) memcpy(pcopy, path, plen);
    file->path = pcopy;
    return file;
}

/* Grow-and-read an open stream to EOF into a caller-owned, NUL-terminated
 * buffer. Used as the fallback when the fast size-based path can't work:
 * `/proc` and `/sys` seq-files report size 0 from ftell, and pipes/sockets
 * aren't seekable at all — the size-based path would silently return an empty
 * string. Returns NULL on OOM. Cap-aware via aether_caps_realloc; the caller
 * frees the result with plain libc free per the caller-owned-return contract
 * (#1116). */
static char* read_stream_to_eof(FILE* fp) {
    size_t cap = 65536;   /* one page-ish chunk; grows as needed */
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
        if (n == 0) break;   /* EOF or error; ferror check below */
    }
    if (ferror(fp)) { aether_caps_free(buffer, cap); return NULL; }
    buffer[len] = '\0';
    return buffer;
}

char* file_read_all_raw(File* file) {
    if (!file || !file->is_open) return NULL;

    FILE* fp = (FILE*)file->handle;

    /* Fast path for regular, seekable files: size the buffer from the file
     * length and read once. Fall through to the EOF loop when the file isn't
     * seekable (pipe/socket) or reports size 0 (any /proc or /sys seq-file) —
     * the old code returned an empty string with no error in those cases
     * (#1116, the /proc/self/mountinfo silent-truncation bug). */
    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            /* Cap-aware (#343): file size is OS-supplied and unbounded.
             * Caller frees with plain libc free per the caller-owned-return
             * contract — counter drifts up on this path, same as
             * string_concat. */
            char* buffer = (char*)aether_caps_malloc((size_t)size + 1);
            if (!buffer) return NULL;
            size_t read = fread(buffer, 1, (size_t)size, fp);
            buffer[read] = '\0';
            return buffer;
        }
        /* size <= 0 or re-seek failed: rewind (best-effort) and stream. */
        fseek(fp, 0, SEEK_SET);
    }
    return read_stream_to_eof(fp);
}

int file_write_raw(File* file, const char* data, int length) {
    if (!file || !file->is_open || !data) return 0;

    FILE* fp = (FILE*)file->handle;
    size_t written = fwrite(data, 1, (size_t)length, fp);
    return (written == (size_t)length) ? 1 : 0;
}

int file_close(File* file) {
    if (!file) return 0;

    if (file->is_open) {
        fclose((FILE*)file->handle);
        file->is_open = 0;
    }

    if (file->path) aether_caps_free((void*)file->path, strlen(file->path) + 1);
    aether_caps_free(file, sizeof(File));
    return 1;
}

/* ===================================================================
 * #640 — Positional binary-safe I/O: pwrite, pread, ftruncate, fsync.
 *
 * The POSIX positional family — required for any out-of-order writer
 * (rsync-style reconstruction, sparse-file build, parallel chunk
 * downloads, block scatter-gather). Wraps fileno(fp) on the FILE*
 * inside our File wrapper.
 *
 * `pwrite` and `pread` loop on short transfers — POSIX permits
 * returning fewer bytes than requested. EINTR retry is required;
 * a syscall interrupted by a signal returns -1 with errno=EINTR
 * and must be re-tried, not failed.
 *
 * fflush(fp) before pread/pwrite: the FILE* may have buffered data
 * not yet flushed to the fd. Without the flush, a pread of recently-
 * written data via the same handle reads stale-from-fd content.
 * =================================================================== */

#include <errno.h>

/* Returns bytes written on success, -1 on error. Loops on short
 * writes. `offset` is a byte position from start-of-file.
 * int64_t (not C `long`) so the definition matches the prototype the
 * compiler emits for an Aether `long` extern on LLP64 (#1021). */
int64_t fs_pwrite_raw(File* file, const char* data, int length, int64_t offset) {
    if (!file || !file->is_open || !data || length < 0 || offset < 0) return -1;
    FILE* fp = (FILE*)file->handle;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total < (size_t)length) {
#ifdef _WIN32
        /* Windows has no pwrite — emulate with seek + write. NOT
         * thread-safe across handles (the seek changes the file
         * position for the whole handle); fine for the single-
         * threaded port-server case the issue addresses. _fseeki64
         * because plain fseek takes a 32-bit long on Windows. */
        if (_fseeki64(fp, offset + (int64_t)total, SEEK_SET) != 0) return -1;
        size_t w = fwrite(data + total, 1, (size_t)length - total, fp);
        if (w == 0) return -1;
        total += w;
#else
        ssize_t w = pwrite(fd, data + total, (size_t)length - total,
                           (off_t)(offset + (int64_t)total));
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) break;
        total += (size_t)w;
#endif
    }
    return (int64_t)total;
}

/* TLS slot for the pread split-accessor — mirrors fs_get_read_binary.
 * Per-thread so concurrent positional reads on different threads
 * don't clobber. Lifetime is until the next call on the same thread
 * or an explicit release. */
static __thread unsigned char* g_pread_buf = NULL;
static __thread size_t         g_pread_cap = 0;
static __thread int            g_pread_len = 0;

static void release_pread_locked(void) {
    if (g_pread_buf) aether_caps_free(g_pread_buf, g_pread_cap);
    g_pread_buf = NULL;
    g_pread_cap = 0;
    g_pread_len = 0;
}

void fs_release_pread(void) {
    release_pread_locked();
}

const char* fs_get_pread(void) {
    return g_pread_buf ? (const char*)g_pread_buf : "";
}

int fs_get_pread_length(void) {
    return g_pread_len;
}

/* Returns 1 on success (TLS slot has up-to-length bytes; partial
 * EOF reads are still success), 0 on failure. */
int fs_pread_raw(File* file, int length, int64_t offset) {
    release_pread_locked();
    if (!file || !file->is_open || length < 0 || offset < 0) return 0;
    FILE* fp = (FILE*)file->handle;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return 0;

    size_t alloc = length > 0 ? (size_t)length : 1;
    unsigned char* buf = (unsigned char*)aether_caps_malloc(alloc);
    if (!buf) return 0;

    size_t total = 0;
    while (total < (size_t)length) {
#ifdef _WIN32
        if (_fseeki64(fp, offset + (int64_t)total, SEEK_SET) != 0) {
            aether_caps_free(buf, alloc); return 0;
        }
        size_t r = fread(buf + total, 1, (size_t)length - total, fp);
        if (r == 0) break;  /* EOF or error — return what we have. */
        total += r;
#else
        ssize_t r = pread(fd, buf + total, (size_t)length - total,
                          (off_t)(offset + (int64_t)total));
        if (r < 0) {
            if (errno == EINTR) continue;
            aether_caps_free(buf, alloc); return 0;
        }
        if (r == 0) break;  /* EOF — short read is success per the issue's contract. */
        total += (size_t)r;
#endif
    }
    g_pread_buf = buf;
    g_pread_cap = alloc;
    g_pread_len = (int)total;
    return 1;
}

/* Zero-copy sibling of fs_pread_raw (#1102): read up to `length` bytes at
 * `offset` directly into the caller's `buf` storage, clamped to the
 * buffer's capacity, then publish the count as its logical length. A
 * fixed-size block reader can reuse one buffer across a loop instead of
 * allocating a fresh string per block. Returns the byte count (>= 0; 0 =
 * EOF, 0 < n < length = short read), or -1 on invalid args / I/O error;
 * on error the buffer is left empty. */
int fs_pread_into_raw(File* file, AetherBytes* buf, int length, int64_t offset) {
    if (!buf) return -1;
    if (!file || !file->is_open || length < 0 || offset < 0) {
        aether_bytes_set_length(buf, 0);
        return -1;
    }
    int cap = aether_bytes_capacity(buf);
    if (cap < 0) { return -1; }
    if (length > cap) length = cap;          /* clamp to buffer capacity */
    aether_bytes_set_length(buf, 0);         /* empty until success publishes the count */
    if (length == 0) return 0;

    unsigned char* dst = (unsigned char*)aether_bytes_data(buf);
    if (!dst) return -1;

    FILE* fp = (FILE*)file->handle;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < (size_t)length) {
#ifdef _WIN32
        if (_fseeki64(fp, offset + (int64_t)total, SEEK_SET) != 0) return -1;
        size_t r = fread(dst + total, 1, (size_t)length - total, fp);
        if (r == 0) break;  /* EOF or error: return what we have (short read is success). */
        total += r;
#else
        ssize_t r = pread(fd, dst + total, (size_t)length - total,
                          (off_t)(offset + (int64_t)total));
        if (r < 0) {
            if (errno == EINTR) continue;
            aether_bytes_set_length(buf, 0);
            return -1;
        }
        if (r == 0) break;  /* EOF: short read is success per the contract. */
        total += (size_t)r;
#endif
    }
    aether_bytes_set_length(buf, (int)total);
    return (int)total;
}

/* ftruncate — set file length to `length` bytes. POSIX truncate(2)
 * extends with zero bytes when length > current size; SHOULD work
 * the same way on Windows via _chsize_s. Returns "" on success or
 * an error message. */
const char* fs_ftruncate_raw(File* file, int64_t length) {
    if (!file || !file->is_open || length < 0) return "invalid args";
    FILE* fp = (FILE*)file->handle;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return "no fd";
#ifdef _WIN32
    if (_chsize_s(fd, length) != 0) return "ftruncate failed";
#else
    if (ftruncate(fd, (off_t)length) != 0) return "ftruncate failed";
#endif
    return "";
}

/* fsync — flush kernel buffers to disk. Required after pwrite for
 * durability guarantees. POSIX fsync(2) / Windows _commit. */
/* Expose the OS-level descriptor inside an open File (issue #1003).
 * Exists so capability plumbing — capsicum.rights_limit() /
 * fcntls_limit() before capsicum.enter() — can narrow a descriptor the
 * program opened through the stdlib rather than a raw extern. The fd
 * is owned by the handle: callers must not close() it, and it dies
 * with file_close() — which also frees the File itself, so this must
 * never be called on a closed handle (only on null, which returns -1).
 * fflush first so the fd's view matches the FILE*'s buffered state. */
int file_fd_raw(File* file) {
    if (!file || !file->is_open) return -1;
    FILE* fp = (FILE*)file->handle;
    fflush(fp);
    return fileno(fp);
}

const char* fs_fsync_raw(File* file) {
    if (!file || !file->is_open) return "invalid args";
    FILE* fp = (FILE*)file->handle;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return "no fd";
#ifdef _WIN32
    if (_commit(fd) != 0) return "fsync failed";
#else
    if (fsync(fd) != 0) return "fsync failed";
#endif
    return "";
}

int file_exists(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_read", path)) return 0;

    struct stat st;
    return (stat(path, &st) == 0 && !S_ISDIR(st.st_mode));
}

/* Path-agnostic existence check: returns 1 if anything is at `path`
 * (regular file, directory, symlink, fifo, socket, …), 0 otherwise.
 * Distinct from `file_exists` (which is regular-file-only) and
 * `dir_exists` (directory-only) — those are the type-specific
 * predicates. Use `fs_path_exists` when the caller doesn't care
 * what kind of thing is there, only whether the path is bound.
 *
 * Uses lstat so a dangling symlink reports as existing — matches
 * POSIX `test -e` and the conventional "is this path bound?"
 * shell idiom. Empty / NULL path returns 0. */
int fs_path_exists(const char* path) {
    if (!path || !*path) return 0;
    if (!aether_sandbox_check("fs_read", path)) return 0;

#ifdef _WIN32
    /* Windows lacks lstat; stat is fine — symlinks behave like
     * their target on Windows by default. */
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
#else
    struct stat st;
    return lstat(path, &st) == 0 ? 1 : 0;
#endif
}

int file_delete_raw(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_write", path)) return 0;
    return remove(path) == 0 ? 1 : 0;
}

/* 64-bit stat shim (#1021). The size/mtime surfaces below return
 * int64_t — the C spelling of an Aether `long` extern (LLP64-safe;
 * plain C `long` is 32-bit on Windows). On POSIX, stat/lstat already
 * fill a 64-bit off_t/time_t on every platform we build. On Windows
 * the CRT's plain _stat carries a 32-bit st_size, so files >= 2 GiB
 * wrap — _stati64 is the 64-bit-size spelling. (No lstat on the
 * Windows CRT; fs_stat_raw's symlink branch is POSIX-only anyway.) */
#ifdef _WIN32
typedef struct _stati64 aether_stat64_t;
#define aether_stat64(p, st)  _stati64((p), (st))
#define aether_lstat64(p, st) _stati64((p), (st))
#else
typedef struct stat aether_stat64_t;
#define aether_stat64(p, st)  stat((p), (st))
#define aether_lstat64(p, st) lstat((p), (st))
#endif

int64_t file_size_raw(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_read", path)) return 0;

    aether_stat64_t st;
    if (aether_stat64(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int64_t file_mtime(const char* path) {
    if (!path) return 0;

    aether_stat64_t st;
    if (aether_stat64(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

// Like file_mtime but distinguishes "stat failed" (returns -1) from
// "file's mtime happens to be 0" (returns 0 — the Unix epoch). The
// older `file_mtime` collapses both into 0, swallowing the error.
// Aether-side callers should prefer `fs.mtime` (Go-style result tuple).
int64_t file_mtime_raw(const char* path) {
    if (!path) return -1;

    aether_stat64_t st;
    if (aether_stat64(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

// Directory operations
int dir_exists(const char* path) {
    if (!path) return 0;

    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int dir_create_raw(const char* path) {
    if (!path) return 0;
    return mkdir(path, 0755) == 0 ? 1 : 0;
}

// Like dir_create_raw, but with an explicit mode. Lets users create
// private directories (e.g. 0700 for keys) atomically — without the
// mkdir-then-chmod race window the previous "always 0755 + shell out
// to chmod" workaround opened. Mode is masked with 0777 to keep
// callers from accidentally setting bits the kernel would reinterpret.
// Windows `mkdir` ignores mode (Win32 doesn't have POSIX permission
// bits at the directory layer); the parameter is accepted for API
// portability and discarded there.
int dir_create_mode_raw(const char* path, int mode) {
#ifdef _WIN32
    (void)mode;
    if (!path) return 0;
    return _mkdir(path) == 0 ? 1 : 0;
#else
    if (!path) return 0;
    return mkdir(path, (mode_t)(mode & 0777)) == 0 ? 1 : 0;
#endif
}

int dir_delete_raw(const char* path) {
    if (!path) return 0;
    return rmdir(path) == 0 ? 1 : 0;
}

// `mkdir -p` semantics: walk through each '/' in `path`, creating each
// intermediate directory if it doesn't already exist. Treats EEXIST as
// success at every step. Returns 1 on success, 0 on failure (e.g. path
// too long, or one of the components exists but isn't a directory).
//
// Windows note: a Windows-shaped path like `C:/msys64/tmp/foo` is
// passed in by cygpath-m'd MSYS scripts and by anything that builds
// paths from getenv-returned Windows roots. We skip the `C:` drive
// prefix before walking '/' separators so the loop never attempts
// `mkdir("C:")` (which fails — the trailing-backslash form `C:\`
// is the only valid shape, and that's already an existing root).
int fs_mkdir_p_raw(const char* path) {
    if (!path || !*path) return 0;
    if (!aether_sandbox_check("fs_write", path)) return 0;

    char buf[4096];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, path, len + 1);

    // Skip a Windows drive prefix (`X:`). Anything shaped
    // `<letter><colon>` at the start is a drive root that's already
    // present; we begin walking '/' from after it.
    size_t start = 1;
    if (len >= 2 &&
        ((buf[0] >= 'A' && buf[0] <= 'Z') || (buf[0] >= 'a' && buf[0] <= 'z')) &&
        buf[1] == ':') {
        // If a separator follows the colon (`C:/foo` or `C:\foo`),
        // skip past it too — the drive root itself doesn't need
        // creating, only the components beneath.
        start = (len >= 3 && (buf[2] == '/' || buf[2] == '\\')) ? 3 : 2;
    }

    // Step through each separator in the interior of the path,
    // creating each prefix as we go. Both '/' and '\' are honoured
    // as separators so paths from cygpath / Windows callers and
    // POSIX paths walk uniformly.
    for (size_t i = start; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0) {
                // Tolerate already-exists. Anything else is a real failure.
                if (errno != EEXIST) return 0;
                struct stat st;
                if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
            }
            buf[i] = saved;
        }
    }
    // Final component (if not already covered by a trailing slash)
    if (mkdir(buf, 0755) != 0) {
        if (errno != EEXIST) return 0;
        struct stat st;
        if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    }
    return 1;
}

#ifndef _WIN32

// Create a symbolic link at `link_path` pointing to `target`. The target
// is recorded verbatim — relative targets stay relative.
int fs_symlink_raw(const char* target, const char* link_path) {
    if (!target || !link_path) return 0;
    if (!aether_sandbox_check("fs_write", link_path)) return 0;
    return symlink(target, link_path) == 0 ? 1 : 0;
}

// Read a symbolic link. Returns the target as a heap-allocated string,
// or NULL if `path` isn't a symlink or can't be read.
char* fs_readlink_raw(const char* path) {
    if (!path) return NULL;
    if (!aether_sandbox_check("fs_read", path)) return NULL;

    char buf[4096];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

// Returns 1 if `path` is a symlink (does NOT follow the link to check
// the target). Returns 0 otherwise — including when the path doesn't
// exist.
int fs_is_symlink(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_read", path)) return 0;

    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISLNK(st.st_mode) ? 1 : 0;
}

// #1368: Returns 1 if `path` is a UNIX-domain socket, else 0 (including when
// the path is missing). Follows symlinks (uses stat, not lstat) — the caller
// asking "is this a socket?" (e.g. a podman/docker socket auto-detect) cares
// about the target. Mirrors fs_is_symlink's shape. POSIX only; the Windows
// stub returns 0.
int fs_is_socket(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_read", path)) return 0;

    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISSOCK(st.st_mode) ? 1 : 0;
}

// Remove a file or symlink. Will NOT remove a directory — use dir_delete
// for that. Returns 1 on success, 0 on failure.
int fs_unlink_raw(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_write", path)) return 0;
    return unlink(path) == 0 ? 1 : 0;
}

#else // _WIN32

// Windows symlinks need elevation or developer mode and use a different
// API surface. For now these are stubs returning failure; a follow-up
// PR can add CreateSymbolicLinkW + a junction fallback for directories.
int fs_symlink_raw(const char* t, const char* l) { (void)t; (void)l; return 0; }
char* fs_readlink_raw(const char* p) { (void)p; return NULL; }
int fs_is_symlink(const char* p) { (void)p; return 0; }
int fs_is_socket(const char* p) { (void)p; return 0; }
int fs_unlink_raw(const char* path) {
    if (!path) return 0;
    if (!aether_sandbox_check("fs_write", path)) return 0;
    return _unlink(path) == 0 ? 1 : 0;
}

#endif // !_WIN32

// ---------------------------------------------------------------------
// Durable/atomic/stat helpers. Cross-platform — rely on fopen + stat +
// rename which exist on both POSIX and Windows CRT. The POSIX-specific
// fsync path is guarded by #ifndef _WIN32; Windows gets a best-effort
// fflush (no FlushFileBuffers call for v1 — good enough for the
// atomicity guarantee, since rename itself is still the durable step).
// ---------------------------------------------------------------------

#include <time.h>

int fs_write_binary_raw(const char* path, const char* data, int length) {
    if (!path || length < 0) return 0;
    if (length > 0 && !data) return 0;
    if (!aether_sandbox_check("fs_write", path)) return 0;

    size_t want;
    const char* bytes = fs_unwrap_bytes(data, length, &want);

    FILE* fp = fopen(path, "wb");
    if (!fp) return 0;

    size_t written = (want > 0) ? fwrite(bytes, 1, want, fp) : 0;
    int fwrite_ok = (written == want);
    int close_ok = (fclose(fp) == 0);

    // Non-atomic: on failure, the caller sees a partial file. This is
    // the explicit contract — use fs_write_atomic_raw when that's not
    // acceptable.
    return (fwrite_ok && close_ok) ? 1 : 0;
}

int fs_write_atomic_raw(const char* path, const char* data, int length) {
    if (!path || length < 0) return 0;
    if (!aether_sandbox_check("fs_write", path)) return 0;

    size_t want;
    const char* bytes = fs_unwrap_bytes(data, length, &want);

    // Build a tmp path <path>.tmp.<pid>.<counter>. The counter keeps
    // concurrent writers from the same PID (unlikely but cheap to
    // guard against) from stomping each other's tmp files.
    static unsigned long s_counter = 0;
    char tmp[4096];
    int plen = (int)strlen(path);
    if (plen <= 0 || plen >= (int)sizeof(tmp) - 32) return 0;
    long pid =
#ifdef _WIN32
        (long)_getpid();
#else
        (long)getpid();
#endif
    unsigned long n = ++s_counter;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%lu", path, pid, n);

    // CVE-class: an attacker who can predict the tmp filename (and
    // both pid + counter are predictable) could plant a symlink at
    // the tmp path before this open and have the subsequent fwrite
    // overwrite the symlink's target. The previous `fopen(tmp, "wb")`
    // followed symlinks happily.
    //
    // Fix: open with O_CREAT | O_EXCL — refuses to open if `tmp`
    // already exists (a symlink, regular file, or any other file
    // type). On POSIX additionally pass O_NOFOLLOW as defence-in-
    // depth: if `tmp` somehow appears as a symlink between the EXCL
    // check and the create, the kernel still refuses to follow.
    //
    // Permissions track what the previous fopen would have produced:
    // 0666 modified by the process umask (so users with a relaxed
    // umask still get group/world write if that's their convention).
#ifdef _WIN32
    int fd = _open(tmp,
                   _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    mode_t um = umask(0);
    umask(um);
    int fd = open(tmp,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                  0666 & ~um);
#endif
    if (fd < 0) return 0;
#ifdef _WIN32
    FILE* fp = _fdopen(fd, "wb");
#else
    FILE* fp = fdopen(fd, "wb");
#endif
    if (!fp) {
#ifdef _WIN32
        _close(fd);
        _unlink(tmp);
#else
        close(fd);
        unlink(tmp);
#endif
        return 0;
    }

    size_t written = (want > 0) ? fwrite(bytes, 1, want, fp) : 0;
    int fwrite_ok = (written == want);

#ifndef _WIN32
    // Flush + fsync before rename. Without fsync a power loss between
    // the rename and the kernel flushing the tmp's data could leave
    // the destination pointing at zero-length or garbage contents.
    int fsync_ok = 1;
    if (fwrite_ok) {
        if (fflush(fp) != 0) fsync_ok = 0;
        else if (fsync(fileno(fp)) != 0) fsync_ok = 0;
    }
#else
    int fsync_ok = fwrite_ok ? (fflush(fp) == 0) : 0;
#endif

    if (fclose(fp) != 0) fsync_ok = 0;

    if (!fwrite_ok || !fsync_ok) {
        remove(tmp);  // don't leak a half-written tmp file
        return 0;
    }

    // rename(2) is atomic on POSIX when src and dst are on the same
    // filesystem — which they always are here, since we put the tmp
    // right next to the destination. On Windows rename will fail if
    // the destination exists, so drop it first; we don't race-check
    // because concurrent writers to the same path is already UB.
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

int fs_rename_raw(const char* from, const char* to) {
    if (!from || !to) return 0;
    if (!aether_sandbox_check("fs_write", from)) return 0;
    if (!aether_sandbox_check("fs_write", to)) return 0;
    return rename(from, to) == 0 ? 1 : 0;
}

// Kind encoding for fs_stat_raw's out_kind:
//   1 = regular file, 2 = directory, 3 = symlink, 4 = other.
// A symlink is reported as kind 3 even if its target is a file or
// directory — lstat(2) never follows. Callers that want "size of
// the target" should readlink + stat the target explicitly.
#define FS_STAT_KIND_FILE    1
#define FS_STAT_KIND_DIR     2
#define FS_STAT_KIND_SYMLINK 3
#define FS_STAT_KIND_OTHER   4
// #1368: sockets, FIFOs and devices previously all collapsed into OTHER (4),
// making an AF_UNIX socket indistinguishable from a FIFO. Give them distinct
// kinds so callers can identify e.g. a podman/docker socket lexically. These
// are POSIX-only (the S_IS* macros below are guarded #ifndef _WIN32); on
// Windows such nodes keep reporting OTHER. Additive — existing callers that
// only test 1..4 are unaffected.
#define FS_STAT_KIND_SOCKET  5
#define FS_STAT_KIND_FIFO    6
#define FS_STAT_KIND_DEVICE  7

int fs_stat_raw(const char* path, int* out_kind,
                int64_t* out_size, int64_t* out_mtime) {
    if (!path) {
        if (out_kind)  *out_kind  = 0;
        if (out_size)  *out_size  = 0;
        if (out_mtime) *out_mtime = 0;
        return 0;
    }
    if (!aether_sandbox_check("fs_read", path)) {
        if (out_kind)  *out_kind  = 0;
        if (out_size)  *out_size  = 0;
        if (out_mtime) *out_mtime = 0;
        return 0;
    }

    aether_stat64_t st;
#ifndef _WIN32
    if (aether_lstat64(path, &st) != 0) {
#else
    // Windows CRT has no lstat; _stati64 follows symlinks, but Windows
    // symlinks already go through a different code path we stub out
    // (fs_is_symlink returns 0). Good enough for v1.
    if (aether_stat64(path, &st) != 0) {
#endif
        if (out_kind)  *out_kind  = 0;
        if (out_size)  *out_size  = 0;
        if (out_mtime) *out_mtime = 0;
        return 0;
    }

    int kind;
#ifndef _WIN32
    if (S_ISLNK(st.st_mode))       kind = FS_STAT_KIND_SYMLINK;
    else
#endif
    if (S_ISREG(st.st_mode))       kind = FS_STAT_KIND_FILE;
    else if (S_ISDIR(st.st_mode))  kind = FS_STAT_KIND_DIR;
#ifndef _WIN32
    // #1368: distinguish sockets / FIFOs / devices (POSIX only; the MSVC
    // stat model has no S_ISSOCK/S_ISFIFO and its S_ISCHR/S_ISBLK cover
    // console/pipe handles, so Windows keeps these as OTHER).
    else if (S_ISSOCK(st.st_mode)) kind = FS_STAT_KIND_SOCKET;
    else if (S_ISFIFO(st.st_mode)) kind = FS_STAT_KIND_FIFO;
    else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) kind = FS_STAT_KIND_DEVICE;
#endif
    else                           kind = FS_STAT_KIND_OTHER;

    if (out_kind)  *out_kind  = kind;
    if (out_size)  *out_size  = (int64_t)st.st_size;
    if (out_mtime) *out_mtime = (int64_t)st.st_mtime;
    return 1;
}

// Thread-local cache for the split fs_try_stat / fs_get_stat_* pair.
// Storing last-stat result here lets Aether callers work without
// allocating C out-params. The trio is called sequentially by the
// Aether-side file_stat wrapper, so the cache only has to survive
// that short window on the calling thread.
#if defined(__GNUC__) || defined(__clang__)
  #define AETHER_FS_TLS __thread
#else
  #define AETHER_FS_TLS
#endif
static AETHER_FS_TLS int     s_last_kind  = 0;
static AETHER_FS_TLS int64_t s_last_size  = 0;
static AETHER_FS_TLS int64_t s_last_mtime = 0;

int fs_try_stat(const char* path) {
    int k = 0;
    int64_t sz = 0, mt = 0;
    int ok = fs_stat_raw(path, &k, &sz, &mt);
    if (!ok) {
        s_last_kind = 0; s_last_size = 0; s_last_mtime = 0;
        return 0;
    }
    s_last_kind = k; s_last_size = sz; s_last_mtime = mt;
    return 1;
}

int     fs_get_stat_kind(void)  { return s_last_kind;  }
int64_t fs_get_stat_size(void)  { return s_last_size;  }
int64_t fs_get_stat_mtime(void) { return s_last_mtime; }

/* statvfs (#1117): exact filesystem byte counts for the filesystem containing
 * `path`. Split try/get pair, same TLS pattern as fs_try_stat, so the Aether
 * wrapper reads three int64 fields without C out-params. total/free/avail are
 * bytes; `avail` is the space usable by an unprivileged process (f_bavail),
 * which is the one callers usually want for "how much can I actually write".
 * Not available on Windows (no statvfs) — stubbed there to return 0/failure. */
static AETHER_FS_TLS int64_t s_vfs_total = 0;
static AETHER_FS_TLS int64_t s_vfs_free  = 0;
static AETHER_FS_TLS int64_t s_vfs_avail = 0;

int fs_try_statvfs(const char* path) {
    s_vfs_total = 0; s_vfs_free = 0; s_vfs_avail = 0;
    if (!path) return 0;
#ifdef _WIN32
    (void)path;
    return 0;   /* no statvfs on Windows; caller gets the error branch */
#else
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0;
    /* f_frsize is the fundamental block size for the byte math; fall back to
     * f_bsize if a platform reports frsize as 0. */
    uint64_t unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
    s_vfs_total = (int64_t)((uint64_t)st.f_blocks * unit);
    s_vfs_free  = (int64_t)((uint64_t)st.f_bfree  * unit);
    s_vfs_avail = (int64_t)((uint64_t)st.f_bavail * unit);
    return 1;
#endif
}

int64_t fs_get_statvfs_total(void) { return s_vfs_total; }
int64_t fs_get_statvfs_free(void)  { return s_vfs_free;  }
int64_t fs_get_statvfs_avail(void) { return s_vfs_avail; }

/* Mount enumeration (#1118): fs_try_mounts loads a thread-local table
 * (freeing the previous one), returns the entry count, -1 on failure.
 * The per-entry getters return pointers BORROWED from that table,
 * valid until the next fs_try_mounts / fs_release_mounts on the same
 * thread. Backends: Linux /proc/self/mountinfo (octal escapes
 * decoded), macOS + BSDs getmntinfo(3), Windows drive letters via
 * GetLogicalDriveStrings + GetVolumeInformation. */

typedef struct {
    char* source;
    char* point;
    char* fstype;
    char* options;
} AetherMountEntry;

static AETHER_FS_TLS AetherMountEntry* s_mounts = NULL;
static AETHER_FS_TLS int s_mount_count = 0;

void fs_release_mounts(void) {
    for (int i = 0; i < s_mount_count; i++) {
        free(s_mounts[i].source);
        free(s_mounts[i].point);
        free(s_mounts[i].fstype);
        free(s_mounts[i].options);
    }
    free(s_mounts);
    s_mounts = NULL;
    s_mount_count = 0;
}

static int fs_mounts_append(const char* src, const char* pt,
                            const char* ty, const char* op) {
    AetherMountEntry* grown = (AetherMountEntry*)realloc(
        s_mounts, (size_t)(s_mount_count + 1) * sizeof(AetherMountEntry));
    if (!grown) return 0;
    s_mounts = grown;
    s_mounts[s_mount_count].source  = strdup(src ? src : "");
    s_mounts[s_mount_count].point   = strdup(pt  ? pt  : "");
    s_mounts[s_mount_count].fstype  = strdup(ty  ? ty  : "");
    s_mounts[s_mount_count].options = strdup(op  ? op  : "");
    if (!s_mounts[s_mount_count].source || !s_mounts[s_mount_count].point ||
        !s_mounts[s_mount_count].fstype || !s_mounts[s_mount_count].options) {
        free(s_mounts[s_mount_count].source);
        free(s_mounts[s_mount_count].point);
        free(s_mounts[s_mount_count].fstype);
        free(s_mounts[s_mount_count].options);
        return 0;
    }
    s_mount_count++;
    return 1;
}

#if defined(__linux__)
/* mountinfo escapes space/tab/newline/backslash as \040-style octal;
 * decode in place so mountpoints with spaces round-trip. */
static void fs_mountinfo_unescape(char* s) {
    char* w = s;
    for (char* r = s; *r; ) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' &&
            r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}
#endif

int fs_try_mounts(void) {
    fs_release_mounts();
#if defined(__linux__)
    if (!aether_sandbox_check("fs_read", "/proc/self/mountinfo")) return -1;
    FILE* fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return -1;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        /* Format: ID PARENT MAJ:MIN ROOT MOUNTPOINT OPTIONS [optional...]
         *         - FSTYPE SOURCE SUPEROPTIONS                          */
        char* fields[8] = {0};
        int nf = 0;
        char* save = NULL;
        char* sep = strstr(line, " - ");
        if (!sep) continue;
        *sep = '\0';
        for (char* tok = strtok_r(line, " ", &save);
             tok && nf < 8; tok = strtok_r(NULL, " ", &save)) {
            fields[nf++] = tok;
        }
        if (nf < 6) continue;
        char* tail = sep + 3;
        char* tsave = NULL;
        char* fstype = strtok_r(tail, " ", &tsave);
        char* source = strtok_r(NULL, " ", &tsave);
        if (!fstype || !source) continue;
        char* nl = strchr(source, '\n');
        if (nl) *nl = '\0';
        fs_mountinfo_unescape(fields[4]);
        fs_mountinfo_unescape(source);
        if (!fs_mounts_append(source, fields[4], fstype, fields[5])) {
            fclose(fp);
            fs_release_mounts();
            return -1;
        }
    }
    fclose(fp);
    return s_mount_count;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    /* macOS / FreeBSD / OpenBSD share the `struct statfs` getmntinfo(3).
     * NetBSD is deliberately NOT in this list: there getmntinfo fills a
     * `struct statvfs` and the flag field is `f_flag`, not `f_flags`, so
     * this body would not compile. It falls through to the unsupported
     * branch and reports the error rather than shipping a shape nobody
     * has built, which is how the MNT_NODEV break below reached CI. */
    /* The optional mount flags this platform actually has. A flag is listed
     * only where the OS defines it, so the table IS the platform's flag set
     * rather than a full list with substitutes for the missing entries.
     * MNT_NODEV is the reason this is a table: nodev became a no-op on
     * FreeBSD and the macro was removed in FreeBSD 10, so a FreeBSD mount
     * has no nodev state and must report none, while macOS and OpenBSD
     * still carry it. MNT_RDONLY is not here because it is not optional:
     * every entry reports ro or rw. */
    static const struct { unsigned long long mask; const char* name; }
    k_optional_mount_flags[] = {
        { MNT_NOSUID, ",nosuid" },
#ifdef MNT_NODEV
        { MNT_NODEV,  ",nodev"  },
#endif
        { MNT_NOEXEC, ",noexec" },
    };
    struct statfs* mntbuf = NULL;
    int n = getmntinfo(&mntbuf, MNT_NOWAIT);
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        char opts[128];
        int used = snprintf(opts, sizeof(opts), "%s",
                            (mntbuf[i].f_flags & MNT_RDONLY) ? "ro" : "rw");
        if (used < 0) return -1;
        for (size_t k = 0;
             k < sizeof(k_optional_mount_flags) / sizeof(k_optional_mount_flags[0]);
             k++) {
            if (!((unsigned long long)mntbuf[i].f_flags & k_optional_mount_flags[k].mask))
                continue;
            int w = snprintf(opts + used, sizeof(opts) - (size_t)used, "%s",
                             k_optional_mount_flags[k].name);
            if (w < 0 || (size_t)w >= sizeof(opts) - (size_t)used) break;
            used += w;
        }
        if (!fs_mounts_append(mntbuf[i].f_mntfromname, mntbuf[i].f_mntonname,
                              mntbuf[i].f_fstypename, opts)) {
            fs_release_mounts();
            return -1;
        }
    }
    return s_mount_count;
#elif defined(_WIN32)
    char drives[512];
    DWORD len = GetLogicalDriveStringsA(sizeof(drives), drives);
    if (len == 0 || len >= sizeof(drives)) return -1;
    for (char* d = drives; *d; d += strlen(d) + 1) {
        UINT type = GetDriveTypeA(d);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        char fsname[64] = "";
        GetVolumeInformationA(d, NULL, 0, NULL, NULL, NULL,
                              fsname, sizeof(fsname));
        const char* opts =
            (type == DRIVE_CDROM) ? "ro" :
            (type == DRIVE_REMOVABLE) ? "rw,removable" : "rw";
        if (!fs_mounts_append(d, d, fsname, opts)) {
            fs_release_mounts();
            return -1;
        }
    }
    return s_mount_count;
#else
    return -1;
#endif
}

int fs_get_mount_count(void) { return s_mount_count; }

static const char* fs_mount_field(int i, size_t off) {
    if (i < 0 || i >= s_mount_count) return "";
    const char* p = *(char**)((char*)&s_mounts[i] + off);
    return p ? p : "";
}

const char* fs_get_mount_source(int i)  { return fs_mount_field(i, offsetof(AetherMountEntry, source)); }
const char* fs_get_mount_point(int i)   { return fs_mount_field(i, offsetof(AetherMountEntry, point)); }
const char* fs_get_mount_fstype(int i)  { return fs_mount_field(i, offsetof(AetherMountEntry, fstype)); }
const char* fs_get_mount_options(int i) { return fs_mount_field(i, offsetof(AetherMountEntry, options)); }

/* Block-device info (#1118): Linux sysfs backend. fs_try_block_info
 * accepts "/dev/sda", "sda", or a partition ("sda1", "nvme0n1p2",
 * resolved to its parent disk for the removable flag). Other
 * platforms return 0: per the stdlib's graceful-degradation
 * convention the caller gets an error, never a fabricated answer. */

static AETHER_FS_TLS int64_t s_blk_size = 0;
static AETHER_FS_TLS int     s_blk_removable = -1;
static AETHER_FS_TLS char    s_blk_transport[16] = "";

#if defined(__linux__)
static int fs_read_sysfs_line(const char* path, char* out, size_t cap) {
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    int ok = fgets(out, (int)cap, fp) != NULL;
    fclose(fp);
    if (!ok) return 0;
    char* nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 1;
}
#endif

int fs_try_block_info(const char* dev) {
    s_blk_size = 0;
    s_blk_removable = -1;
    s_blk_transport[0] = '\0';
    if (!dev || !*dev) return 0;
#if defined(__linux__)
    const char* name = dev;
    if (strncmp(name, "/dev/", 5) == 0) name = dev + 5;
    for (const char* c = name; *c; c++) {
        if (!(((*c >= 'a') && (*c <= 'z')) || ((*c >= 'A') && (*c <= 'Z')) ||
              ((*c >= '0') && (*c <= '9')) || *c == '_' || *c == '-')) {
            return 0;
        }
    }
    char path[512];
    char buf[128];
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", name);
    if (!fs_read_sysfs_line(path, buf, sizeof(buf))) return 0;
    s_blk_size = (int64_t)strtoll(buf, NULL, 10) * 512;

    /* removable lives on the whole disk; a partition ("sda1",
     * "nvme0n1p2") resolves to its parent via the sysfs symlink
     * (.../block/<disk>/<part>). */
    char parent[128];
    snprintf(parent, sizeof(parent), "%s", name);
    snprintf(path, sizeof(path), "/sys/class/block/%s", name);
    char real[512];
    if (realpath(path, real)) {
        char* blk = strstr(real, "/block/");
        if (blk) {
            blk += 7;
            char* slash = strchr(blk, '/');
            size_t plen = slash ? (size_t)(slash - blk) : strlen(blk);
            if (plen > 0 && plen < sizeof(parent)) {
                memcpy(parent, blk, plen);
                parent[plen] = '\0';
            }
        }
        if (strstr(real, "/usb"))         snprintf(s_blk_transport, sizeof(s_blk_transport), "usb");
        else if (strstr(real, "/nvme"))   snprintf(s_blk_transport, sizeof(s_blk_transport), "nvme");
        else if (strstr(real, "/virtio")) snprintf(s_blk_transport, sizeof(s_blk_transport), "virtio");
        else if (strstr(real, "/mmc"))    snprintf(s_blk_transport, sizeof(s_blk_transport), "mmc");
        else if (strstr(real, "/ata"))    snprintf(s_blk_transport, sizeof(s_blk_transport), "sata");
    }
    snprintf(path, sizeof(path), "/sys/class/block/%s/removable", parent);
    if (fs_read_sysfs_line(path, buf, sizeof(buf))) {
        s_blk_removable = (buf[0] == '1') ? 1 : 0;
    }
    return 1;
#else
    return 0;
#endif
}

int64_t     fs_get_block_size_bytes(void) { return s_blk_size; }
int         fs_get_block_removable(void)  { return s_blk_removable; }
const char* fs_get_block_transport(void)  { return s_blk_transport; }

char* fs_read_binary_raw(const char* path, int* out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    if (!aether_sandbox_check("fs_read", path)) return NULL;

    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    // Allocate size+1 so we can append a NUL past the end — handy for
    // callers who know the content is text and want to treat it as a
    // C string. The `out_len` byte count does NOT include this NUL.
    // Cap-aware (#343): same rationale as file_read_all_raw above —
    // unbounded file size, caller-owned return.
    size_t alloc_cap = (size_t)size + 1;
    char* buf = (char*)aether_caps_malloc(alloc_cap);
    if (!buf) { fclose(fp); return NULL; }

    size_t read = (size > 0) ? fread(buf, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (read != (size_t)size) { aether_caps_free(buf, alloc_cap); return NULL; }

    buf[size] = '\0';
    if (out_len) *out_len = (int)size;
    return buf;
}

// TLS cache for the split fs_try_read_binary path. The cached buffer
// is owned here; the getter hands back a borrowed pointer valid
// until the next fs_try_read_binary call or until fs_release_read_binary
// explicitly releases it. Aether callers copy the bytes out before
// issuing another read.
static AETHER_FS_TLS char* s_read_binary_buf = NULL;
static AETHER_FS_TLS int   s_read_binary_len = 0;

void fs_release_read_binary(void) {
    free(s_read_binary_buf);
    s_read_binary_buf = NULL;
    s_read_binary_len = 0;
}

int fs_try_read_binary(const char* path) {
    fs_release_read_binary();  // drop any previous read
    int len = 0;
    char* buf = fs_read_binary_raw(path, &len);
    if (!buf) return 0;
    s_read_binary_buf = buf;
    s_read_binary_len = len;
    return 1;
}

const char* fs_get_read_binary(void) { return s_read_binary_buf; }
int fs_get_read_binary_length(void)  { return s_read_binary_len; }

// Tuple-returning read_binary — the unified shape that the four-extern
// split-accessor pattern (fs_try_read_binary + fs_get_read_binary +
// fs_get_read_binary_length + fs_release_read_binary) was working
// around. Closes #273.
//
// Returns (bytes, length, err). On success: (AetherString*, len, "").
// On failure: (NULL, 0, "<reason>"). The bytes pointer is a refcounted
// AetherString that owns its payload — no companion release call needed.
//
// The struct shape mirrors the codegen-emitted `_tuple_ptr_int_string`
// typedef from `extern fs_read_binary_tuple(...) -> (ptr, int, string)`.
typedef struct {
    void* _0;          // AetherString* (cast to void* for the tuple ABI)
    int _1;            // length in bytes
    const char* _2;    // "" on success, error message on failure
} _tuple_ptr_int_string;

_tuple_ptr_int_string fs_read_binary_tuple(const char* path) {
    _tuple_ptr_int_string out;
    int len = 0;
    char* buf = fs_read_binary_raw(path, &len);
    if (!buf) {
        // Return an empty AetherString rather than NULL to preserve
        // the historical "" contract on the error path. Callers that
        // pattern-match on `err != ""` see the error first; readers
        // that touch `bytes` get a safe-to-print empty string instead
        // of a null deref.
        out._0 = (void*)string_empty();
        out._1 = 0;
        out._2 = "cannot read file";
        return out;
    }
    AetherString* wrapped = string_new_with_length(buf, (size_t)len);
    /* `buf` came from fs_read_binary_raw via aether_caps_malloc with
     * capacity len + 1 (size + 1 for the trailing NUL). Release it
     * through the matching aether_caps_free so the cap counter
     * decrements — libc free leaks the accounting and leaves
     * aether_caps_used_bytes() climbing by one buffer per call even
     * once string_release lands on the wrapper. */
    aether_caps_free(buf, (size_t)len + 1);
    if (!wrapped) {
        out._0 = (void*)string_empty();
        out._1 = 0;
        out._2 = "allocation failed";
        return out;
    }
    out._0 = (void*)wrapped;
    out._1 = len;
    out._2 = "";
    return out;
}

// ============================================================
// Structured-error pilot (issue #392) for fs.copy/move/realpath/chmod
// ============================================================
//
// Tuple ABI struct mirroring the codegen-emitted typedef from
// `extern fs_copy_raw(...) -> (int, int, string)`. _0 = bytes copied
// (or progress made on partial failure), _1 = AETHER_FS_KIND_*,
// _2 = "" on success or human-readable diagnostic on failure.
typedef struct {
    int _0;            // bytes copied (saturated at INT_MAX)
    int _1;            // AETHER_FS_KIND_*
    const char* _2;    // "" on success, error message on failure
} _tuple_int_int_string;

// Single errno -> kind translation site, used by every pilot
// primitive. Adding a new kind: extend the switch + the macros in
// aether_fs.h + the const block in std/fs/module.ae together.
static int aether_fs_errno_to_kind(int err) {
    switch (err) {
        case 0:            return AETHER_FS_KIND_OK;
        case ENOENT:       return AETHER_FS_KIND_NOT_FOUND;
#ifdef EACCES
        case EACCES:       return AETHER_FS_KIND_PERMISSION_DENIED;
#endif
#if defined(EPERM) && (!defined(EACCES) || EPERM != EACCES)
        case EPERM:        return AETHER_FS_KIND_PERMISSION_DENIED;
#endif
        case EEXIST:       return AETHER_FS_KIND_EXISTS;
#ifdef EXDEV
        case EXDEV:        return AETHER_FS_KIND_CROSS_DEVICE;
#endif
#ifdef EIO
        case EIO:          return AETHER_FS_KIND_IO;
#endif
        case EINVAL:       return AETHER_FS_KIND_INVALID;
#ifdef ELOOP
        case ELOOP:        return AETHER_FS_KIND_LOOP;
#endif
#ifdef ENAMETOOLONG
        case ENAMETOOLONG: return AETHER_FS_KIND_NAME_TOO_LONG;
#endif
#ifdef ENOSPC
        case ENOSPC:       return AETHER_FS_KIND_NO_SPACE;
#endif
#ifdef EISDIR
        case EISDIR:       return AETHER_FS_KIND_IS_DIR;
#endif
#ifdef ENOTDIR
        case ENOTDIR:      return AETHER_FS_KIND_NOT_DIR;
#endif
        default:           return AETHER_FS_KIND_IO;
    }
}

static int aether_fs_saturate_int(long long v) {
    if (v < 0) return 0;
    if (v > (long long)INT_MAX) return INT_MAX;
    return (int)v;
}

static _tuple_int_int_string aether_fs_iks_err(int kind, const char* msg) {
    _tuple_int_int_string out = { 0, kind, msg };
    return out;
}
static _tuple_int_int_string aether_fs_iks_err_partial(long long bytes, int kind, const char* msg) {
    _tuple_int_int_string out = { aether_fs_saturate_int(bytes), kind, msg };
    return out;
}
static _tuple_int_int_string aether_fs_iks_ok(long long bytes) {
    _tuple_int_int_string out = { aether_fs_saturate_int(bytes), AETHER_FS_KIND_OK, "" };
    return out;
}

#if defined(__linux__) && defined(SYS_copy_file_range)
/* Wrap copy_file_range via syscall(2) so we don't need _GNU_SOURCE
 * across the whole translation unit and don't depend on the libc
 * actually exporting the symbol. Returns the same shape the libc
 * wrapper would: bytes copied (>=0), or -1 with errno set. */
static long long aether_fs_copy_file_range_syscall(int in_fd, int out_fd, size_t len) {
    return (long long)syscall(SYS_copy_file_range,
                              in_fd, (long long*)NULL,
                              out_fd, (long long*)NULL,
                              len, (unsigned int)0);
}
#endif

#ifndef _WIN32
/* Last-tier portable fallback: 8 MiB read/write loop with
 * partial-write resumption + EINTR retry. Returns total bytes
 * written on success, or -1 with errno set on failure. On failure
 * `*out_partial` is set to bytes successfully written so far so the
 * caller can surface progress in the structured-error tuple. */
static long long aether_fs_copy_readwrite(int in_fd, int out_fd, long long* out_partial) {
    enum { BUF_BYTES = 8 * 1024 * 1024 };  // 8 MiB
    /* #462: the copy scratch is a fixed 8 MiB — gate it so a sandboxed
     * plugin spamming fs.copy can't pin large buffers past the cap.
     * Internal/transient: every exit frees through the cap (constant
     * size), so accounting is exactly balanced. */
    char* buf = (char*)aether_caps_malloc((size_t)BUF_BYTES);
    if (!buf) {
        if (out_partial) *out_partial = 0;
        errno = ENOMEM;
        return -1;
    }
    long long total = 0;
    for (;;) {
        ssize_t r;
        do { r = read(in_fd, buf, (size_t)BUF_BYTES); } while (r < 0 && errno == EINTR);
        if (r < 0) {
            int saved = errno;
            aether_caps_free(buf, (size_t)BUF_BYTES);
            if (out_partial) *out_partial = total;
            errno = saved;
            return -1;
        }
        if (r == 0) break;  // EOF
        const char* p = buf;
        ssize_t left = r;
        while (left > 0) {
            ssize_t w;
            do { w = write(out_fd, p, (size_t)left); } while (w < 0 && errno == EINTR);
            if (w < 0) {
                int saved = errno;
                aether_caps_free(buf, (size_t)BUF_BYTES);
                if (out_partial) *out_partial = total;
                errno = saved;
                return -1;
            }
            left -= w;
            p += w;
            total += w;
        }
    }
    aether_caps_free(buf, (size_t)BUF_BYTES);
    if (out_partial) *out_partial = total;
    return total;
}
#endif

/* fs_copy_raw — copy file contents from `src` to `dst`, preserving
 * source mode bits (file owner is NOT changed; that needs CAP_CHOWN
 * and is out of scope). Returns the structured-error tuple
 * (bytes_copied, kind, message). On partial failure `bytes_copied`
 * reflects how far we got before erroring out.
 *
 * Symlink behaviour: follows the source symlink (matches POSIX `cp`
 * without -P). The destination is overwritten if it exists (matches
 * `cp` with no -i / -n). Concurrent copies to the same destination
 * are caller-coordinated — interleaving is POSIX UB and surfaces
 * here as KIND_IO.
 *
 * Performance tiers (best to fallback):
 *   Linux:   copy_file_range(2) via syscall  -> sendfile(2) -> read/write
 *   macOS:   fcopyfile(COPYFILE_DATA)        -> read/write
 *   Other:   read/write
 *   Windows: CopyFileExW (kernel block copy; UTF-8 path conversion)
 */
_tuple_int_int_string fs_copy_raw(const char* src, const char* dst) {
    if (!src || !dst) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID, "null path");
    }
    if (!aether_sandbox_check("fs_read", src)) {
        return aether_fs_iks_err(AETHER_FS_KIND_PERMISSION_DENIED,
                                 "sandbox: cannot read src");
    }
    if (!aether_sandbox_check("fs_write", dst)) {
        return aether_fs_iks_err(AETHER_FS_KIND_PERMISSION_DENIED,
                                 "sandbox: cannot write dst");
    }
    /* Lexical equality reject — POSIX `cp` also refuses. Doesn't catch
     * the same-file-via-different-paths case; that's the caller's
     * responsibility (would require resolving both via `realpath`,
     * which itself can fail if a parent doesn't exist yet). */
    if (strcmp(src, dst) == 0) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID,
                                 "src and dst are the same path");
    }

#ifdef _WIN32
    /* Windows fast path: kernel-side block copy via CopyFileExW.
     * UTF-8 → UTF-16 conversion required; CopyFileA only handles ANSI
     * code-page paths, which mishandles non-Latin1 filenames. */
    int wsrc_len = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    int wdst_len = MultiByteToWideChar(CP_UTF8, 0, dst, -1, NULL, 0);
    if (wsrc_len <= 0 || wdst_len <= 0) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID, "path UTF-8 invalid");
    }
    /* #462: transient UTF-8 → UTF-16 path scratch — alloc'd and freed
     * locally on every path through this function, sized by the
     * (plugin-supplied) path length. Gate it and free through the cap
     * with the exact byte count so accounting balances. */
    size_t wsrc_bytes = (size_t)wsrc_len * sizeof(wchar_t);
    size_t wdst_bytes = (size_t)wdst_len * sizeof(wchar_t);
    wchar_t* wsrc = (wchar_t*)aether_caps_malloc(wsrc_bytes);
    wchar_t* wdst = (wchar_t*)aether_caps_malloc(wdst_bytes);
    if (!wsrc || !wdst) {
        aether_caps_free(wsrc, wsrc_bytes); aether_caps_free(wdst, wdst_bytes);
        return aether_fs_iks_err(AETHER_FS_KIND_IO, "allocation failed");
    }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, wsrc, wsrc_len);
    MultiByteToWideChar(CP_UTF8, 0, dst, -1, wdst, wdst_len);
    /* Pre-flight: classify directories explicitly. CopyFileExW returns
     * ERROR_ACCESS_DENIED when src is a directory or dst is an
     * existing directory, which would surface as KIND_PERMISSION_DENIED
     * — wrong: the POSIX path returns KIND_IS_DIR for the same shape
     * (S_ISDIR check), and callers expect that classification. Use
     * GetFileAttributesW to discriminate before the syscall. */
    DWORD src_attr = GetFileAttributesW(wsrc);
    if (src_attr != INVALID_FILE_ATTRIBUTES &&
        (src_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        aether_caps_free(wsrc, wsrc_bytes); aether_caps_free(wdst, wdst_bytes);
        return aether_fs_iks_err(AETHER_FS_KIND_IS_DIR, "src is a directory");
    }
    DWORD dst_attr = GetFileAttributesW(wdst);
    if (dst_attr != INVALID_FILE_ATTRIBUTES &&
        (dst_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        aether_caps_free(wsrc, wsrc_bytes); aether_caps_free(wdst, wdst_bytes);
        return aether_fs_iks_err(AETHER_FS_KIND_IS_DIR, "dst is a directory");
    }
    /* bFailIfExists=FALSE → match POSIX cp: overwrite. */
    BOOL ok = CopyFileExW(wsrc, wdst, NULL, NULL, NULL, 0);
    DWORD win_err = GetLastError();
    aether_caps_free(wsrc, wsrc_bytes); aether_caps_free(wdst, wdst_bytes);
    if (!ok) {
        int kind = AETHER_FS_KIND_IO;
        const char* msg = "CopyFileExW failed";
        switch (win_err) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                kind = AETHER_FS_KIND_NOT_FOUND; msg = "src not found"; break;
            case ERROR_ACCESS_DENIED:
                kind = AETHER_FS_KIND_PERMISSION_DENIED; msg = "access denied"; break;
            case ERROR_DISK_FULL:
                kind = AETHER_FS_KIND_NO_SPACE; msg = "disk full"; break;
            case ERROR_ALREADY_EXISTS:
                kind = AETHER_FS_KIND_EXISTS; msg = "destination exists"; break;
        }
        return aether_fs_iks_err(kind, msg);
    }
    /* Windows stat for the size — best-effort (CopyFileExW already
     * succeeded so the file is fully written). */
    struct _stat64 win_st;
    long long bytes = 0;
    if (_stat64(dst, &win_st) == 0) bytes = (long long)win_st.st_size;
    return aether_fs_iks_ok(bytes);
#else
    /* POSIX path. */
    struct stat src_st;
    if (stat(src, &src_st) != 0) {
        int kind = aether_fs_errno_to_kind(errno);
        return aether_fs_iks_err(kind, "stat src failed");
    }
    if (S_ISDIR(src_st.st_mode)) {
        return aether_fs_iks_err(AETHER_FS_KIND_IS_DIR, "src is a directory");
    }

    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        int kind = aether_fs_errno_to_kind(errno);
        return aether_fs_iks_err(kind, "open src failed");
    }
    /* Reject if dst already exists as a directory — overwriting onto
     * a directory is not what the caller meant. POSIX open(O_CREAT)
     * would EISDIR here; we surface that as KIND_IS_DIR cleanly. */
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
        close(in_fd);
        return aether_fs_iks_err(AETHER_FS_KIND_IS_DIR, "dst is a directory");
    }
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC,
                      (mode_t)(src_st.st_mode & 07777));
    if (out_fd < 0) {
        int kind = aether_fs_errno_to_kind(errno);
        close(in_fd);
        return aether_fs_iks_err(kind, "open dst failed");
    }

    long long total = 0;
    long long expected = (long long)src_st.st_size;

#if defined(__linux__) && defined(SYS_copy_file_range)
    /* Tier 1: copy_file_range — kernel-side; reflinks on btrfs/XFS. */
    while (total < expected) {
        size_t want = (size_t)(expected - total);
        long long n = aether_fs_copy_file_range_syscall(in_fd, out_fd, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* Fall through to sendfile only on the documented
             * "this filesystem can't" / "kernel doesn't support"
             * errnos. Anything else is a real failure that we
             * surface immediately with the partial count. */
            if (errno == ENOSYS || errno == EINVAL || errno == EXDEV ||
                errno == EOPNOTSUPP || errno == EBADF || errno == ETXTBSY) {
                break;
            }
            int kind = aether_fs_errno_to_kind(errno);
            close(in_fd); close(out_fd);
            return aether_fs_iks_err_partial(total, kind, "copy_file_range failed");
        }
        if (n == 0) break;  /* EOF */
        total += n;
    }
#endif

#if defined(__linux__)
    /* Tier 2: sendfile — older kernels and cross-fs cases that
     * copy_file_range refused. Picks up from the fd offset advanced
     * by tier 1 (both syscalls share the kernel fd-pos invariant). */
    while (total < expected) {
        size_t want = (size_t)(expected - total);
        ssize_t n = sendfile(out_fd, in_fd, NULL, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP) {
                break;  /* fall through to read/write */
            }
            int kind = aether_fs_errno_to_kind(errno);
            close(in_fd); close(out_fd);
            return aether_fs_iks_err_partial(total, kind, "sendfile failed");
        }
        if (n == 0) break;
        total += n;
    }
#elif defined(__APPLE__)
    /* macOS fast path: fcopyfile triggers APFS clone on same-volume
     * copies, kernel-level block copy otherwise. Fully resets fd
     * offsets internally; if it fails we restart with a fresh truncate
     * for the read/write fallback. */
    if (total == 0) {
        copyfile_state_t state = copyfile_state_alloc();
        int rv = (state ? fcopyfile(in_fd, out_fd, state, COPYFILE_DATA) : -1);
        if (state) copyfile_state_free(state);
        if (rv == 0) {
            total = expected;
        } else {
            /* Reset for read/write fallback. */
            (void)lseek(in_fd, 0, SEEK_SET);
            (void)lseek(out_fd, 0, SEEK_SET);
            (void)ftruncate(out_fd, 0);
            total = 0;
        }
    }
#endif

    if (total < expected) {
        /* Tier 3 (or sole tier on platforms that fell through both
         * tier 1 and tier 2): portable read/write with EINTR retry,
         * partial-write resumption, and 8 MiB chunks. */
        long long partial = 0;
        long long rv = aether_fs_copy_readwrite(in_fd, out_fd, &partial);
        if (rv < 0) {
            int kind = aether_fs_errno_to_kind(errno);
            close(in_fd); close(out_fd);
            return aether_fs_iks_err_partial(total + partial, kind, "read/write failed");
        }
        total += rv;
    }

    /* Preserve permission bits. Failure to fchmod is non-fatal —
     * the data copy already succeeded; the caller can chmod
     * separately if they care. */
    (void)fchmod(out_fd, (mode_t)(src_st.st_mode & 07777));

    close(in_fd);
    if (close(out_fd) != 0) {
        /* Some filesystems (NFS) defer write errors until close.
         * Surface as KIND_IO with the bytes-counted-so-far. */
        int kind = aether_fs_errno_to_kind(errno);
        return aether_fs_iks_err_partial(total, kind, "close dst failed");
    }
    return aether_fs_iks_ok(total);
#endif
}

/* fs_move_raw — move file from `src` to `dst` with cross-device
 * fallback. Returns (1, KIND_OK, "") on success or (0, KIND_*, msg)
 * on failure. POSIX rename(2) is atomic on the same filesystem. On
 * cross-device (EXDEV) we fall back to fs_copy_raw + unlink — NOT
 * atomic, but correct. On copy failure during the EXDEV path, src
 * is left in place (no half-move) and the copy's kind is
 * propagated. Windows MoveFileExW with REPLACE_EXISTING +
 * COPY_ALLOWED handles the cross-fs case internally. */
_tuple_int_int_string fs_move_raw(const char* src, const char* dst) {
    if (!src || !dst) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID, "null path");
    }
    if (!aether_sandbox_check("fs_write", src)) {
        return aether_fs_iks_err(AETHER_FS_KIND_PERMISSION_DENIED,
                                 "sandbox: cannot remove src");
    }
    if (!aether_sandbox_check("fs_write", dst)) {
        return aether_fs_iks_err(AETHER_FS_KIND_PERMISSION_DENIED,
                                 "sandbox: cannot write dst");
    }
    if (strcmp(src, dst) == 0) {
        /* POSIX rename of a path onto itself is a successful no-op —
         * but POSIX `mv x x` errors out. Match the user-facing tool. */
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID,
                                 "src and dst are the same path");
    }

#ifdef _WIN32
    int wsrc_len = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    int wdst_len = MultiByteToWideChar(CP_UTF8, 0, dst, -1, NULL, 0);
    if (wsrc_len <= 0 || wdst_len <= 0) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID, "path UTF-8 invalid");
    }
    /* #462: transient UTF-8 → UTF-16 path scratch — see fs_copy_raw. */
    size_t wsrc_bytes = (size_t)wsrc_len * sizeof(wchar_t);
    size_t wdst_bytes = (size_t)wdst_len * sizeof(wchar_t);
    wchar_t* wsrc = (wchar_t*)aether_caps_malloc(wsrc_bytes);
    wchar_t* wdst = (wchar_t*)aether_caps_malloc(wdst_bytes);
    if (!wsrc || !wdst) {
        aether_caps_free(wsrc, wsrc_bytes); aether_caps_free(wdst, wdst_bytes);
        return aether_fs_iks_err(AETHER_FS_KIND_IO, "allocation failed");
    }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, wsrc, wsrc_len);
    MultiByteToWideChar(CP_UTF8, 0, dst, -1, wdst, wdst_len);
    BOOL ok = MoveFileExW(wsrc, wdst,
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    DWORD win_err = GetLastError();
    aether_caps_free(wsrc, wsrc_bytes); aether_caps_free(wdst, wdst_bytes);
    if (!ok) {
        int kind = AETHER_FS_KIND_IO;
        const char* msg = "MoveFileExW failed";
        switch (win_err) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                kind = AETHER_FS_KIND_NOT_FOUND; msg = "src not found"; break;
            case ERROR_ACCESS_DENIED:
                kind = AETHER_FS_KIND_PERMISSION_DENIED; msg = "access denied"; break;
            case ERROR_DISK_FULL:
                kind = AETHER_FS_KIND_NO_SPACE; msg = "disk full"; break;
        }
        return aether_fs_iks_err(kind, msg);
    }
    _tuple_int_int_string out = { 1, AETHER_FS_KIND_OK, "" };
    return out;
#else
    if (rename(src, dst) == 0) {
        _tuple_int_int_string out = { 1, AETHER_FS_KIND_OK, "" };
        return out;
    }
    int saved = errno;
    if (saved != EXDEV) {
        int kind = aether_fs_errno_to_kind(saved);
        return aether_fs_iks_err(kind, "rename failed");
    }
    /* Cross-device fallback: copy + unlink. fs_copy_raw refuses to
     * copy directories, so cross-device directory moves surface as
     * KIND_IS_DIR with a clear message — documented limitation. */
    _tuple_int_int_string copy_result = fs_copy_raw(src, dst);
    if (copy_result._1 != AETHER_FS_KIND_OK) {
        /* Don't try to roll back a partial dst — the destination is
         * the caller's territory, removing it could surprise them.
         * Surface the copy's kind so the caller knows what failed. */
        return aether_fs_iks_err(copy_result._1, copy_result._2);
    }
    if (unlink(src) != 0) {
        /* Data is now in dst (good) but src is still there (bad).
         * Half-move state. Surface as KIND_IO with a clear message;
         * caller may want to retry the unlink or accept the duplicate. */
        int kind = aether_fs_errno_to_kind(errno);
        _tuple_int_int_string out = { 1, kind, "moved (copy) but unlink src failed" };
        return out;
    }
    _tuple_int_int_string out = { 1, AETHER_FS_KIND_OK, "" };
    return out;
#endif
}

/* Tuple ABI for (string, int, string) returns. _0 is a heap-allocated
 * resolved-path string the caller (Aether runtime) takes ownership of;
 * _1 is the kind; _2 is a string literal — never freed. Mirrors the
 * shape used by std/os/aether_os.c's _tuple_string_int_string. */
typedef struct {
    const char* _0;    // resolved path (heap, runtime owns) or "" on failure
    int _1;            // AETHER_FS_KIND_*
    const char* _2;    // "" on success, error message on failure
} _tuple_string_int_string;

/* Position-0 sentinel for error returns. The extern is annotated
 * `(string @heap, int, string)` so the caller-side heap-string-
 * tracker auto-frees position 0 on function exit. Returning a
 * static literal `""` would surface as `free((void*)"")` and
 * abort under glibc / dyld. Allocate a fresh 1-byte buffer so
 * the @heap contract holds uniformly across success and error
 * paths; callers comparing `resolved == ""` continue to match
 * via the AetherString-aware string compare (string_equals walks
 * the bytes regardless of allocation origin). */
static char* aether_fs_sks_empty_heap(void) {
    char* p = (char*)malloc(1);
    if (p) p[0] = '\0';
    return p;
}
static _tuple_string_int_string aether_fs_sks_err(int kind, const char* msg) {
    _tuple_string_int_string out = { aether_fs_sks_empty_heap(), kind, msg };
    return out;
}
static _tuple_string_int_string aether_fs_sks_ok(const char* resolved) {
    _tuple_string_int_string out = { resolved, AETHER_FS_KIND_OK, "" };
    return out;
}

/* fs_realpath_raw — canonicalise `path` via the OS resolver. Follows
 * every symlink along the way and removes . / .. components.
 * POSIX: realpath(path, NULL) allocates a fresh buffer (POSIX.1-2008
 * extension; available in glibc 2.3+, every modern BSD, macOS).
 * Windows: GetFinalPathNameByHandleW after CreateFileW with
 * FILE_FLAG_BACKUP_SEMANTICS so it works on directories. The
 * \\?\ prefix that GetFinalPathNameByHandleW prepends is stripped
 * before returning so callers get plain forward-slash paths. */
_tuple_string_int_string fs_realpath_raw(const char* path) {
    if (!path) {
        return aether_fs_sks_err(AETHER_FS_KIND_INVALID, "null path");
    }
    if (!aether_sandbox_check("fs_read", path)) {
        return aether_fs_sks_err(AETHER_FS_KIND_PERMISSION_DENIED,
                                 "sandbox: cannot read path");
    }

#ifdef _WIN32
    int wpath_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wpath_len <= 0) {
        return aether_fs_sks_err(AETHER_FS_KIND_INVALID, "path UTF-8 invalid");
    }
    /* #462: transient UTF-8 → UTF-16 path scratch — alloc'd and freed
     * locally, sized by the (plugin-supplied) path length. The eventual
     * UTF-8 result `u8` below stays raw because it escapes as the
     * caller-owned path string (PR #845 returned-string contract). */
    size_t wpath_bytes = (size_t)wpath_len * sizeof(wchar_t);
    wchar_t* wpath = (wchar_t*)aether_caps_malloc(wpath_bytes);
    if (!wpath) {
        return aether_fs_sks_err(AETHER_FS_KIND_IO, "allocation failed");
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wpath_len);
    HANDLE h = CreateFileW(wpath, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    aether_caps_free(wpath, wpath_bytes);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD win_err = GetLastError();
        int kind = (win_err == ERROR_FILE_NOT_FOUND ||
                    win_err == ERROR_PATH_NOT_FOUND)
                   ? AETHER_FS_KIND_NOT_FOUND
                   : (win_err == ERROR_ACCESS_DENIED
                      ? AETHER_FS_KIND_PERMISSION_DENIED
                      : AETHER_FS_KIND_IO);
        return aether_fs_sks_err(kind, "CreateFileW failed");
    }
    /* GetFinalPathNameByHandleW: first call with cb=0 returns required size. */
    DWORD need = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
    if (need == 0) {
        CloseHandle(h);
        return aether_fs_sks_err(AETHER_FS_KIND_IO, "GetFinalPathNameByHandleW failed");
    }
    /* #462: transient wide-char result buffer — freed locally on every
     * path; only the UTF-8 `u8` conversion below escapes (stays raw). */
    size_t wresult_bytes = (size_t)need * sizeof(wchar_t);
    wchar_t* wresult = (wchar_t*)aether_caps_malloc(wresult_bytes);
    if (!wresult) {
        CloseHandle(h);
        return aether_fs_sks_err(AETHER_FS_KIND_IO, "allocation failed");
    }
    DWORD got = GetFinalPathNameByHandleW(h, wresult, need, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (got == 0 || got >= need) {
        aether_caps_free(wresult, wresult_bytes);
        return aether_fs_sks_err(AETHER_FS_KIND_IO, "GetFinalPathNameByHandleW size race");
    }
    /* Strip the \\?\ prefix (4 wchars) so callers see a plain path. */
    const wchar_t* wstart = wresult;
    if (got >= 4 && wresult[0] == L'\\' && wresult[1] == L'\\' &&
        wresult[2] == L'?'  && wresult[3] == L'\\') {
        wstart = wresult + 4;
    }
    int u8_len = WideCharToMultiByte(CP_UTF8, 0, wstart, -1, NULL, 0, NULL, NULL);
    if (u8_len <= 0) {
        aether_caps_free(wresult, wresult_bytes);
        return aether_fs_sks_err(AETHER_FS_KIND_IO, "UTF-16 → UTF-8 failed");
    }
    /* RAW BY DESIGN (PR #845): `u8` is the resolved-path result handed
     * back to Aether and freed by the heap-string machinery, not by
     * aether_caps_free. Cap-accounting it would drift the counter. */
    char* u8 = (char*)malloc((size_t)u8_len);
    if (!u8) {
        aether_caps_free(wresult, wresult_bytes);
        return aether_fs_sks_err(AETHER_FS_KIND_IO, "allocation failed");
    }
    WideCharToMultiByte(CP_UTF8, 0, wstart, -1, u8, u8_len, NULL, NULL);
    aether_caps_free(wresult, wresult_bytes);
    return aether_fs_sks_ok(u8);
#else
    char* resolved = realpath(path, NULL);
    if (!resolved) {
        int kind = aether_fs_errno_to_kind(errno);
        const char* msg = "realpath failed";
        if (kind == AETHER_FS_KIND_NOT_FOUND) msg = "path not found";
        else if (kind == AETHER_FS_KIND_LOOP) msg = "symlink cycle";
        else if (kind == AETHER_FS_KIND_NAME_TOO_LONG) msg = "name too long";
        else if (kind == AETHER_FS_KIND_NOT_DIR) msg = "non-directory in path";
        else if (kind == AETHER_FS_KIND_PERMISSION_DENIED) msg = "access denied";
        return aether_fs_sks_err(kind, msg);
    }
    return aether_fs_sks_ok(resolved);
#endif
}

/* fs_chmod_raw — change permission bits on `path`. POSIX chmod(2)
 * follows symlinks (matches what a shell `chmod` does). Windows has
 * no concept of POSIX mode bits at the filesystem layer, so we
 * emulate the user-write bit only via SetFileAttributesW: the
 * read-only attribute is toggled by 0o200; every other bit is
 * silently ignored. This matches the documented behaviour of
 * Python's `os.chmod`. Returns (1, KIND_OK, "") on success or
 * (0, KIND_*, msg) on failure. */
_tuple_int_int_string fs_chmod_raw(const char* path, int mode) {
    if (!path) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID, "null path");
    }
    if (!aether_sandbox_check("fs_write", path)) {
        return aether_fs_iks_err(AETHER_FS_KIND_PERMISSION_DENIED,
                                 "sandbox: cannot chmod path");
    }
#ifdef _WIN32
    int wpath_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wpath_len <= 0) {
        return aether_fs_iks_err(AETHER_FS_KIND_INVALID, "path UTF-8 invalid");
    }
    /* #462: transient UTF-8 → UTF-16 path scratch — see fs_copy_raw. */
    size_t wpath_bytes = (size_t)wpath_len * sizeof(wchar_t);
    wchar_t* wpath = (wchar_t*)aether_caps_malloc(wpath_bytes);
    if (!wpath) {
        return aether_fs_iks_err(AETHER_FS_KIND_IO, "allocation failed");
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wpath_len);
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        aether_caps_free(wpath, wpath_bytes);
        DWORD win_err = GetLastError();
        int kind = (win_err == ERROR_FILE_NOT_FOUND ||
                    win_err == ERROR_PATH_NOT_FOUND)
                   ? AETHER_FS_KIND_NOT_FOUND
                   : AETHER_FS_KIND_IO;
        return aether_fs_iks_err(kind, "GetFileAttributesW failed");
    }
    /* Owner-write (0o200) sets clear; all other bits ignored. */
    if (mode & 0200) {
        attrs &= ~FILE_ATTRIBUTE_READONLY;
    } else {
        attrs |= FILE_ATTRIBUTE_READONLY;
    }
    BOOL ok = SetFileAttributesW(wpath, attrs);
    aether_caps_free(wpath, wpath_bytes);
    if (!ok) {
        int kind = (GetLastError() == ERROR_ACCESS_DENIED)
                   ? AETHER_FS_KIND_PERMISSION_DENIED
                   : AETHER_FS_KIND_IO;
        return aether_fs_iks_err(kind, "SetFileAttributesW failed");
    }
    _tuple_int_int_string out = { 1, AETHER_FS_KIND_OK, "" };
    return out;
#else
    if (chmod(path, (mode_t)(mode & 07777)) != 0) {
        int kind = aether_fs_errno_to_kind(errno);
        return aether_fs_iks_err(kind, "chmod failed");
    }
    _tuple_int_int_string out = { 1, AETHER_FS_KIND_OK, "" };
    return out;
#endif
}

// Path operations
char* path_join(const char* path1, const char* path2) {
    if (!path1 || !path2) return NULL;

    size_t len1 = strlen(path1);
    size_t len2 = strlen(path2);

    // Always use '/' — it works on all platforms (Windows C stdlib accepts '/')
    // and keeps paths consistent with Aether's module system.
    char sep = '/';

    int needs_sep = (len1 > 0 && path1[len1-1] != '/' && path1[len1-1] != '\\');
    size_t total = len1 + len2 + (needs_sep ? 1 : 0);

    char* result = (char*)malloc(total + 1);
    if (!result) return NULL;
    strcpy(result, path1);
    if (needs_sep) {
        result[len1] = sep;
        strcpy(result + len1 + 1, path2);
    } else {
        strcpy(result + len1, path2);
    }

    return result;
}

char* path_dirname(const char* path) {
    if (!path) return NULL;

    const char* last_sep = strrchr(path, '/');
    const char* last_sep_win = strrchr(path, '\\');

    if (last_sep_win && (!last_sep || last_sep_win > last_sep)) {
        last_sep = last_sep_win;
    }

    if (!last_sep) {
        return strdup(".");
    }

    size_t len = last_sep - path;
    if (len == 0) len = 1;  // Root directory

    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, path, len);
    result[len] = '\0';

    return result;
}

char* path_basename(const char* path) {
    if (!path) return NULL;

    const char* last_sep = strrchr(path, '/');
    const char* last_sep_win = strrchr(path, '\\');

    if (last_sep_win && (!last_sep || last_sep_win > last_sep)) {
        last_sep = last_sep_win;
    }

    const char* base = last_sep ? last_sep + 1 : path;
    return strdup(base);
}

char* path_extension(const char* path) {
    if (!path) return NULL;

    const char* last_dot = strrchr(path, '.');
    const char* last_sep = strrchr(path, '/');
    const char* last_sep_win = strrchr(path, '\\');

    if (last_sep_win && (!last_sep || last_sep_win > last_sep)) {
        last_sep = last_sep_win;
    }

    if (!last_dot || (last_sep && last_dot < last_sep)) {
        return strdup("");
    }

    return strdup(last_dot);
}

int path_is_absolute(const char* path) {
    if (!path || path[0] == '\0') return 0;

    // Unix-style absolute: /path (works on all platforms)
    if (path[0] == '/') return 1;

    #ifdef _WIN32
    // Windows: C:\ or C:/ or \\server\share
    if ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) {
        if (path[1] && path[1] == ':' && path[2] && (path[2] == '\\' || path[2] == '/')) {
            return 1;
        }
    }
    if (path[0] == '\\' && path[1] && path[1] == '\\') return 1;
    #endif

    return 0;
}

/* #1369: the platform path separator as a static string ("/" on POSIX, "\\"
 * on Windows). Returned as a plain const char* literal — no allocation, no
 * free. Lets callers stop hardcoding "/" in path concatenation. */
const char* path_separator(void) {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

/* #1369: on POSIX a backslash is an ordinary filename byte and must never
 * split a segment. Only Windows accepts both. */
static int path_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* The separator these helpers EMIT. Always '/', including on Windows, which
 * accepts it everywhere: the cleaned form feeds globs, import paths and
 * existing callers that compare against '/'. Only the INPUT side is
 * platform-dependent (path_is_sep), which is what #1369 actually needed. */
#define PATH_SEP_CHAR '/'

/* Bytes at the head of `p` naming a volume rather than a path segment: 2 for
 * "C:", the whole "\\server\share" for a UNC path, 0 otherwise (always 0 on
 * POSIX). The prefix survives normalisation verbatim. */
static size_t path_volume_len(const char* p, size_t n) {
#ifdef _WIN32
    if (n >= 2 && p[1] == ':' &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) {
        return 2;
    }
    if (n >= 2 && path_is_sep(p[0]) && path_is_sep(p[1])) {
        size_t i = 2;
        while (i < n && !path_is_sep(p[i])) i++;          /* server */
        if (i < n) {
            i++;
            while (i < n && !path_is_sep(p[i])) i++;      /* share  */
        }
        return i;
    }
#else
    (void)p; (void)n;
#endif
    return 0;
}

/* Windows filesystems are case-insensitive, so a byte-exact containment test
 * would reject a legitimate "c:\base\f" under "C:\Base". Separators compare
 * equal to each other for the same reason. */
static int path_chars_equal(char a, char b) {
#ifdef _WIN32
    if (path_is_sep(a) && path_is_sep(b)) return 1;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
    return a == b;
}

static int path_span_equal(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!path_chars_equal(a[i], b[i])) return 0;
    }
    return 1;
}

/* #462: strdup through the capability allocator. The matching free in
 * dir_list_free recomputes strlen(name)+1, which equals this size, so
 * the accounting balances exactly. NULL-safe. */
static char* fs_caps_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)aether_caps_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* #966: map readdir's d_type to fs_stat_raw's kind encoding
 * (1 file / 2 dir / 3 symlink / 4 other), 0 when the FS doesn't report
 * one (DT_UNKNOWN) so the caller stats only those. */
#ifndef _WIN32
static int fs_dtype_to_kind(unsigned char dt) {
#ifdef DT_DIR
    switch (dt) {
        case DT_REG:  return FS_STAT_KIND_FILE;
        case DT_DIR:  return FS_STAT_KIND_DIR;
        case DT_LNK:  return FS_STAT_KIND_SYMLINK;
        case DT_SOCK: return FS_STAT_KIND_SOCKET;  /* #1368 */
        case DT_FIFO: return FS_STAT_KIND_FIFO;    /* #1368 */
        case DT_CHR:
        case DT_BLK:  return FS_STAT_KIND_DEVICE;  /* #1368 */
        default:      return 0;   /* DT_UNKNOWN or unrecognised */
    }
#else
    (void)dt;
    return 0;   /* platform lacks d_type; caller falls back to stat */
#endif
}
#else
/* #966: same mapping from Windows' dwFileAttributes. */
static int fs_winattr_to_kind(DWORD attr) {
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return FS_STAT_KIND_SYMLINK;
    if (attr & FILE_ATTRIBUTE_DIRECTORY)     return FS_STAT_KIND_DIR;
    return FS_STAT_KIND_FILE;
}
#endif

// Directory listing
DirList* dir_list_raw(const char* path) {
    if (!path) return NULL;

    /* #462: a directory with many entries is an unbounded surface — a
     * sandboxed plugin can list a huge tree. Gate the DirList struct,
     * the entries array, and every entry name through the cap. */
    DirList* list = (DirList*)aether_caps_malloc(sizeof(DirList));
    if (!list) return NULL;
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
    list->kinds = NULL;   /* #966 */

    #ifdef _WIN32
    WIN32_FIND_DATAA find_data;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    HANDLE hFind = FindFirstFileA(search_path, &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        return list;
    }

    // Capacity-doubling growth for the entries array. Without it every
    // readdir step reallocated + memcpy'd the whole list, turning an
    // N-entry directory into O(N^2) work.
    int cap = 0;
    do {
        if (strcmp(find_data.cFileName, ".") != 0 &&
            strcmp(find_data.cFileName, "..") != 0) {
            if (list->count >= cap) {
                int new_cap = cap ? cap * 2 : 16;
                char** new_entries = (char**)aether_caps_realloc(
                    list->entries, (size_t)cap * sizeof(char*),
                    (size_t)new_cap * sizeof(char*));
                if (!new_entries) break;
                list->entries = new_entries;
                /* #966: grow the parallel kinds array in lock-step. */
                int* new_kinds = (int*)aether_caps_realloc(
                    list->kinds, (size_t)cap * sizeof(int),
                    (size_t)new_cap * sizeof(int));
                if (!new_kinds) break;
                list->kinds = new_kinds;
                cap = new_cap;
                list->capacity = new_cap;
            }
            char* name_copy = fs_caps_strdup(find_data.cFileName);
            if (!name_copy) break;
            list->kinds[list->count] =                                   /* #966 */
                fs_winattr_to_kind(find_data.dwFileAttributes);
            list->entries[list->count++] = name_copy;
        }
    } while (FindNextFileA(hFind, &find_data));

    FindClose(hFind);
    #else
    DIR* dir = opendir(path);
    if (!dir) return list;

    int cap = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            if (list->count >= cap) {
                int new_cap = cap ? cap * 2 : 16;
                char** new_entries = (char**)aether_caps_realloc(
                    list->entries, (size_t)cap * sizeof(char*),
                    (size_t)new_cap * sizeof(char*));
                if (!new_entries) break;
                list->entries = new_entries;
                /* #966: grow the parallel kinds array in lock-step. */
                int* new_kinds = (int*)aether_caps_realloc(
                    list->kinds, (size_t)cap * sizeof(int),
                    (size_t)new_cap * sizeof(int));
                if (!new_kinds) break;
                list->kinds = new_kinds;
                cap = new_cap;
                list->capacity = new_cap;
            }
            char* name_copy = fs_caps_strdup(entry->d_name);
            if (!name_copy) break;
            list->kinds[list->count] = fs_dtype_to_kind(entry->d_type);  /* #966 */
            list->entries[list->count++] = name_copy;
        }
    }

    closedir(dir);
    #endif

    return list;
}

int dir_list_count(DirList* list) {
    return list ? list->count : 0;
}

const char* dir_list_get(DirList* list, int index) {
    if (!list || index < 0 || index >= list->count) return NULL;
    return list->entries[index];
}

/* #966: file kind of entry `index`, from readdir's d_type. Out-of-range
 * or a list built before kinds were tracked reports 0 (unknown), so a
 * caller can safely fall back to stat. */
int dir_list_kind(DirList* list, int index) {
    if (!list || !list->kinds || index < 0 || index >= list->count) return 0;
    return list->kinds[index];
}

void dir_list_free(DirList* list) {
    if (!list) return;

    /* #462: free through the cap. Each entry name was minted by
     * fs_caps_strdup (strlen+1 bytes); the array is `capacity` slots. */
    for (int i = 0; i < list->count; i++) {
        char* e = list->entries[i];
        if (e) aether_caps_free(e, strlen(e) + 1);
    }
    if (list->entries)
        aether_caps_free(list->entries, (size_t)list->capacity * sizeof(char*));
    /* #966: the parallel kinds array is `capacity` ints. */
    if (list->kinds)
        aether_caps_free(list->kinds, (size_t)list->capacity * sizeof(int));
    aether_caps_free(list, sizeof(DirList));
}

// --- Glob: pattern matching for file discovery ---

#ifndef _WIN32
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

// Helper: add a path to a DirList
static void dirlist_add(DirList* list, const char* path) {
    /* #462: grow-by-one through the cap. capacity tracks the slot count
     * so dir_list_free releases the array with its exact size. */
    char** new_entries = (char**)aether_caps_realloc(
        list->entries, (size_t)list->capacity * sizeof(char*),
        (size_t)(list->count + 1) * sizeof(char*));
    if (!new_entries) return;
    list->entries = new_entries;
    list->capacity = list->count + 1;
    list->entries[list->count] = fs_caps_strdup(path);
    list->count++;
}

#ifdef _WIN32
// Simple glob-style pattern match for Windows (replaces fnmatch).
// Supports '*' (any sequence) and '?' (any single char).
// When the pattern does NOT start with '.', a leading dot in the name
// is not matched by '*' — mirroring FNM_PERIOD / POSIX semantics.
static int win_fnmatch(const char* pattern, const char* name) {
    const char* p = pattern;
    const char* n = name;
    const char* star_p = NULL;
    const char* star_n = NULL;

    // FNM_PERIOD semantics: if pattern doesn't start with '.',
    // a leading dot in name must not be matched by '*' or '?'.
    if (n[0] == '.' && p[0] != '.') return 0;

    while (*n) {
        if (*p == '*') {
            star_p = ++p;
            star_n = n;
            continue;
        }
        if (*p == '?' || *p == *n) {
            p++;
            n++;
            continue;
        }
        if (star_p) {
            p = star_p;
            n = ++star_n;
            continue;
        }
        return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

// Recursive walk for ** patterns (Windows).
static void walk_recursive(const char* dir, const char* suffix_pattern, DirList* result) {
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        // Skip '.' and '..'
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0'))) {
            continue;
        }

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip dot-prefixed directories (.git, .aeb, .vscode, …)
            if (fd.cFileName[0] == '.') continue;
            walk_recursive(fullpath, suffix_pattern, result);
        } else {
            if (win_fnmatch(suffix_pattern, fd.cFileName)) {
                dirlist_add(result, fullpath);
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}
#else
// Recursive walk for ** patterns (POSIX).
// Skips '.' and '..', and skips dot-prefixed directories (e.g. .git, .aeb)
// from recursion — but matches dot-prefixed FILES against the suffix pattern.
// Without this, patterns like "**/.build.ae" or "**/.*.ae" would never find
// dot-prefixed config files.
static void walk_recursive(const char* dir, const char* suffix_pattern, DirList* result) {
    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        // Always skip '.' and '..'
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            // Skip dot-prefixed directories (.git, .aeb, .vscode, …) from
            // recursion. Most build/config systems keep these as opaque
            // metadata, and the existing aeb scan excluded them too.
            if (entry->d_name[0] == '.') continue;
            walk_recursive(fullpath, suffix_pattern, result);
        } else {
            // Match suffix pattern against the file name. Use FNM_PERIOD
            // to require explicit leading-dot matching for dot-prefixed
            // files — that matches POSIX shell-glob expectations and
            // means a pattern like ".*.ae" picks up ".build.ae" while
            // "*.ae" still doesn't.
            if (fnmatch(suffix_pattern, entry->d_name, FNM_PERIOD) == 0) {
                dirlist_add(result, fullpath);
            }
        }
    }
    closedir(d);
}
#endif

DirList* fs_glob_raw(const char* pattern) {
    if (!pattern) return NULL;

    DirList* result = (DirList*)aether_caps_malloc(sizeof(DirList)); /* #462 */
    if (!result) return NULL;
    result->entries = NULL;
    result->count = 0;
    result->capacity = 0;
    result->kinds = NULL;   /* #966: globs carry no d_type; kind stays unknown */

#ifdef _WIN32
    // Check for ** (recursive glob)
    const char* dstar = strstr(pattern, "/**/");
    if (dstar) {
        char dir[4096];
        int dirlen = (int)(dstar - pattern);
        if (dirlen == 0) {
            strcpy(dir, ".");
        } else {
            strncpy(dir, pattern, dirlen);
            dir[dirlen] = '\0';
        }
        const char* suffix = dstar + 4;  // skip "/**/"

        // `**` matches zero-or-more directories, so files directly in the
        // base dir must match too. walk_recursive already matches files at
        // every level it visits — including the top-level `dir` — so a
        // separate base-dir scan here would re-emit every root-level match
        // (issue #1279). Rely solely on the walk.
        walk_recursive(dir, suffix, result);
    } else {
        // Simple glob (no **). FindFirstFileA matches against the full
        // `pattern` but its WIN32_FIND_DATA::cFileName is the BARE file name,
        // with no directory component. POSIX glob(3) returns full paths
        // (`subdir/foo.c`), so to keep parity we must reattach the pattern's
        // directory prefix — otherwise a Windows caller globbing `dir/*.c`
        // gets back `foo.c` it can't open relative to CWD (issue #1367).
        // Split the leading directory off `pattern` at the last separator
        // (Windows accepts both '/' and '\\').
        const char* last_slash = strrchr(pattern, '/');
        const char* last_bslash = strrchr(pattern, '\\');
        const char* sep = (last_bslash > last_slash) ? last_bslash : last_slash;
        size_t dir_prefix_len = sep ? (size_t)(sep - pattern) + 1 : 0; // includes the separator

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (dir_prefix_len > 0) {
                        char full[4096];
                        // Copy the pattern's dir prefix verbatim (preserving the
                        // caller's separator), then the bare match name.
                        if (dir_prefix_len < sizeof(full)) {
                            memcpy(full, pattern, dir_prefix_len);
                            snprintf(full + dir_prefix_len,
                                     sizeof(full) - dir_prefix_len,
                                     "%s", fd.cFileName);
                            dirlist_add(result, full);
                        }
                    } else {
                        dirlist_add(result, fd.cFileName);
                    }
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    // Check for ** (recursive glob)
    const char* dstar = strstr(pattern, "/**/");
    if (dstar) {
        // Split: prefix is the directory, suffix is the file pattern
        // e.g., "src/**/*.c" → dir="src", suffix="*.c"
        char dir[4096];
        int dirlen = (int)(dstar - pattern);
        if (dirlen == 0) {
            strcpy(dir, ".");
        } else {
            strncpy(dir, pattern, dirlen);
            dir[dirlen] = '\0';
        }
        const char* suffix = dstar + 4;  // skip "/**/"

        // `**` matches zero-or-more directories, so files directly in the
        // base dir must match too. walk_recursive already matches files at
        // every level it visits — including the top-level `dir` — so it
        // covers the zero-depth case on its own. A separate base-dir glob
        // here would re-emit every root-level match (issue #1279), so we
        // rely solely on the walk.
        walk_recursive(dir, suffix, result);
    } else {
        // Simple glob (no **)
        glob_t g;
        if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                dirlist_add(result, g.gl_pathv[i]);
            }
            globfree(&g);
        }
    }
#endif

    return result;
}

// Multi-pattern glob: takes a list of patterns, returns merged DirList.
// The list is an ArrayList (from std.list) containing string pointers.
extern int list_size(void*);
extern void* list_get_raw(void*, int);
// String-ABI accessor: list_get_raw may return a magic AetherString*
// (heap-built) or a raw const char* (literal/args_get). Unwrap so we
// hand fs_glob_raw the payload pointer, not the struct header (#688).
extern const char* aether_string_data(const void*);

DirList* fs_glob_multi_raw(void* pattern_list) {
    if (!pattern_list) return NULL;

    DirList* result = (DirList*)aether_caps_malloc(sizeof(DirList)); /* #462 */
    if (!result) return NULL;
    result->entries = NULL;
    result->count = 0;
    result->capacity = 0;
    result->kinds = NULL;   /* #966: globs carry no d_type; kind stays unknown */

    int n = list_size(pattern_list);
    for (int i = 0; i < n; i++) {
        const char* pattern = aether_string_data(list_get_raw(pattern_list, i));
        if (!pattern) continue;

        DirList* partial = fs_glob_raw(pattern);
        if (!partial) continue;

        for (int j = 0; j < partial->count; j++) {
            dirlist_add(result, partial->entries[j]);
        }
        dir_list_free(partial);
    }

    return result;
}

/* ===================================================================
 * #632 — Lexical path ops: path_clean, path_rel, path_is_within_base.
 *
 * Pure string operations (no filesystem access). Match Go's
 * filepath.Clean / Rel semantics — that's the reference every
 * porter checks against, and a one-byte deviation in `..` resolution
 * is a directory-traversal CVE.
 *
 * POSIX semantics:
 *   - collapse `//`, `./`
 *   - resolve `..` against a segment stack (drop the parent)
 *   - preserve a leading `/`
 *   - preserve UNRESOLVED leading `..` on a relative path
 *     (`../../x` cleans to `../../x`, NOT `x`)
 *   - empty input → "."
 *   - trailing `/` is dropped except for the root `/` itself
 * =================================================================== */

/* Walks `path` segment-by-segment using a stack of (start, len) pairs
 * into `path`. Tracks how many leading `..` we couldn't resolve
 * (relative paths only). Reconstructs into a freshly-malloc'd
 * NUL-terminated string the caller frees. */
char* path_clean(const char* path) {
    if (!path) return strdup(".");

    size_t in_len = strlen(path);
    if (in_len == 0) return strdup(".");

    /* #1369: a Windows volume prefix ("C:", "\\server\share") is not a
     * segment and must be copied through untouched; rootedness is then the
     * separator that follows it, so "C:\a" is absolute while "C:a" is
     * drive-relative. On POSIX vol is always 0 and this reduces to the
     * original leading-'/' test. */
    size_t vol = path_volume_len(path, in_len);
    int rooted = (vol < in_len && path_is_sep(path[vol]));

    /* Segment stack — at most one entry per byte of input. */
    struct seg { size_t start; size_t len; };
    /* #462: proportional to the (plugin-supplied) path length — gate it;
     * internal/transient, freed through the cap with this same size on
     * every exit. */
    size_t stk_bytes = sizeof(struct seg) * (in_len + 1);
    struct seg* stk = (struct seg*)aether_caps_malloc(stk_bytes);
    if (!stk) return NULL;
    size_t sp = 0;
    /* Count of leading `..` we couldn't resolve (relative paths only). */
    size_t leading_dotdot = 0;

    size_t i = rooted ? vol + 1 : vol;
    while (i < in_len) {
        /* Skip consecutive separators. */
        while (i < in_len && path_is_sep(path[i])) i++;
        if (i >= in_len) break;
        size_t start = i;
        while (i < in_len && !path_is_sep(path[i])) i++;
        size_t len = i - start;
        /* "." — skip. */
        if (len == 1 && path[start] == '.') continue;
        /* ".." — pop or push. */
        if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (sp > leading_dotdot) {
                /* Can pop a real segment. */
                sp--;
            } else if (!rooted) {
                /* Relative path with no resolvable parent: preserve. */
                stk[sp].start = start;
                stk[sp].len = 2;
                sp++;
                leading_dotdot++;
            }
            /* Rooted: ".." at the root is dropped (POSIX). */
            continue;
        }
        /* Regular segment. */
        stk[sp].start = start;
        stk[sp].len = len;
        sp++;
    }

    /* Total output length: volume + leading separator + segments joined. */
    size_t total = vol + (rooted ? 1 : 0);
    for (size_t k = 0; k < sp; k++) {
        if (k > 0) total += 1;  /* separator */
        total += stk[k].len;
    }
    /* Nothing left: the volume root if rooted, else "." (kept after a volume,
     * so "C:" cleans to "C:." — a drive-relative current directory). */
    if (sp == 0) {
        char* empty = (char*)malloc(vol + 2);
        if (!empty) { aether_caps_free(stk, stk_bytes); return NULL; }
        memcpy(empty, path, vol);
        if (rooted) {
            empty[vol] = PATH_SEP_CHAR;
            empty[vol + 1] = '\0';
        } else {
            empty[vol] = '.';
            empty[vol + 1] = '\0';
        }
        aether_caps_free(stk, stk_bytes);
        return empty;
    }

    char* out = (char*)malloc(total + 1);
    if (!out) { aether_caps_free(stk, stk_bytes); return NULL; }
    size_t o = 0;
    memcpy(out, path, vol);
    o += vol;
    if (rooted) out[o++] = PATH_SEP_CHAR;
    for (size_t k = 0; k < sp; k++) {
        if (k > 0) out[o++] = PATH_SEP_CHAR;
        memcpy(out + o, path + stk[k].start, stk[k].len);
        o += stk[k].len;
    }
    out[o] = '\0';
    aether_caps_free(stk, stk_bytes);
    return out;
}

/* Lexical containment: does the cleaned `target` lie within cleaned
 * `base`? Returns 1 yes, 0 no. Pure-string — never touches the
 * filesystem (no realpath, no stat). The right primitive for
 * pre-validation in a blob store / static-file server / archive
 * extractor: reject the request BEFORE you open(2) the path.
 *
 * Both paths are cleaned via path_clean first. After that:
 *   - identical → within
 *   - `target` starts with `base + "/"` → within
 *   - anything else → not within
 *
 * Treats `base == "/"` specially (any absolute target is within).
 * Symlinks are NOT followed — this is the lexical check. If a
 * symlink under `base` points outside, that's an open-time concern
 * separate from this function.
 */
int path_is_within_base(const char* base, const char* target) {
    if (!base || !target) return 0;
    char* cb = path_clean(base);
    char* ct = path_clean(target);
    if (!cb || !ct) { free(cb); free(ct); return 0; }
    size_t bl = strlen(cb);
    size_t tl = strlen(ct);

    /* #1369: compare with the platform's separator and case rules. Byte
     * comparison against a hardcoded '/' let a backslash path slip past the
     * boundary test on Windows, and path_clean not collapsing `..` there meant
     * an escaping target could be reported as contained. */
    int within = 0;
    size_t bvol = path_volume_len(cb, bl);
    if (bl == bvol + 1 && path_is_sep(cb[bvol]) &&
        path_span_equal(cb, ct, bvol)) {
        /* base is the volume root: any target on that volume is within. */
        within = (tl > bvol && path_is_sep(ct[bvol])) ? 1 : 0;
    } else if (tl == bl && path_span_equal(cb, ct, bl)) {
        within = 1;
    } else if (tl > bl && path_span_equal(cb, ct, bl) && path_is_sep(ct[bl])) {
        within = 1;
    }
    free(cb);
    free(ct);
    return within;
}

/* path_rel(base, target) — "what's the relative path from base to
 * target?" Matches Go filepath.Rel: returns a path that, joined onto
 * base and cleaned, equals target.
 *
 * Returns a caller-frees string on success, NULL when there's no
 * relative path (one is absolute and the other isn't, or target is
 * outside base on Windows-style drive boundaries — not a concern on
 * POSIX). For POSIX:
 *   - both must be absolute, or both relative
 *   - if base==target → "."
 *   - shared prefix gets dropped; remaining base segments → "..",
 *     remaining target segments stay
 *
 * Used by porters who want to show a path RELATIVE to a project
 * root in logs / errors / UI. Less security-critical than
 * is_within_base; the lexical computation is the same shape.
 */
char* path_rel(const char* base, const char* target) {
    if (!base || !target) return NULL;
    char* cb = path_clean(base);
    char* ct = path_clean(target);
    if (!cb || !ct) { free(cb); free(ct); return NULL; }
    size_t bl = strlen(cb);
    size_t tl = strlen(ct);
    /* #1369: a relative path between different volumes does not exist, and
     * mixing absolute with relative has no answer either. */
    size_t bvol = path_volume_len(cb, bl);
    size_t tvol = path_volume_len(ct, tl);
    if (bvol != tvol || !path_span_equal(cb, ct, bvol)) {
        free(cb); free(ct); return NULL;
    }
    int b_root = (bvol < bl && path_is_sep(cb[bvol]));
    int t_root = (tvol < tl && path_is_sep(ct[tvol]));
    if (b_root != t_root) {
        free(cb); free(ct); return NULL;
    }
    /* Find shared-prefix segments. */
    size_t bi = b_root ? bvol + 1 : bvol;
    size_t ti = t_root ? tvol + 1 : tvol;
    while (bi < bl && ti < tl) {
        /* Compare next segment in cb vs ct. */
        size_t bsa = bi, tsa = ti;
        while (bi < bl && !path_is_sep(cb[bi])) bi++;
        while (ti < tl && !path_is_sep(ct[ti])) ti++;
        if ((bi - bsa) != (ti - tsa) ||
            !path_span_equal(cb + bsa, ct + tsa, bi - bsa)) {
            /* Diverged — rewind to start of this segment. */
            bi = bsa;
            ti = tsa;
            break;
        }
        if (bi < bl && path_is_sep(cb[bi])) bi++;
        if (ti < tl && path_is_sep(ct[ti])) ti++;
    }
    /* `bi` is at the first byte of the unmatched base remainder,
     * `ti` at the first byte of the unmatched target remainder. */
    /* Count remaining base segments → that many ".." */
    size_t up = 0;
    {
        size_t p = bi;
        while (p < bl) {
            up++;
            while (p < bl && !path_is_sep(cb[p])) p++;
            if (p < bl) p++;
        }
    }
    /* Build output: up * "../" + (target remainder). */
    size_t out_cap = up * 3 + (tl - ti) + 2;
    char* out = (char*)malloc(out_cap);
    if (!out) { free(cb); free(ct); return NULL; }
    size_t o = 0;
    for (size_t u = 0; u < up; u++) {
        out[o++] = '.';
        out[o++] = '.';
        if (u + 1 < up || ti < tl) out[o++] = PATH_SEP_CHAR;
    }
    if (ti < tl) {
        memcpy(out + o, ct + ti, tl - ti);
        o += tl - ti;
    }
    if (o == 0) {
        out[o++] = '.';
    }
    out[o] = '\0';
    free(cb);
    free(ct);
    return out;
}

// ============================================================
// #977: recursive directory walk + filesystem change notification
// ============================================================

/* Layout-compatible view of the codegen's boxed `_AeClosure`
 * (see std/collections/aether_stringseq.c and codegen.c's prologue):
 *     typedef struct { void (*fn)(void); void* env; } _AeClosure;
 * `env` is the implicit first argument to `fn`. The box (and its env)
 * is malloc'd by _aether_box_closure and OWNED by the callee. */
typedef struct { void (*fn)(void); void* env; } AeFsClosure;

static void fs_closure_free(void* box) {
    if (!box) return;
    AeFsClosure* clo = (AeFsClosure*)box;
    if (clo->env) free(clo->env);
    free(box);
}

/* Walk paths are built into one shared heap buffer (append the entry name,
 * recurse, truncate back) so recursion costs no per-level path storage. */
#define FS_WALK_PATH_CAP 4096

/* Resolve an entry kind when the readdir sweep couldn't (DT_UNKNOWN — some
 * filesystems don't fill d_type). lstat so a symlink reports as symlink. */
static int fs_walk_stat_kind(const char* path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    return fs_winattr_to_kind(attr);
#else
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISLNK(st.st_mode))  return FS_STAT_KIND_SYMLINK;
    if (S_ISREG(st.st_mode))  return FS_STAT_KIND_FILE;
    if (S_ISDIR(st.st_mode))  return FS_STAT_KIND_DIR;
    return FS_STAT_KIND_OTHER;
#endif
}

/* Recursive worker. `buf` holds the current directory path (`len` bytes,
 * NUL-terminated); entries are appended as "/name" for the callback and the
 * buffer is truncated back after each. Returns 2 as soon as the callback
 * asks to stop (propagates all the way out), 0 otherwise. */
static int fs_walk_recurse(char* buf, size_t len, int depth,
                           int (*cb)(void*, const char*, int, int), void* env,
                           int* count) {
#ifdef _WIN32
    /* Build the search pattern in the shared path buffer (append the
     * slash-wildcard suffix, FindFirstFile copies it, restore) — no
     * per-recursion-level stack array, so deep trees cost no stack. */
    if (len + 2 >= FS_WALK_PATH_CAP) return 0;  /* path too deep — prune */
    buf[len] = '/';
    buf[len + 1] = '*';
    buf[len + 2] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(buf, &fd);
    buf[len] = '\0';
    if (h == INVALID_HANDLE_VALUE) return 0;    /* unreadable dir — skip */
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        size_t nlen = strlen(fd.cFileName);
        if (len + 1 + nlen >= FS_WALK_PATH_CAP) continue;  /* too long — prune */
        buf[len] = '/';
        memcpy(buf + len + 1, fd.cFileName, nlen + 1);
        int kind = fs_winattr_to_kind(fd.dwFileAttributes);
        (*count)++;
        int rc = cb(env, buf, kind, depth);
        if (rc == 2) { buf[len] = '\0'; FindClose(h); return 2; }
        if (rc != 1 && kind == FS_STAT_KIND_DIR) {
            if (fs_walk_recurse(buf, len + 1 + nlen, depth + 1,
                                cb, env, count) == 2) {
                buf[len] = '\0';
                FindClose(h);
                return 2;
            }
        }
        buf[len] = '\0';
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(buf);
    if (!dir) return 0;                         /* unreadable dir — skip */
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        size_t nlen = strlen(entry->d_name);
        if (len + 1 + nlen >= FS_WALK_PATH_CAP) continue;  /* too long — prune */
        buf[len] = '/';
        memcpy(buf + len + 1, entry->d_name, nlen + 1);
        int kind = fs_dtype_to_kind(entry->d_type);
        if (kind == 0) kind = fs_walk_stat_kind(buf);   /* DT_UNKNOWN fallback */
        (*count)++;
        int rc = cb(env, buf, kind, depth);
        if (rc == 2) { buf[len] = '\0'; closedir(dir); return 2; }
        if (rc != 1 && kind == FS_STAT_KIND_DIR) {
            /* Only genuine directories are entered — a symlink to a
             * directory reports kind 3 and is never followed (no cycles). */
            if (fs_walk_recurse(buf, len + 1 + nlen, depth + 1,
                                cb, env, count) == 2) {
                buf[len] = '\0';
                closedir(dir);
                return 2;
            }
        }
        buf[len] = '\0';
    }
    closedir(dir);
#endif
    return 0;
}

int fs_walk_raw(const char* root, void* cb_box) {
    if (!root || !cb_box) { fs_closure_free(cb_box); return -1; }
    if (!aether_sandbox_check("fs_read", root)) {
        fs_closure_free(cb_box);
        return -1;
    }
    size_t rlen = strlen(root);
    if (rlen == 0 || rlen >= FS_WALK_PATH_CAP) {
        fs_closure_free(cb_box);
        return -1;
    }
    int root_kind = fs_walk_stat_kind(root);
    if (root_kind == 0) { fs_closure_free(cb_box); return -1; }

    AeFsClosure clo = *(AeFsClosure*)cb_box;
    int (*cb)(void*, const char*, int, int) =
        (int (*)(void*, const char*, int, int))clo.fn;

    /* #462: the path buffer goes through the capability allocator like the
     * rest of the module's traversal storage. */
    char* buf = (char*)aether_caps_malloc(FS_WALK_PATH_CAP);
    if (!buf) { fs_closure_free(cb_box); return -1; }
    memcpy(buf, root, rlen + 1);
    /* Trim trailing separators so joined paths don't double the slash. */
    while (rlen > 1 && buf[rlen - 1] == '/') buf[--rlen] = '\0';

    int count = 1;
    int rc = cb(clo.env, buf, root_kind, 0);    /* the root is visited too */
    if (rc != 1 && rc != 2 && root_kind == FS_STAT_KIND_DIR) {
        fs_walk_recurse(buf, rlen, 1, cb, clo.env, &count);
    }

    aether_caps_free(buf, FS_WALK_PATH_CAP);
    fs_closure_free(cb_box);
    return count;
}

// ---- watch: coarse change notification on one directory (or file) ----

#if defined(__linux__)
#include <sys/inotify.h>
#include <poll.h>
typedef struct { int ifd; int wd; } AeFsWatch;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#include <sys/time.h>
typedef struct { int kq; int dirfd; } AeFsWatch;
#elif defined(_WIN32)
typedef struct { HANDLE h; } AeFsWatch;
#else
typedef struct { int unused; } AeFsWatch;
#endif

void* fs_watch_open_raw(const char* path) {
    if (!path) return NULL;
    if (!aether_sandbox_check("fs_read", path)) return NULL;

#if defined(__linux__)
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return NULL;
    int wd = inotify_add_watch(ifd, path,
                               IN_CREATE | IN_DELETE | IN_MODIFY |
                               IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                               IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd < 0) { close(ifd); return NULL; }
    AeFsWatch* w = (AeFsWatch*)aether_caps_malloc(sizeof(AeFsWatch));
    if (!w) { inotify_rm_watch(ifd, wd); close(ifd); return NULL; }
    w->ifd = ifd;
    w->wd = wd;
    return w;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    /* O_EVTONLY (macOS) opens for event monitoring without blocking
     * unmount; elsewhere a plain read-only descriptor serves. */
#ifdef O_EVTONLY
    int dirfd = open(path, O_EVTONLY);
#else
    int dirfd = open(path, O_RDONLY);
#endif
    if (dirfd < 0) return NULL;
    int kq = kqueue();
    if (kq < 0) { close(dirfd); return NULL; }
    struct kevent kev;
    EV_SET(&kev, dirfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE |
           NOTE_RENAME | NOTE_LINK,
           0, NULL);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        close(kq);
        close(dirfd);
        return NULL;
    }
    AeFsWatch* w = (AeFsWatch*)aether_caps_malloc(sizeof(AeFsWatch));
    if (!w) { close(kq); close(dirfd); return NULL; }
    w->kq = kq;
    w->dirfd = dirfd;
    return w;
#elif defined(_WIN32)
    HANDLE h = FindFirstChangeNotificationA(
        path, FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    AeFsWatch* w = (AeFsWatch*)aether_caps_malloc(sizeof(AeFsWatch));
    if (!w) { FindCloseChangeNotification(h); return NULL; }
    w->h = h;
    return w;
#else
    (void)path;
    return NULL;    /* no change-notification primitive on this platform */
#endif
}

int fs_watch_wait(void* watch, int timeout_ms) {
    if (!watch) return -1;
    AeFsWatch* w = (AeFsWatch*)watch;

#if defined(__linux__)
    struct pollfd pfd = { .fd = w->ifd, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, timeout_ms);   /* negative timeout = forever */
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    /* Drain everything queued so one burst of changes reports once. The
     * fd is non-blocking; read until EAGAIN. */
    char drain[4096];
    while (read(w->ifd, drain, sizeof(drain)) > 0) { }
    return 1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    struct kevent ev;
    int n;
    if (timeout_ms < 0) {
        n = kevent(w->kq, NULL, 0, &ev, 1, NULL);
    } else {
        struct timespec ts = { timeout_ms / 1000,
                               (long)(timeout_ms % 1000) * 1000000L };
        n = kevent(w->kq, NULL, 0, &ev, 1, &ts);
    }
    if (n < 0) return -1;
    return n > 0 ? 1 : 0;   /* EV_CLEAR resets the state after delivery */
#elif defined(_WIN32)
    DWORD t = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    DWORD r = WaitForSingleObject(w->h, t);
    if (r == WAIT_OBJECT_0) {
        FindNextChangeNotification(w->h);   /* re-arm for the next wait */
        return 1;
    }
    if (r == WAIT_TIMEOUT) return 0;
    return -1;
#else
    (void)timeout_ms;
    return -1;
#endif
}

void fs_watch_close(void* watch) {
    if (!watch) return;
    AeFsWatch* w = (AeFsWatch*)watch;
#if defined(__linux__)
    inotify_rm_watch(w->ifd, w->wd);
    close(w->ifd);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    close(w->kq);
    close(w->dirfd);
#elif defined(_WIN32)
    FindCloseChangeNotification(w->h);
#endif
    aether_caps_free(w, sizeof(AeFsWatch));
}

#endif // AETHER_HAS_FILESYSTEM

