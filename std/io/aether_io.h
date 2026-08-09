#ifndef AETHER_IO_H
#define AETHER_IO_H

#include <stddef.h>

// Console I/O
void io_print(const char* str);
void io_print_line(const char* str);
void io_print_int(int value);
void io_print_float(double value);

// File I/O (raw)
char* io_read_file_raw(const char* path);
int io_write_file_raw(const char* path, const char* content);
int io_append_file_raw(const char* path, const char* content);
int io_file_exists(const char* path);
int io_delete_file_raw(const char* path);

// File info
typedef struct {
    long size;
    int is_directory;
    long modified_time;
} FileInfo;

FileInfo* io_file_info_raw(const char* path);
void io_file_info_free(FileInfo* info);

// Environment variables
char* io_getenv(const char* name);
int io_setenv_raw(const char* name, const char* value);
int io_unsetenv_raw(const char* name);

// Unbuffered fd-1 / fd-2 writes — bypass stdio buffering so output is
// guaranteed on the wire before the process exits / crashes / aborts.
// `length` is the count of bytes to write; data may contain NULs.
// Returns the number of bytes written, or -1 on error. Loops on
// partial writes and retries on EINTR.
//
// `println` / `io.print` go through stdio (line-buffered on tty,
// block-buffered when stdout is piped) — fine for normal output but
// loses the last few lines if the process is killed mid-buffer.
// Reach for these in crash-trace paths where you need the bytes
// flushed NOW. Section A.3 (minimal scope) of aether_changes_needed.md.
int io_stderr_write_raw(const char* data, int length);
int io_stdout_write_raw(const char* data, int length);

// Error reporting off the current errno. Both must be called
// immediately after the failing libc call. perror prints
// "name: <message>\n" to stderr; errno_message returns the message
// as a heap string the caller owns (NULL only on OOM).
void  io_perror_raw(const char* name);
char* io_errno_message_raw(void);

// Tuple shapes returned by the fd-open / fd-read externs below.
// The struct field names (_0, _1, _2) mirror the codegen-emitted
// `_tuple_*_*_*` typedef synthesised from the spelled return-type
// of the matching `extern foo(...) -> (T1, T2, T3)` declaration.
// See std/fs/aether_fs.c's `_tuple_ptr_int_string` for prior art.
typedef struct {
    int _0;             // fd, or -1 on failure
    const char* _1;     // "" on success, error message on failure
} _tuple_int_string_io;

typedef struct {
    void* _0;           // AetherString* (cast for tuple ABI; "" on EOF/error)
    int _1;             // bytes actually read (0 on clean EOF / on error)
    const char* _2;     // "" on success/EOF, error message on failure
} _tuple_ptrintstr_io;

typedef struct {
    void* _0;           // AetherString* line content (no trailing '\n');
                        //   "" on EOF / read error / clean end-of-file
    const char* _1;     // "" on success or clean EOF, error message otherwise
} _tuple_ptrstr_io;

// File-descriptor lifecycle. `mode` is implicit in the function name:
// fd_open_read       → O_RDONLY
// fd_open_write      → O_WRONLY | O_CREAT | O_TRUNC, mode 0644 on POSIX
// Returns (fd, err): on success (fd >= 0, ""); on failure (-1, "...").
// fd_close returns "" on success, error message on failure.
// Section 1 of nuther-ask-of-aether-team.md.
_tuple_int_string_io io_fd_open_read_tuple(const char* path);
_tuple_int_string_io io_fd_open_write_tuple(const char* path);
const char* io_fd_close_raw(int fd);

// Looped fd write. Writes exactly `length` bytes (loops on partial
// writes; retries EINTR on POSIX). `data` may contain NULs.
// Returns 0 on success, -1 on error.
int io_fd_write_n(int fd, const char* data, int length);

// Looped fd read of up to `n` bytes. Reads until either `n` bytes
// have arrived, the fd hits EOF, or an error occurs. Returns
// (bytes, count, err): a refcounted AetherString of `count` bytes
// plus the actual byte count plus an error string ("" on success or
// clean EOF). On clean EOF after zero reads, count is 0 and err is
// "". On error, count is 0, bytes is empty, err describes the
// failure. The string is binary-safe (carries explicit length;
// embedded NULs survive).
_tuple_ptrintstr_io io_fd_read_n_tuple(int fd, int n);
/* Zero-allocation read into a caller-owned buffer (#1471). Returns the byte
 * count, 0 at EOF, or -1 on error. Returns as soon as ANY bytes are available
 * (unlike io_fd_read_n_tuple, which loops to fill) so it is usable on a live
 * pipe. */
int io_fd_read_into_raw(int fd, void* buf, int length);

// Read one '\n'-delimited line from `fd`. Trailing '\n' is stripped
// (a trailing '\r' before it is also stripped, so CRLF input yields
// CR-and-LF-stripped content). On clean EOF with no bytes pending
// returns ("", ""). On read error or EOF mid-line returns
// ("", "<reason>"). The returned AetherString is owned by the
// caller (refcount 1).
_tuple_ptrstr_io io_fd_read_line_tuple(int fd);

#endif // AETHER_IO_H

