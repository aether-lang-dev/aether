// ae - Unified Aether CLI tool
// The single entry point for the Aether programming language.
//
// Usage:
//   ae init <name>          Create a new Aether project
//   ae run [file.ae]        Compile and run a program
//   ae build [file.ae]      Compile to executable
//   ae test [file|dir]      Run tests
//   ae add <package>        Add a dependency
//   ae repl                 Start interactive REPL
//   ae fmt [file]           Format source code
//   ae version              Show version
//   ae help                 Show help

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>     // gc_stale_cache_tmp age gate (#1032)


#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>   // getpid() / _getpid() on MinGW and MSVC
#define PATH_SEP "\\"
#define EXE_EXT ".exe"
#define mkdir_p(path) _mkdir(path)
// MSVC uses _popen/_pclose; MinGW maps popen/pclose but be explicit
#ifndef popen
#  define popen  _popen
#  define pclose _pclose
#endif
// MinGW exposes getpid() in <process.h>; MSVC only has _getpid()
#ifndef getpid
#  define getpid _getpid
#endif
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <spawn.h>
#include <libgen.h>
#include <dirent.h>
#include <dlfcn.h>            /* `ae lib-info` opens a `--emit=lib` artifact via dlopen + dlsym */
#define PATH_SEP "/"
#define EXE_EXT ""
#define mkdir_p(path) mkdir(path, 0755)
extern char** environ;
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Shared header — AETHER_LIB_DIRS_MAX + AETHER_LIB_PATH_SEP_CHAR.
 * Pulled in early so the Toolchain struct can size its lib_dirs
 * array with the same cap the compiler enforces. Lives in
 * `compiler/aether_lib_path.h` (a tiny no-AST-deps header) so this
 * include is light. Issue #413. */
#include "../compiler/aether_lib_path.h"

#include "apkg/toml_parser.h"
#include "ae_help.h"
#include "ae_fmt.h"
#include "ae_bindgen.h"

// Version is set by Makefile from VERSION file
#ifndef AETHER_VERSION
#define AETHER_VERSION "0.0.0-dev"
#endif
#define AE_VERSION AETHER_VERSION

// --------------------------------------------------------------------------
// Cross-platform temp directory
// --------------------------------------------------------------------------
const char* get_temp_dir(void) {
#ifdef _WIN32
    const char* t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t) t = ".";
    return t;
#else
    const char* t = getenv("TMPDIR");
    if (t && t[0]) return t;
    return "/tmp";
#endif
}

// --------------------------------------------------------------------------
// Toolchain state
// --------------------------------------------------------------------------

#include "ae_internal.h"

Toolchain tc = {0};

/* Append a directory (or a separator-string of directories) to
 * `tc.lib_dirs`. Used by every `--lib <X>` flag site so that
 * repeated flags AND separator-strings both end up as discrete
 * entries in the list:
 *
 *    `--lib a --lib b`        → [a, b]
 *    `--lib a:b`              → [a, b]   (POSIX separator)
 *    `--lib "a;b"`            → [a, b]   (Windows separator)
 *    `--lib a:b --lib c`      → [a, b, c]
 *
 * Storing as a list (rather than a separator-string buffer) means
 * the aetherc command we build later emits one `--lib X` per entry
 * — no separator-string has to survive shell quoting through
 * system(). cmd.exe + MSYS2's joint handling of `;` inside double
 * quotes is uneven; one-flag-per-entry sidesteps the entire
 * surface. Dedup is O(N) over the cap-of-8 list. Issue #413. */
static void tc_lib_dir_append_one(const char* dir) {
    if (!dir || !dir[0]) return;
    /* Normalise trailing slash — matches the compiler-side
     * `module_add_lib_dir` normalisation so dedup catches
     * `./lib` vs `./lib/` cleanly. ALSO translate MSYS2 POSIX-form
     * paths (`/d/foo`) to native Windows form (`D:/foo`) so a
     * `;`-joined path-list and a sequence of flags end up
     * byte-identical regardless of how MSYS2 handled the argv.
     * `aether_lib_path_normalize` is a no-op on POSIX.
     *
     * memcpy with an explicit length (not `strncpy(dst, src,
     * sizeof(dst)-1)`) keeps GCC's `-Wstringop-truncation` happy
     * AND is the faster shape — single bulk copy of a known-good
     * byte count, no per-byte NUL scan inside libc. */
    char norm[256];
    aether_lib_path_normalize(dir, norm, sizeof(norm));
    size_t nlen = strlen(norm);
    while (nlen > 1 &&
           (norm[nlen - 1] == '/' || norm[nlen - 1] == '\\') &&
           norm[nlen - 2] != ':') {
        norm[--nlen] = '\0';
    }
    for (int i = 0; i < tc.lib_dir_count; i++) {
        if (strcmp(tc.lib_dirs[i], norm) == 0) return;
    }
    if (tc.lib_dir_count >= AETHER_LIB_DIRS_MAX) {
        fprintf(stderr,
            "warning: --lib search path is full (max %d entries); "
            "ignoring '%s'\n", AETHER_LIB_DIRS_MAX, norm);
        return;
    }
    int idx = tc.lib_dir_count;
    /* +1 carries the NUL. nlen is post-normalisation length,
     * always < sizeof(lib_dirs[idx]). Same warning + perf
     * rationale as above. */
    memcpy(tc.lib_dirs[idx], norm, nlen + 1);
    tc.lib_dir_count++;
}
void tc_lib_dir_append(const char* spec) {
    if (!spec || !spec[0]) return;
    /* Split on the platform separator and append each piece. Empty
     * segments (trailing/leading/double separators) are silently
     * skipped — matches Java -cp and PATH semantics. */
    const char* cur = spec;
    char buf[256];
    while (*cur) {
        const char* next = strchr(cur, AETHER_LIB_PATH_SEP_CHAR);
        size_t len = next ? (size_t)(next - cur) : strlen(cur);
        if (len > 0) {
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, cur, len);
            buf[len] = '\0';
            tc_lib_dir_append_one(buf);
        }
        if (!next) break;
        cur = next + 1;
    }
}

// --with=<caps> forwarded verbatim to aetherc. Empty by default; set
// by cmd_build's arg loop when the user passes `--with=fs` etc. Just
// a string because the aetherc side owns parsing and validation.
static char g_with_caps[128] = "";

// --emit=<exe|lib|both> for the current build. Set by cmd_build before
// build_aetherc_cmd / build_gcc_cmd run; both helpers read these globals
// to decide what flags to emit.
static bool g_emit_exe = true;
static bool g_emit_lib = false;
static bool g_emit_csrc = false;  // #996 --emit=csrc: emit .c + catalog .h, no gcc

// Extra link flags accumulated by the binary-import prepass: when a
// program `import`s a precompiled `--emit=lib` artifact (libfoo.so),
// `prepare_binary_imports` generates an Aether interface stub for it
// and records the .so path + rpath here so build_gcc_cmd links it.
// Empty for the common all-source build. POSIX-only (the prepass is
// gated on dlopen availability); stays empty on Windows.
#ifndef _WIN32
static char g_binimport_link[4096] = "";
#endif

// Extra link flags accumulated by the host-bridge import prepass: when
// a program `import`s `contrib.host.<lang>`, the bridge's static lib
// (libaether_host_<lang>.a) must be on the link line or the produced
// binary fails at runtime with `undefined symbol: <lang>_run` (the
// BRIDGE symbol, not the host language's). The user previously had to
// repeat themselves with `link_flags = "-laether_host_python"` in
// aether.toml — same information twice. Driven entirely by the import,
// so a pure-Aether program with no `contrib.host.*` imports does NOT
// link any bridge .a (critical: blanket-linking would force a
// hello-world binary to dlopen libpython at runtime). Empty unless
// `prepare_host_bridge_imports` found a match. POSIX-only (the host
// bridges aren't built / linked on Windows).
#ifndef _WIN32
static char g_host_bridge_link[2048] = "";
#endif

// Mirror of runtime/aether_lib_meta.h's catalog structs, kept
// layout-compatible so `ae` can dlopen a `--emit=lib` artifact and walk
// its `aether_lib_meta()` without including the runtime header. Used by
// both `ae lib-info` and the binary-import prepass below. Updates to the
// schema must touch BOTH this declaration and the canonical header.
typedef struct {
    const char* aether_name;
    const char* c_symbol;
    const char* signature;
    const char* source_file;
    int         source_line;
} _AeLibInfoFn;

typedef struct {
    const char* name;
    const char* type;
} _AeLibInfoCap;

typedef struct {
    const char* name;
    const char* role;
    const char* enclosing_export;
    const char* signature;
    int         capture_count;
    const _AeLibInfoCap* captures;
    const char* source_file;
    int         source_line;
} _AeLibInfoClosure;

typedef struct {
    const char* name;
    const char* type;
    const char* value;
} _AeLibInfoConst;

typedef struct {
    const char* schema_version;
    const char* aether_version;
    const char* primary_source;
    int         function_count;
    const _AeLibInfoFn* functions;
    int         closure_count;
    const _AeLibInfoClosure* closures;
    int         constant_count;
    const _AeLibInfoConst* constants;
} _AeLibInfoMeta;

// --coverage: when set, build_gcc_cmd appends `--coverage` to the gcc
// invocation so the resulting binary writes .gcda files when run, and
// .gcno files sit next to the .o. Pairs with `make ci-coverage` and
// the gcov-driven report under build/coverage/. The flag also forces
// the user-program build into -O0 -g (matching the COV_FLAGS pattern
// in the Makefile) so gcov line numbers don't get scrambled by
// optimisation.
static bool g_coverage = false;

// Build an aetherc command string with optional --lib flag
void build_aetherc_cmd(char* cmd, size_t cmd_size, const char* input, const char* output) {
    const char* emit_flag = "";
    if (g_emit_csrc)                   emit_flag = " --emit=csrc";
    else if (g_emit_lib && g_emit_exe) emit_flag = " --emit=both";
    else if (g_emit_lib)               emit_flag = " --emit=lib";
    // exe-only is the default; no flag needed.

    /* #996 --emit=csrc: also emit the catalog header (.h) and the machine-
     * readable JSON catalog (.catalog.json) alongside the .c. The header path
     * is the .c output with .c → .h (or +.h if no .c suffix); the JSON path
     * strips a trailing .c and appends .catalog.json. */
    char csrc_hdr_flag[PATH_MAX + 32] = "";
    char csrc_json_flag[PATH_MAX + 40] = "";
    if (g_emit_csrc && output) {
        char hpath[PATH_MAX];
        snprintf(hpath, sizeof(hpath), "%s", output);
        size_t hl = strlen(hpath);
        if (hl > 2 && hpath[hl-2] == '.' && hpath[hl-1] == 'c') {
            hpath[hl-1] = 'h';
        } else {
            snprintf(hpath + hl, sizeof(hpath) - hl, ".h");
        }
        snprintf(csrc_hdr_flag, sizeof(csrc_hdr_flag),
                 " --emit-catalog-header=%s", hpath);

        char jpath[PATH_MAX];
        snprintf(jpath, sizeof(jpath), "%s", output);
        size_t jl = strlen(jpath);
        if (jl > 2 && jpath[jl-2] == '.' && jpath[jl-1] == 'c') jpath[jl-2] = '\0';
        size_t jb = strlen(jpath);
        snprintf(jpath + jb, sizeof(jpath) - jb, ".catalog.json");
        snprintf(csrc_json_flag, sizeof(csrc_json_flag),
                 " --emit-catalog-json=%s", jpath);
    }

    // --with= is forwarded verbatim to aetherc, which owns parsing and
    // the reject messages. Only attached when non-empty so exe builds
    // don't see a spurious flag.
    char with_flag[160] = "";
    if (g_with_caps[0]) {
        snprintf(with_flag, sizeof(with_flag), " --with=%s", g_with_caps);
    }

    /* Emit one `--lib <dir>` per entry rather than a single
     * `--lib "a:b:c"` separator-string. Each arg is therefore a
     * plain directory path — survives cmd.exe, MSYS2, and any
     * other shell quoting without depending on `;` or `:`
     * preservation inside double quotes. Issue #413. */
    char lib_flags[2304] = "";
    size_t lf_off = 0;
    for (int i = 0; i < tc.lib_dir_count; i++) {
        int w = snprintf(lib_flags + lf_off, sizeof(lib_flags) - lf_off,
                         " --lib \"%s\"", tc.lib_dirs[i]);
        if (w < 0 || (size_t)w >= sizeof(lib_flags) - lf_off) break;
        lf_off += (size_t)w;
    }
    snprintf(cmd, cmd_size, "\"%s\"%s%s%s%s%s \"%s\" \"%s\"",
             tc.compiler, emit_flag, csrc_hdr_flag, csrc_json_flag, with_flag, lib_flags, input, output);
}

// --------------------------------------------------------------------------
// Utility functions
// --------------------------------------------------------------------------

#ifndef _WIN32
// Run a command via posix_spawnp (faster than system() — no /bin/sh overhead)
// Space-splits the command string into argv (no shell quoting supported,
// but our controlled commands never need it).
// quiet=0: show all output, quiet=1: hide stdout+stderr, quiet=2: hide stdout only (keep stderr for warnings)
static int posix_run(const char* cmd_str, int quiet) {
    if (tc.verbose) fprintf(stderr, "[cmd] %s\n", cmd_str);
    char buf[16384];
    strncpy(buf, cmd_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* toks[512];
    int n = 0;
    for (char* p = buf; *p && n < 511; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;  // skip opening quote
            toks[n++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';  // null-terminate and skip closing quote
        } else {
            toks[n++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
    }
    toks[n] = NULL;
    if (n == 0) return 0;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (quiet == 1) {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    } else if (quiet == 2) {
        // Hide stdout but keep stderr (so gcc warnings are visible)
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }

    pid_t pid;
    int ret = posix_spawnp(&pid, toks[0], &fa, NULL, toks, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (ret != 0) return -1;

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return -WTERMSIG(status);  // negative signal number
    return -1;
}
#endif

// Windows: use _spawnvp to avoid cmd.exe quoting issues with system()
#ifdef _WIN32
#include <process.h>
#include <io.h>
#ifndef _O_WRONLY
#define _O_WRONLY 1
#endif
/* `_O_BINARY` is in MinGW's <fcntl.h> but some compile-flag combos
 * (-D__STRICT_ANSI__, `-std=c11` without `_DEFAULT_SOURCE`, certain
 * MSYS2 mingw-w64 builds) gate it behind underscore-prefix macros
 * that aren't defined. Fall back to the literal MSVCRT value so the
 * `_setmode(_fileno(stdout), _O_BINARY)` LF-only output dance for
 * `ae lib-path` (#413 Windows follow-up) is portable across the
 * matrix. Same workaround pattern this section already uses for
 * `_O_WRONLY` above. */
#ifndef _O_BINARY
#define _O_BINARY 0x8000
#endif
static int win_run(const char* cmd_str, int quiet) {
    if (tc.verbose) fprintf(stderr, "[cmd] %s\n", cmd_str);
    char buf[16384];
    strncpy(buf, cmd_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Tokenize the command string into argv tokens for _spawnvp. Quoted
    // segments map to ONE token even when they contain spaces.
    //
    // toks[0] (the program name) is unquoted: _spawnvp wants a bare path.
    // toks[1..] are passed to the child verbatim — but MSVCRT's _spawnvp
    // joins them with single spaces to build the child's command line
    // WITHOUT any quoting of its own (documented MS behaviour). So a
    // token containing a space, if left bare in toks[], reaches the
    // child as multiple argv entries.  Wrap each non-program token that
    // contains a space in literal `"..."` so the child's CRT
    // command-line parser re-fuses it into one arg.  (Args that
    // themselves contain a `"` are not handled — the caller's quoting
    // convention at the cmd_str layer already doesn't support those.)
    char* toks[512];
    int n = 0;
    // Backing store for re-quoted tokens. Sized 2× the input buffer so a
    // worst-case input where every byte is part of a quoted token still
    // fits (each token grows by 2 bytes of `"..."` wrapper).
    char qbuf[32768];
    int qoff = 0;
    for (char* p = buf; *p && n < 511; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        char* tok_start;
        int had_quotes = 0;
        if (*p == '"') {
            had_quotes = 1;
            p++;
            tok_start = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else {
            tok_start = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        // For the program name (toks[0]) and tokens with no spaces,
        // pass-through. For other tokens, store a re-quoted copy so
        // _spawnvp's space-join produces a cmdline the child can re-
        // tokenize correctly.
        int needs_quoting = 0;
        if (n > 0 && (had_quotes || strchr(tok_start, ' ') != NULL)) {
            needs_quoting = 1;
        }
        if (needs_quoting) {
            int len = (int)strlen(tok_start);
            if (qoff + len + 3 > (int)sizeof(qbuf)) {
                // Out of re-quote space — pass through and hope for the best.
                toks[n++] = tok_start;
            } else {
                char* dst = qbuf + qoff;
                dst[0] = '"';
                memcpy(dst + 1, tok_start, len);
                dst[len + 1] = '"';
                dst[len + 2] = '\0';
                toks[n++] = dst;
                qoff += len + 3;
            }
        } else {
            toks[n++] = tok_start;
        }
    }
    toks[n] = NULL;
    if (n == 0) return 0;

    // Redirect stdout/stderr for quiet modes
    int saved_stdout = -1, saved_stderr = -1;
    if (quiet == 1 || quiet == 2) {
        fflush(stdout);
        saved_stdout = _dup(1);
        int nul = _open("nul", _O_WRONLY);
        if (nul >= 0) { _dup2(nul, 1); _close(nul); }
    }
    if (quiet == 1) {
        fflush(stderr);
        saved_stderr = _dup(2);
        int nul = _open("nul", _O_WRONLY);
        if (nul >= 0) { _dup2(nul, 2); _close(nul); }
    }

    int ret = (int)_spawnvp(_P_WAIT, toks[0], (const char* const*)toks);

    // Restore
    if (saved_stdout >= 0) { _dup2(saved_stdout, 1); _close(saved_stdout); }
    if (saved_stderr >= 0) { _dup2(saved_stderr, 2); _close(saved_stderr); }

    return ret;
}
#endif

int run_cmd(const char* cmd) {
#ifndef _WIN32
    return posix_run(cmd, 0);
#else
    return win_run(cmd, 0);
#endif
}

// Run a command, suppressing all output (quiet mode)
int run_cmd_quiet(const char* cmd) {
#ifndef _WIN32
    return posix_run(cmd, 1);
#else
    return win_run(cmd, 1);
#endif
}

// Run a command, showing stderr (warnings) but hiding stdout
int run_cmd_show_warnings(const char* cmd) {
#ifndef _WIN32
    return posix_run(cmd, 2);
#else
    return win_run(cmd, 2);
#endif
}

// Validate that a path is safe for use in shell commands (no metacharacters)
static bool is_safe_path(const char* path) {
    if (!path) return false;
    for (const char* p = path; *p; p++) {
        // Reject shell metacharacters that could enable command injection
        if (*p == '`' || *p == '$' || *p == '|' || *p == ';' ||
            *p == '&' || *p == '\n' || *p == '\r' || *p == '\'' ||
            *p == '!' || *p == '(' || *p == ')') {
            return false;
        }
    }
    return true;
}

/* True when `path` names an existing regular file. Every caller probes
 * for a file (a compiler binary, a library, a source), and pairs this
 * with dir_exists where a directory is meant. The POSIX branch used
 * access(F_OK), which also succeeds for directories, so the same probe
 * answered differently per platform; both now mean "regular file". */
/* Build the source-fallback list from MANIFEST instead of a list kept by
 * hand. The hand-kept list had drifted to 13 of the 45 stdlib sources, so
 * a toolchain without libaether.a could not link most of std (missing
 * aether_alloc, aether_bytes, strbuilder, regex, worker and more).
 * MANIFEST is regenerated by every `make stdlib` and installed beside the
 * sources, so it cannot go stale. Returns 0 if it cannot be read, leaving
 * the caller's existing list in place. */
static int append_manifest_srcs(char* out, size_t out_sz,
                                const char* manifest_path, const char* base) {
    FILE* mf = fopen(manifest_path, "r");
    if (!mf) return 0;
    size_t pos = 0;
    out[0] = '\0';
    char line[512];
    while (fgets(line, sizeof(line), mf)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = '\0';
        if (!n || *p == '#') continue;
        int w = snprintf(out + pos, out_sz - pos, "\"%s/%s\" ", base, p);
        if (w < 0 || (size_t)w >= out_sz - pos) { fclose(mf); out[0] = '\0'; return 0; }
        pos += (size_t)w;
    }
    fclose(mf);
    return pos > 0;
}


bool path_exists(const char* path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

// Validate a string contains only safe characters for shell commands.
// Allows: alphanumeric, '.', '/', '-', '_', '@'
static bool is_safe_shell_arg(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '/' ||
            c == '-' || c == '_' || c == '@') continue;
        return false;
    }
    return true;
}

bool dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdirs(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            mkdir_p(tmp);
            *p = sep;
        }
    }
    mkdir_p(tmp);
}

// Stream-copy src → dst, preserving the source file's permission bits
// so executables stay executable and libs stay non-executable. Returns
// 1 on success, 0 on any I/O failure. Used by the build cache to
// materialise a cached binary at the user-requested output path (and
// the inverse to store a freshly built binary in the cache slot).
static int copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return 0;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[8192];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    fclose(out);
#ifndef _WIN32
    if (ok) {
        struct stat src_st;
        if (stat(src, &src_st) == 0) {
            chmod(dst, src_st.st_mode & 07777);
        }
    }
#endif
    return ok;
}

static char* get_basename(const char* path) {
    const char* fslash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    const char* base = (!fslash) ? bslash : (!bslash) ? fslash : (fslash > bslash ? fslash : bslash);
    if (!base) base = path; else base++;
    static char result[256];
    strncpy(result, base, sizeof(result) - 1);
    result[sizeof(result) - 1] = '\0';
    char* dot = strrchr(result, '.');
    if (dot) *dot = '\0';
    return result;
}

// Get directory containing this executable
/* Absolute path of the running `ae` binary itself. Besides seeding
 * get_exe_dir, compute_cache_key folds this file's mtime into the key:
 * the key already covered aetherc's mtime, but a rebuilt `ae` (whose
 * codegen-driving flags such as -Wformat live here) served stale
 * binaries until `ae cache clear`. */
bool get_exe_path(char* buf, size_t size) {
#ifdef __APPLE__
    uint32_t sz = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buf, resolved)) {
            strncpy(buf, resolved, size - 1);
            buf[size - 1] = '\0';
            return true;
        }
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len > 0) {
        buf[len] = '\0';
        return true;
    }
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if (len > 0 && len < (DWORD)size) {
        buf[len] = '\0';
        return true;
    }
#endif
    return false;
}

static bool get_exe_dir(char* buf, size_t size) {
    if (!get_exe_path(buf, size)) return false;
    char* slash = strrchr(buf, '/');
#ifdef _WIN32
    char* bslash = strrchr(buf, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (!slash) return false;
    *slash = '\0';
    return true;
}

// --------------------------------------------------------------------------
// Toolchain discovery
// --------------------------------------------------------------------------

// GCC's -Wformat-truncation flags the runtime_srcs snprintf because it
// multiplies the theoretical max of each %s arg (1023 bytes) by 34 copies,
// exceeding the buffer.  In practice src is ~30-50 bytes and snprintf
// truncates safely, so suppress the false positive.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

// ---------------------------------------------------------------------------
// Recursive directory walker — emits `-I<path>` for `root` and every
// subdirectory it contains, space-separated, into `out`. Used to build
// `tc.include_flags` dynamically rather than maintaining a hardcoded
// list (issue #329 follow-on item 2). The hardcoded list silently
// missed `std/bytes`, `std/cryptography`, `std/zlib`, `std/dl`,
// `std/config`, `std/actors`, and the entire `std/http*` tree as
// those modules landed; the walker doesn't.
//
// Returns 1 on success, 0 if the buffer would overflow (caller can
// surface that as a fatal error — 4 KiB is enough for any reasonable
// install layout, and overflow means the layout grew beyond what
// `tc.include_flags` can hold).
// ---------------------------------------------------------------------------

static int append_include_one_dir(char* out, size_t out_size, size_t* pos, const char* path) {
    size_t path_len = strlen(path);
    // " -I<path>" needs path_len + 4 bytes plus the NUL.
    size_t need = (*pos == 0 ? 0 : 1) + 2 + path_len + 1;
    if (*pos + need >= out_size) return 0;
    if (*pos != 0) out[(*pos)++] = ' ';
    out[(*pos)++] = '-';
    out[(*pos)++] = 'I';
    memcpy(out + *pos, path, path_len);
    *pos += path_len;
    out[*pos] = '\0';
    return 1;
}

static int walk_dirs_emit_includes(const char* root, char* out, size_t out_size, size_t* pos) {
    if (!root || !*root) return 1;
    // Emit the root itself first.
    if (!append_include_one_dir(out, out_size, pos, root)) return 0;

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        const char* name = fd.cFileName;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s\\%s", root, name);
        if (!walk_dirs_emit_includes(child, out, out_size, pos)) {
            FindClose(h);
            return 0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(root);
    if (!d) return 1;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", root, name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        if (!walk_dirs_emit_includes(child, out, out_size, pos)) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
#endif
    return 1;
}

static void discover_toolchain(void) {
    char exe_dir[1024] = {0};
    bool found_exe_dir = get_exe_dir(exe_dir, sizeof(exe_dir));

    // Strategy 1: Dev mode — ae sitting next to aetherc in build/.
    // Checked first so that ./build/ae always uses ./build/aetherc,
    // even when $AETHER_HOME points to an older installed version.
    // GUARD: The installed layout also has aetherc next to ae (in bin/),
    // so we verify that the parent directory contains runtime/ (repo root)
    // rather than lib/ or share/ (installed prefix).
    //
    // Path-construction note: we compose the runtime probe as
    // `<parent_dir>/runtime` (where parent_dir = exe_dir with the last
    // component stripped), NOT `<exe_dir>/../runtime`. Windows native
    // stat() does NOT canonicalise mid-path `..` reliably on MSYS2's
    // mingw-w64 build — `D:\a\aether\aether\build\..\runtime` was
    // failing stat() in CI even though the directory exists, because
    // the kernel was being handed a literal path with `..` in the
    // middle and mixed slashes. Same fix for `tc.root`. POSIX
    // tolerates mid-path `..` in stat(), so this is a no-op there.
    if (found_exe_dir) {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/aetherc" EXE_EXT, exe_dir);
        if (path_exists(candidate)) {
            char parent_dir[1024];
            strncpy(parent_dir, exe_dir, sizeof(parent_dir) - 1);
            parent_dir[sizeof(parent_dir) - 1] = '\0';
            char* tail_sep = strrchr(parent_dir, '/');
            if (!tail_sep) tail_sep = strrchr(parent_dir, '\\');
            if (tail_sep) *tail_sep = '\0';
            char runtime_dir[1024];
            snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime", parent_dir);
            if (dir_exists(runtime_dir)) {
                strncpy(tc.root, parent_dir, sizeof(tc.root) - 1);
                tc.root[sizeof(tc.root) - 1] = '\0';
                strncpy(tc.compiler, candidate, sizeof(tc.compiler) - 1);
                tc.dev_mode = true;
                goto found_root;
            }
        }
    }

    // Strategy 2: $AETHER_HOME
    const char* home = getenv("AETHER_HOME");
    static char home_clean[1024];
    if (home) {
        strncpy(home_clean, home, sizeof(home_clean) - 1);
        home_clean[sizeof(home_clean) - 1] = '\0';
        size_t len = strlen(home_clean);
        while (len > 0 && (home_clean[len-1] == '\r' || home_clean[len-1] == '\n' || home_clean[len-1] == ' '))
            home_clean[--len] = '\0';
        home = home_clean;
    }
    if (home && home[0] && dir_exists(home)) {
        // Prefer ~/.aether/current/ if a version symlink exists (ae version use)
        char current_compiler[1024];
        snprintf(current_compiler, sizeof(current_compiler), "%s/current/bin/aetherc" EXE_EXT, home);
        if (path_exists(current_compiler)) {
            // Verify the installation has lib or share/aether — if neither,
            // the version was installed with a buggy ae that only extracted bin/.
            char share_probe[1024], lib_probe[1024];
            snprintf(share_probe, sizeof(share_probe), "%s/current/share/aether", home);
            snprintf(lib_probe, sizeof(lib_probe), "%s/current/lib/aether/libaether.a", home);
            if (dir_exists(share_probe) || path_exists(lib_probe)) {
                snprintf(tc.root, sizeof(tc.root), "%s/current", home);
                strncpy(tc.compiler, current_compiler, sizeof(tc.compiler) - 1);
                if (tc.verbose) fprintf(stderr, "[toolchain] compiler=%s (via current symlink)\n", tc.compiler);
                goto found_root;
            }
            // Check if the direct ~/.aether/ layout will work before warning —
            // install.sh puts files directly in AETHER_HOME, not under current/.
            char direct_share[1024], direct_lib[1024];
            snprintf(direct_share, sizeof(direct_share), "%s/share/aether", home);
            snprintf(direct_lib, sizeof(direct_lib), "%s/lib/aether/libaether.a", home);
            if (!dir_exists(direct_share) && !path_exists(direct_lib)) {
                fprintf(stderr, "Warning: %s/current has bin/aetherc but no lib/ or share/ — installation is incomplete.\n", home);
                fprintf(stderr, "Fix with: ae version install <version> or ./install.sh\n");
            }
            // Fall through to try other strategies
        }
        snprintf(current_compiler, sizeof(current_compiler), "%s/current/aetherc" EXE_EXT, home);
        if (path_exists(current_compiler)) {
            // Flat layout: aetherc at root of current/ with no bin/ subdirectory.
            // This is a broken install (old ae version install bug). Check if
            // share/aether/ exists — if not, warn and skip so we fall through
            // to a working toolchain.
            char share_check[1024];
            snprintf(share_check, sizeof(share_check), "%s/current/share/aether", home);
            if (dir_exists(share_check)) {
                snprintf(tc.root, sizeof(tc.root), "%s/current", home);
                strncpy(tc.compiler, current_compiler, sizeof(tc.compiler) - 1);
                if (tc.verbose) fprintf(stderr, "[toolchain] compiler=%s (via current symlink, flat layout)\n", tc.compiler);
                goto found_root;
            }
            // Also check for lib
            char lib_check[1024];
            snprintf(lib_check, sizeof(lib_check), "%s/current/lib/aether/libaether.a", home);
            if (path_exists(lib_check)) {
                snprintf(tc.root, sizeof(tc.root), "%s/current", home);
                strncpy(tc.compiler, current_compiler, sizeof(tc.compiler) - 1);
                goto found_root;
            }
            // Check if the direct ~/.aether/ layout will work before warning
            char direct_share2[1024], direct_lib2[1024];
            snprintf(direct_share2, sizeof(direct_share2), "%s/share/aether", home);
            snprintf(direct_lib2, sizeof(direct_lib2), "%s/lib/aether/libaether.a", home);
            if (!dir_exists(direct_share2) && !path_exists(direct_lib2)) {
                fprintf(stderr, "Warning: %s/current has aetherc but no lib/ or share/ — installation is incomplete.\n", home);
                fprintf(stderr, "Fix with: ae version install <version> or ./install.sh\n");
            }
            // Fall through to try other strategies
        }
        strncpy(tc.root, home, sizeof(tc.root) - 1);
        snprintf(tc.compiler, sizeof(tc.compiler), "%s/bin/aetherc" EXE_EXT, tc.root);
        if (tc.verbose) fprintf(stderr, "[toolchain] compiler=%s exists=%d\n", tc.compiler, path_exists(tc.compiler));
        if (path_exists(tc.compiler)) {
            // Verify AETHER_HOME has sources or lib — otherwise build will fail
            char share_check[1024], lib_check[1024];
            snprintf(share_check, sizeof(share_check), "%s/share/aether", home);
            snprintf(lib_check, sizeof(lib_check), "%s/lib/aether/libaether.a", home);
            if (dir_exists(share_check) || path_exists(lib_check)) {
                goto found_root;
            }
            // AETHER_HOME is incomplete — fall through to other strategies
        }
    }

    // Strategy 3: Relative to ae binary — installed layout ($PREFIX/bin/ae)
    // Detect installed layout by checking for lib/aether/ (canonical install
    // path) or share/aether/ (release ZIP).
    if (found_exe_dir) {
        char candidate[1024];
        bool is_installed = false;
        snprintf(candidate, sizeof(candidate), "%s/../lib/aether", exe_dir);
        if (dir_exists(candidate)) is_installed = true;
        if (!is_installed) {
            snprintf(candidate, sizeof(candidate), "%s/../share/aether", exe_dir);
            if (dir_exists(candidate)) is_installed = true;
        }
        if (is_installed) {
            // If a 'current' symlink exists (from ae version use), prefer it
            // so that version-managed stdlib files take priority over stale
            // files left by a previous install.sh in the parent directory.
            char current_root[1024];
            snprintf(current_root, sizeof(current_root), "%s/../current", exe_dir);
            if (dir_exists(current_root)) {
                char cs[1024], cl[1024];
                snprintf(cs, sizeof(cs), "%s/../current/share/aether", exe_dir);
                snprintf(cl, sizeof(cl), "%s/../current/lib/aether/libaether.a", exe_dir);
                if (dir_exists(cs) || path_exists(cl)) {
                    snprintf(tc.root, sizeof(tc.root), "%s/../current", exe_dir);
                    snprintf(tc.compiler, sizeof(tc.compiler), "%s/aetherc" EXE_EXT, exe_dir);
                    if (path_exists(tc.compiler)) goto found_root;
                }
            }
            snprintf(tc.root, sizeof(tc.root), "%s/..", exe_dir);
            snprintf(tc.compiler, sizeof(tc.compiler), "%s/aetherc" EXE_EXT, exe_dir);
            if (path_exists(tc.compiler)) goto found_root;
        }
    }

    // Strategy 4: CWD dev mode — ./build/aetherc
    if (path_exists("build/aetherc" EXE_EXT)) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            strncpy(tc.root, cwd, sizeof(tc.root) - 1);
        } else {
            strcpy(tc.root, ".");
        }
        snprintf(tc.compiler, sizeof(tc.compiler), "%s/build/aetherc" EXE_EXT, tc.root);
        tc.dev_mode = true;
        goto found_root;
    }

    // Strategy 5: Standard install paths
    const char* standard_paths[] = {
        "/usr/local/bin/aetherc",
        "/usr/bin/aetherc",
        NULL
    };
    for (int i = 0; standard_paths[i]; i++) {
        if (path_exists(standard_paths[i])) {
            strncpy(tc.compiler, standard_paths[i], sizeof(tc.compiler) - 1);
            tc.compiler[sizeof(tc.compiler) - 1] = '\0';
            strncpy(tc.root, standard_paths[i], sizeof(tc.root) - 1);
            char* slash = strrchr(tc.root, '/');
            if (slash) *slash = '\0';
            slash = strrchr(tc.root, '/');
            if (slash) *slash = '\0';
            goto found_root;
        }
    }

    fprintf(stderr, "Error: Aether compiler not found.\n");
#ifdef _WIN32
    fprintf(stderr, "\n");
    fprintf(stderr, "If you downloaded a release ZIP, make sure to:\n");
    fprintf(stderr, "  1. Extract the ZIP (e.g. to C:\\aether)\n");
    fprintf(stderr, "  2. Add C:\\aether\\bin to your PATH\n");
    fprintf(stderr, "  3. Restart your terminal\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Or set AETHER_HOME to the extraction folder:\n");
    fprintf(stderr, "  set AETHER_HOME=C:\\aether\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Download: https://github.com/nicolasmd87/aether/releases\n");
#else
    fprintf(stderr, "Run 'make compiler' to build it, or set $AETHER_HOME.\n");
#endif
    exit(1);

found_root:
    // Propagate AETHER_HOME to child processes (aetherc) so module
    // resolution works even when the shell environment is not configured.
#ifdef _WIN32
    {
        char env_buf[1100];
        snprintf(env_buf, sizeof(env_buf), "AETHER_HOME=%s", tc.root);
        _putenv(env_buf);
    }
#else
    setenv("AETHER_HOME", tc.root, 0);
#endif

    if (tc.verbose) {
        fprintf(stderr, "[toolchain] root: %s\n", tc.root);
        fprintf(stderr, "[toolchain] compiler: %s\n", tc.compiler);
        fprintf(stderr, "[toolchain] dev_mode: %s\n", tc.dev_mode ? "yes" : "no");
    }

    // Check for precompiled library. Canonical install layout (both
    // `make install` and `install.sh`) places it under lib/aether/, so
    // contrib archives (libaether_<x>.a) live alongside it; downstream
    // consumers must pass `-L<prefix>/lib/aether` on the link line, or
    // use `ae cflags` which emits the right -L automatically.
    if (tc.dev_mode) {
        snprintf(tc.lib, sizeof(tc.lib), "%s/build/libaether.a", tc.root);
    } else {
        snprintf(tc.lib, sizeof(tc.lib), "%s/lib/aether/libaether.a", tc.root);
        /* #959: prefer the nested canonical archive, but fall back to a flat
         * lib/libaether.a if that's all the package shipped. The flat archive
         * is complete; the from-source fallback below is NOT (it omits the
         * io-poller, sandbox/capsicum, http2, string_seq, and caps runtime
         * sources), so a package that placed the archive flat — as the macOS
         * arm64 v0.331/0.332 packages did — silently produced an unlinkable
         * binary ("Undefined symbols ... _aether_io_poller_init"). Take the
         * complete flat archive over the incomplete source compile. */
        if (!path_exists(tc.lib)) {
            char flat[1024];
            snprintf(flat, sizeof(flat), "%s/lib/libaether.a", tc.root);
            if (path_exists(flat)) {
                snprintf(tc.lib, sizeof(tc.lib), "%s/lib/libaether.a", tc.root);
            }
        }
    }
    tc.has_lib = path_exists(tc.lib);

    if (tc.verbose) {
        fprintf(stderr, "[toolchain] lib: %s (%s)\n", tc.lib,
                tc.has_lib ? "found" : "not found, using source fallback");
    }

    /* Toolchain-consistency check (aeb-ae-help-and-toolchain-feedback.md
     * #3). `make install` stamps the installed version into
     * lib/aether/VERSION next to libaether.a. When `ae` ends up
     * resolving a runtime archive whose stamp disagrees with the
     * compiler's own version — the classic split where a stale
     * `current` symlink shadows an older version dir — the only
     * symptom is a link failure with `undefined reference to
     * aether_*`, which points nowhere near the cause. Surface the
     * mismatch up front. Skipped in dev mode (build/ carries no
     * stamp, and the compiler + archive are always built together
     * there) and when the stamp is absent — absent means "cannot
     * check", not "mismatch", so the check never breaks an install
     * that predates the marker. */
    if (!tc.dev_mode && tc.has_lib) {
        char ver_path[1024];
        snprintf(ver_path, sizeof(ver_path), "%s/lib/aether/VERSION", tc.root);
        FILE* vf = fopen(ver_path, "r");
        if (vf) {
            char stamp[64] = {0};
            if (fgets(stamp, sizeof(stamp), vf)) {
                size_t sl = strlen(stamp);
                while (sl > 0 && (stamp[sl - 1] == '\n' || stamp[sl - 1] == '\r' ||
                                  stamp[sl - 1] == ' '  || stamp[sl - 1] == '\t')) {
                    stamp[--sl] = '\0';
                }
                if (sl > 0 && strcmp(stamp, AE_VERSION) != 0) {
                    fprintf(stderr,
                        "warning: toolchain version mismatch\n"
                        "  ae / aetherc : %s\n"
                        "  libaether.a  : %s  (%s)\n"
                        "  The runtime archive disagrees with the compiler. A link\n"
                        "  failure with 'undefined reference to aether_*' below means\n"
                        "  the archive predates the compiler — reinstall: make install\n",
                        AE_VERSION, stamp, tc.lib);
                }
            }
            fclose(vf);
        }
    }

    // Build include flags and source file lists.
    //
    // Dynamic walk over the runtime/ and std/ subtrees rather than the
    // hardcoded list this used to hold — the hardcoded version silently
    // dropped new modules as they landed (`std/bytes`, `std/cryptography`,
    // `std/zlib`, `std/dl`, `std/config`, `std/actors`, all of `std/http*`
    // were missing on `main` until #329 surfaced it). The walker can't
    // miss anything; new modules are picked up the next build.
    if (tc.dev_mode) {
        size_t pos = 0;
        tc.include_flags[0] = '\0';
        char rt[1024], stdroot[1024];
        snprintf(rt, sizeof(rt), "%s/runtime", tc.root);
        snprintf(stdroot, sizeof(stdroot), "%s/std", tc.root);
        if (!walk_dirs_emit_includes(rt, tc.include_flags, sizeof(tc.include_flags), &pos) ||
            !walk_dirs_emit_includes(stdroot, tc.include_flags, sizeof(tc.include_flags), &pos)) {
            // Buffer overflow — fall back to a minimal -I that gets
            // through the build. Caller will see warnings on missing
            // headers; the layout has outgrown the include_flags
            // capacity and needs to be bumped.
            fprintf(stderr,
                    "Warning: include-flag buffer overflow during dev-tree walk; some -I dirs dropped.\n");
        }

        if (!tc.has_lib) {
            char mpath[1200];
            snprintf(mpath, sizeof(mpath), "%s/build/MANIFEST", tc.root);
            if (!append_manifest_srcs(tc.runtime_srcs, sizeof(tc.runtime_srcs),
                                      mpath, tc.root))
            snprintf(tc.runtime_srcs, sizeof(tc.runtime_srcs),
                "%s/runtime/scheduler/multicore_scheduler.c "
                "%s/runtime/scheduler/scheduler_optimizations.c "
                "%s/runtime/config/aether_optimization_config.c "
                "%s/runtime/memory/aether_arena.c "
                "%s/runtime/memory/aether_pool.c "
                "%s/runtime/memory/aether_memory_stats.c "
                "%s/runtime/utils/aether_tracing.c "
                "%s/runtime/utils/aether_bounds_check.c "
                "%s/runtime/utils/aether_test.c "
                "%s/runtime/memory/aether_arena_optimized.c "
                "%s/runtime/aether_runtime_types.c "
                "%s/runtime/utils/aether_cpu_detect.c "
                "%s/runtime/utils/aether_simd_vectorized.c "
                "%s/runtime/aether_runtime.c "
                "%s/runtime/aether_numa.c "
                "%s/runtime/aether_host.c "
                "%s/runtime/actors/aether_send_buffer.c "
                "%s/runtime/actors/aether_send_message.c "
                "%s/runtime/actors/aether_actor_thread.c "
                "%s/runtime/actors/aether_panic.c "
                "%s/std/string/aether_string.c "
                "%s/std/math/aether_math.c "
                "%s/std/net/aether_http.c "
                "%s/std/net/aether_http_server.c "
                "%s/std/net/aether_http_pool.c "
                "%s/std/net/aether_net.c "
                "%s/std/net/aether_actor_bridge.c "
                "%s/std/collections/aether_collections.c "
                "%s/std/json/aether_json.c "
                "%s/std/io/aether_io.c "
                "%s/std/fs/aether_fs.c "
                "%s/std/log/aether_log.c "
                "%s/std/os/aether_os.c "
                "%s/std/collections/aether_set.c "
                "%s/std/collections/aether_pqueue.c "
                "%s/std/collections/aether_intarr.c "
                "%s/std/collections/aether_floatarr.c "
                "%s/std/collections/aether_longarr.c "
                "%s/std/collections/aether_bits.c "
                "%s/std/collections/aether_stringlist.c "
                "%s/std/collections/aether_stringseq.c",
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root, tc.root, tc.root, tc.root, tc.root,
                tc.root);
        }
    } else {
        // Installed layout: headers in include/aether/, source in
        // share/aether/. Walk both trees — include/ is the canonical
        // header location, share/ stays in the include-path while the
        // from-source fallback is supported (#329 is tracking the
        // longer-term question of dropping share/ source entirely).
        size_t pos = 0;
        tc.include_flags[0] = '\0';
        char inc_rt[1024], inc_std[1024], shr_rt[1024], shr_std[1024];
        snprintf(inc_rt,  sizeof(inc_rt),  "%s/include/aether/runtime", tc.root);
        snprintf(inc_std, sizeof(inc_std), "%s/include/aether/std",     tc.root);
        snprintf(shr_rt,  sizeof(shr_rt),  "%s/share/aether/runtime",   tc.root);
        snprintf(shr_std, sizeof(shr_std), "%s/share/aether/std",       tc.root);
        int ok =
            walk_dirs_emit_includes(inc_rt,  tc.include_flags, sizeof(tc.include_flags), &pos) &&
            walk_dirs_emit_includes(inc_std, tc.include_flags, sizeof(tc.include_flags), &pos) &&
            walk_dirs_emit_includes(shr_rt,  tc.include_flags, sizeof(tc.include_flags), &pos) &&
            walk_dirs_emit_includes(shr_std, tc.include_flags, sizeof(tc.include_flags), &pos);
        if (!ok) {
            fprintf(stderr,
                    "Warning: include-flag buffer overflow during installed-tree walk; some -I dirs dropped.\n");
        }

        // Source fallback: when libaether.a is not available, compile from share/aether/
        if (!tc.has_lib) {
            char src[1024];
            snprintf(src, sizeof(src), "%s/share/aether", tc.root);
            char mpath2[1200];
            snprintf(mpath2, sizeof(mpath2), "%s/MANIFEST", src);
            if (!append_manifest_srcs(tc.runtime_srcs, sizeof(tc.runtime_srcs),
                                      mpath2, src))
            snprintf(tc.runtime_srcs, sizeof(tc.runtime_srcs),
                "%s/runtime/scheduler/multicore_scheduler.c "
                "%s/runtime/scheduler/scheduler_optimizations.c "
                "%s/runtime/config/aether_optimization_config.c "
                "%s/runtime/memory/aether_arena.c "
                "%s/runtime/memory/aether_pool.c "
                "%s/runtime/memory/aether_memory_stats.c "
                "%s/runtime/utils/aether_tracing.c "
                "%s/runtime/utils/aether_bounds_check.c "
                "%s/runtime/utils/aether_test.c "
                "%s/runtime/memory/aether_arena_optimized.c "
                "%s/runtime/aether_runtime_types.c "
                "%s/runtime/utils/aether_cpu_detect.c "
                "%s/runtime/utils/aether_simd_vectorized.c "
                "%s/runtime/aether_runtime.c "
                "%s/runtime/aether_numa.c "
                "%s/runtime/aether_host.c "
                "%s/runtime/actors/aether_send_buffer.c "
                "%s/runtime/actors/aether_send_message.c "
                "%s/runtime/actors/aether_actor_thread.c "
                "%s/runtime/actors/aether_panic.c "
                "%s/std/string/aether_string.c "
                "%s/std/math/aether_math.c "
                "%s/std/net/aether_http.c "
                "%s/std/net/aether_http_server.c "
                "%s/std/net/aether_http_pool.c "
                "%s/std/net/aether_net.c "
                "%s/std/net/aether_actor_bridge.c "
                "%s/std/collections/aether_collections.c "
                "%s/std/json/aether_json.c "
                "%s/std/io/aether_io.c "
                "%s/std/fs/aether_fs.c "
                "%s/std/log/aether_log.c "
                "%s/std/os/aether_os.c "
                "%s/std/collections/aether_set.c "
                "%s/std/collections/aether_pqueue.c "
                "%s/std/collections/aether_intarr.c "
                "%s/std/collections/aether_floatarr.c "
                "%s/std/collections/aether_longarr.c "
                "%s/std/collections/aether_bits.c "
                "%s/std/collections/aether_stringlist.c "
                "%s/std/collections/aether_stringseq.c",
                src, src, src, src, src,
                src, src, src, src, src,
                src, src, src, src, src,
                src, src, src, src, src,
                src, src, src, src, src,
                src, src, src, src, src,
                src, src, src, src, src,
                src, src, src, src, src,
                src);
        }
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

// Expand ${VAR} occurrences in `src` against the process environment,
// writing into dst (NUL-terminated, truncated if needed). Unset vars
// expand to the empty string and emit a one-time stderr warning
// (clarity, not security — silent expansion to "" is the most
// surprising failure mode of this feature). `\$` is a literal `$`;
// bare `$VAR` (without braces) is NOT expanded — keeps the grammar
// tight and avoids ambiguity with `-L$LIB-foo`-style values. Shell
// `$(...)` command substitution is intentionally NOT supported
// (would let any aether.toml exec arbitrary commands at build time).
//
// SECURITY: name allowlist — only `AETHER_*` (uppercase letters,
// digits, underscore) is honoured. Anything else expands to empty
// + warning. The threat model: `aether.toml` is project-trusted,
// and the env var contribution is a side channel from whoever runs
// the build. By restricting names to a documented prefix, an
// attacker who can write arbitrary env (e.g. via a CI permission
// gap) can only hijack the contract this codebase declares — not
// e.g. ${PATH}, ${HOME}, or ${LD_PRELOAD}. The allowlist is
// deliberately conservative for v1; widening to a richer namespace
// can come later under review. Removing it entirely would not
// introduce a new RCE primitive (a hostile aether.toml can already
// put `-Wl,...` literally in link_flags), but the allowlist makes
// the *implicit* environment surface narrower and easier to audit.
//
// Why this is needed: aether.toml `[build] link_flags` / `cflags`
// values are copied verbatim into the gcc argv via posix_spawnp.
// No shell sees them, so `$(python3-config --ldflags --embed)` and
// raw `${VAR}` would reach gcc as literal text and fail. Containerised
// builds (aeb-ctr) probe the deploy host for host-specific runtime
// link flags and pass them in via env (-e VAR=…), so the toml needs
// a way to consume that. `${VAR}` is that way.
static bool env_var_name_allowed(const char* name, size_t n) {
    if (n < 8) return false;                       // "AETHER_" + ≥1
    if (memcmp(name, "AETHER_", 7) != 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

static void expand_env_vars(const char* src, char* dst, size_t dst_size) {
    if (dst_size == 0) return;
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; ) {
        if (src[si] == '\\' && src[si + 1] == '$') {
            dst[di++] = '$';
            si += 2;
            continue;
        }
        if (src[si] == '$' && src[si + 1] == '{') {
            const char* end = strchr(src + si + 2, '}');
            if (end) {
                char name[128];
                size_t n = (size_t)(end - (src + si + 2));
                if (n < sizeof(name)) {
                    memcpy(name, src + si + 2, n);
                    name[n] = '\0';
                    if (!env_var_name_allowed(name, n)) {
                        fprintf(stderr,
                            "ae: warning: aether.toml ${%s} not expanded "
                            "(only ${AETHER_*} env vars are honoured); "
                            "substituting empty string\n", name);
                    } else {
                        const char* v = getenv(name);
                        if (v) {
                            size_t vl = strlen(v);
                            size_t room = dst_size - 1 - di;
                            size_t cp = vl < room ? vl : room;
                            memcpy(dst + di, v, cp);
                            di += cp;
                        } else {
                            fprintf(stderr,
                                "ae: warning: aether.toml references "
                                "${%s} which is unset; substituting "
                                "empty string\n", name);
                        }
                    }
                    si = (size_t)(end - src) + 1;
                    continue;
                }
                // Name too long: fall through and copy the literal '$'.
            }
            // Unterminated ${: fall through and copy the literal '$'.
        }
        dst[di++] = src[si++];
    }
    dst[di] = '\0';
}

// Get link_flags from aether.toml [build] section
// Returns empty string if not found or no aether.toml.
// `${VAR}` occurrences are expanded against the process environment
// (see expand_env_vars()).
static const char* get_link_flags(void) {
    static char flags[1024] = "";
    static bool checked = false;

    if (checked) return flags;
    checked = true;

    if (!path_exists("aether.toml")) return flags;

    TomlDocument* doc = toml_parse_file("aether.toml");
    if (!doc) return flags;

    const char* val = toml_get_value(doc, "build", "link_flags");
    if (val) {
        expand_env_vars(val, flags, sizeof(flags));
    }

    toml_free_document(doc);
    return flags;
}

// --------------------------------------------------------------------------
// C-backend compiler override: honor $AE_CC then $CC (mirrors the Makefile's
// CC=). This selects the compiler that turns Aether's generated C into the
// final object / executable / library; it never affects aetherc (the
// Aether->C front end, selected via tc.compiler). Returns NULL when neither
// is set, so each platform keeps its existing default (gcc on POSIX,
// WinLibs/gcc on Windows).
// --------------------------------------------------------------------------
static const char* c_backend_env_override(void) {
    const char* cc = getenv("AE_CC");
    if (cc && *cc) return cc;
    cc = getenv("CC");
    if (cc && *cc) return cc;
    return NULL;
}

// --------------------------------------------------------------------------
// Windows: auto-install bundled GCC (WinLibs) if none found on PATH
// --------------------------------------------------------------------------
#ifdef _WIN32

// Pinned WinLibs release — GCC 14.2.0 UCRT, x86-64, no LLVM (~250 MB).
// Update WINLIBS_TAG + WINLIBS_ZIP together when upgrading.
#define WINLIBS_TAG "14.2.0posix-12.0.0-ucrt-r3"
#define WINLIBS_ZIP "winlibs-x86_64-posix-seh-gcc-14.2.0-mingw-w64ucrt-12.0.0-r3.zip"
#define WINLIBS_URL \
    "https://github.com/brechtsanders/winlibs_mingw/releases/download/" \
    WINLIBS_TAG "/" WINLIBS_ZIP

static char s_gcc_bin[1100] = "gcc";  // path to gcc; updated by ensure_gcc_windows()
static bool s_gcc_ready      = false; // set after first successful check

// Checks PATH, then ~/.aether/tools/, then downloads WinLibs on demand.
// Returns true when gcc is usable; false means the user must intervene.
static bool ensure_gcc_windows(void) {
    if (s_gcc_ready) return true;

    // 0. Explicit $AE_CC / $CC override wins over PATH and the WinLibs
    //    auto-download: the user picked the C-backend compiler, so trust it.
    const char* ov = c_backend_env_override();
    if (ov) {
        snprintf(s_gcc_bin, sizeof(s_gcc_bin), "%s", ov);
        s_gcc_ready = true;
        return true;
    }

    // 1. Already on PATH?
    if (system("gcc --version >nul 2>&1") == 0) {
        s_gcc_ready = true;
        return true;
    }

    // 2. Already installed to ~/.aether/tools/ from a previous run?
    const char* home  = get_home_dir();
    /* tools_bin/tools_gcc derive from tools_dir plus a fixed suffix;
     * sized a tier up so gcc's -Wformat-truncation heuristic (which
     * assumes the %s can fill its whole source buffer) stays quiet. */
    char tools_dir[1024], tools_bin[1100], tools_gcc[1100];
    snprintf(tools_dir, sizeof(tools_dir), "%s\\.aether\\tools",           home);
    snprintf(tools_bin, sizeof(tools_bin), "%s\\mingw64\\bin",             tools_dir);
    snprintf(tools_gcc, sizeof(tools_gcc), "%s\\mingw64\\bin\\gcc.exe",    tools_dir);

    struct stat st;
    if (stat(tools_gcc, &st) == 0) goto found;

    // 3. Auto-download (one-time, ~250 MB).
    printf("[ae] GCC not found. Downloading MinGW-w64 GCC (~250 MB) -- one-time setup...\n");
    fflush(stdout);

    mkdirs(tools_dir);  // Create ~/.aether/tools/ (and parents)

    // Write a tiny PowerShell script to avoid shell-quoting nightmares.
    char ps_path[1100], zip_path[1100];
    snprintf(ps_path,  sizeof(ps_path),  "%s\\install_gcc.ps1", tools_dir);
    snprintf(zip_path, sizeof(zip_path), "%s\\mingw.zip",        tools_dir);

    FILE* ps = fopen(ps_path, "w");
    if (!ps) {
        fprintf(stderr, "[ae] Cannot write installer script to %s\n", tools_dir);
        goto fail;
    }
    fprintf(ps,
        "$ProgressPreference = 'SilentlyContinue'\n"
        "Write-Host '[ae] Downloading GCC...'\n"
        "Invoke-WebRequest -Uri '%s' -OutFile '%s'\n"
        "Write-Host '[ae] Extracting...'\n"
        "Expand-Archive -Path '%s' -DestinationPath '%s' -Force\n"
        "Remove-Item -Path '%s' -Force\n"
        "Write-Host '[ae] GCC ready.'\n",
        WINLIBS_URL, zip_path, zip_path, tools_dir, zip_path);
    fclose(ps);

    {
        char run_ps[2048];
        snprintf(run_ps, sizeof(run_ps),
            "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps_path);
        int ret = system(run_ps);
        remove(ps_path);
        if (ret != 0 || stat(tools_gcc, &st) != 0) goto fail;
    }

found:
    // Add bundled bin dir to PATH for this process so gcc is found by name too.
    {
        char cur[8192] = "", updated[9400];
        GetEnvironmentVariableA("PATH", cur, sizeof(cur));
        snprintf(updated, sizeof(updated), "%s;%s", tools_bin, cur);
        SetEnvironmentVariableA("PATH", updated);
    }
    snprintf(s_gcc_bin, sizeof(s_gcc_bin), "%s", tools_gcc);
    s_gcc_ready = true;
    return true;

fail:
    fprintf(stderr, "[ae] GCC auto-install failed. Install it manually:\n");
    fprintf(stderr, "[ae]   Option A: WinLibs (easiest) — https://winlibs.com\n");
    fprintf(stderr, "[ae]             Extract the zip, add the bin\\ folder to PATH.\n");
    fprintf(stderr, "[ae]   Option B: MSYS2 — https://www.msys2.org\n");
    fprintf(stderr, "[ae]             pacman -S mingw-w64-x86_64-gcc\n");
    return false;
}

#endif // _WIN32

// Get cflags from aether.toml [build] section (applied only for release/ae-build)
// Returns empty string if not found or no aether.toml
const char* get_cflags(void) {
    static char flags[512] = "";
    static bool checked = false;

    if (checked) return flags;
    checked = true;

    if (!path_exists("aether.toml")) return flags;

    TomlDocument* doc = toml_parse_file("aether.toml");
    if (!doc) return flags;

    const char* val = toml_get_value(doc, "build", "cflags");
    if (val) {
        expand_env_vars(val, flags, sizeof(flags));
    }

    toml_free_document(doc);
    return flags;
}

// Get extra_sources for the [[bin]] entry whose path matches ae_file.
// Writes space-separated C source paths into out[out_size].
//
// Handles both single-line and multi-line array forms:
//
//     extra_sources = ["a.c", "b.c", "c.c"]
//
//     extra_sources = [
//         "a.c",
//         "b.c",
//         "c.c"
//     ]
//
// Continuation lines are the only way to stay readable past ~30
// filenames; before multi-line was supported, downstream projects
// would squash everything onto one line and hit the assembly
// buffer limit (v0.85 / the "tail entries dropped" fix).
//
// Returns 0 on clean fill, 1 if the `out` buffer was too small and at
// least one filename was silently truncated. Callers should warn in
// that case — the caller's subsequent `build_gcc_cmd` will hand the
// linker a mangled partial path ("ae/.../handler_copy_generat" was
// the real-world symptom that prompted this signature change) and
// the error message won't point at extra_sources as the culprit.
// Walk up from the current working directory looking for an
// `aether.toml`. If found in some ancestor directory `D`, chdir
// there and adjust the positional `*file_inout` (when relative) to
// resolve against `D`. Returns 1 on chdir, 0 when nothing was found
// or cwd already has the toml. Closes #280 (2).
//
// The cargo rule: only walk up when there's no toml in cwd. Users
// running `ae build foo.ae` from a subdirectory of a project get
// the project's toml found automatically and `foo.ae` re-resolved
// relative to the project root. Users with no project toml at all
// see no behaviour change.
static int find_and_chdir_to_aether_toml(const char** file_inout) {
    if (path_exists("aether.toml")) return 0;  /* already present */

    char start_cwd[1024];
    if (!getcwd(start_cwd, sizeof(start_cwd))) return 0;

    char walk[1024];
    strncpy(walk, start_cwd, sizeof(walk) - 1);
    walk[sizeof(walk) - 1] = '\0';

    /* Walk up to /. POSIX `dirname` mutates; compose by truncating
     * at the last '/'. Stop when we either find aether.toml or hit
     * the root. */
    while (1) {
        char probe[1040];
        snprintf(probe, sizeof(probe), "%s/aether.toml", walk);
        if (path_exists(probe)) {
            if (chdir(walk) != 0) return 0;
            /* Adjust the positional file argument: if it was a
             * relative path, prepend the original cwd's relationship
             * to the new cwd. e.g. starting at /home/p/proj/ae, after
             * chdir to /home/p/proj, a positional `myprobe.ae`
             * becomes `ae/myprobe.ae`. */
            if (file_inout && *file_inout) {
                const char* f = *file_inout;
                if (f[0] != '/' && f[0] != '\\') {
                    /* relative — splice the subdir we walked out of */
                    size_t walk_len = strlen(walk);
                    if (strncmp(start_cwd, walk, walk_len) == 0 &&
                        start_cwd[walk_len] == '/') {
                        const char* sub = start_cwd + walk_len + 1;
                        static char rebased[1024];
                        snprintf(rebased, sizeof(rebased), "%s/%s", sub, f);
                        *file_inout = rebased;
                    }
                }
            }
            return 1;
        }
        /* Step up one directory by truncating at the last '/'. Stop
         * when we hit the root marker (just "/" or empty). */
        char* slash = strrchr(walk, '/');
        if (!slash) break;
        if (slash == walk) {
            /* At "/X" — the parent is "/". One more probe at "/". */
            walk[1] = '\0';
            char root_probe[1040];
            snprintf(root_probe, sizeof(root_probe), "%s/aether.toml", walk);
            if (path_exists(root_probe) && chdir(walk) == 0) return 1;
            break;
        }
        *slash = '\0';
    }
    return 0;
}

// Look up a [[bin]] entry by `name = "..."`. If found, copy its
// `path = "..."` value into `out` and return 1. Returns 0 when no
// aether.toml exists in cwd, or when no [[bin]] matches the name.
//
// Lets users invoke `ae build <bin-name>` instead of having to type
// the underlying file path. Closes #280 (1).
static int find_bin_path_by_name(const char* bin_name, char* out, size_t out_size) {
    out[0] = '\0';
    if (!bin_name || !path_exists("aether.toml")) return 0;

    FILE* f = fopen("aether.toml", "r");
    if (!f) return 0;

    char line[1024];
    int in_bin = 0;
    int matched_name = 0;
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t ln = strlen(s);
        while (ln > 0 && (s[ln-1] == '\n' || s[ln-1] == '\r' || s[ln-1] == ' ')) s[--ln] = '\0';
        if (!s[0] || s[0] == '#') continue;

        if (strncmp(s, "[[bin]]", 7) == 0) {
            in_bin = 1;
            matched_name = 0;
            continue;
        }
        if (s[0] == '[' && s[1] != '[') {
            in_bin = 0;
            matched_name = 0;
            continue;
        }
        if (!in_bin) continue;

        if (strncmp(s, "name", 4) == 0 && strchr(s, '=')) {
            char* eq = strchr(s, '=') + 1;
            while (*eq == ' ') eq++;
            if (*eq == '"') eq++;
            char* end = strrchr(eq, '"');
            if (end) *end = '\0';
            if (strcmp(eq, bin_name) == 0) matched_name = 1;
            continue;
        }
        if (matched_name && strncmp(s, "path", 4) == 0 && strchr(s, '=')) {
            char* eq = strchr(s, '=') + 1;
            while (*eq == ' ') eq++;
            if (*eq == '"') eq++;
            char* end = strrchr(eq, '"');
            if (end) *end = '\0';
            strncpy(out, eq, out_size - 1);
            out[out_size - 1] = '\0';
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int get_extra_sources_for_bin(const char* ae_file, char* out, size_t out_size) {
    out[0] = '\0';
    if (!ae_file || !path_exists("aether.toml")) return 0;

    FILE* f = fopen("aether.toml", "r");
    if (!f) return 0;

    int truncated = 0;

    // 1 KiB was too small for projects with many extra_sources on one
    // logical line: `extra_sources = ["a.c", "b.c", ..., "zz.c"]`. fgets
    // silently truncates at the buffer boundary, dropping the tail of
    // the array and producing link errors for the omitted shims — no
    // warning, just "undefined reference to ..." at link time. 8 KiB
    // fits ~250 comma-separated filenames of average length; projects
    // hitting even that limit should switch to multi-line TOML arrays
    // (tracked separately — parser still only handles single-line).
    char line[8192];
    int in_bin = 0;
    int matched = 0;

    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t ln = strlen(s);
        while (ln > 0 && (s[ln-1] == '\n' || s[ln-1] == '\r' || s[ln-1] == ' ')) s[--ln] = '\0';
        if (!s[0] || s[0] == '#') continue;

        // [[bin]] section marker
        if (strncmp(s, "[[bin]]", 7) == 0) {
            in_bin = 1;
            matched = 0;
            continue;
        }

        // Other section resets context
        if (s[0] == '[' && s[1] != '[') {
            in_bin = 0;
            matched = 0;
            continue;
        }

        if (!in_bin) continue;

        // path = "..." — check if this bin entry matches ae_file
        if (strncmp(s, "path", 4) == 0 && strchr(s, '=')) {
            char* eq = strchr(s, '=') + 1;
            while (*eq == ' ') eq++;
            if (*eq == '"') eq++;
            char* end = strrchr(eq, '"');
            if (end) *end = '\0';
            // Normalize: strip leading "./"
            const char* aef = ae_file;
            if (aef[0] == '.' && aef[1] == '/') aef += 2;
            if (eq[0] == '.' && eq[1] == '/') eq += 2;
            // Match if aef == eq, or aef ends with "/<eq>" (handles
            // absolute and cwd-relative invocations of the same file).
            // The strict `alen > vlen` is required: with `alen == vlen`
            // and strings unequal, `aef[alen - vlen - 1]` underflows to
            // `aef[-1]` (size_t arithmetic wraps), which is an OOB read.
            size_t vlen = strlen(eq);
            size_t alen = strlen(aef);
            if (strcmp(aef, eq) == 0 ||
                (alen > vlen && aef[alen - vlen - 1] == '/' &&
                 strcmp(aef + alen - vlen, eq) == 0)) {
                matched = 1;
            }
            continue;
        }

        // extra_sources = ["a.c", "b.c"] in a matched [[bin]]. Accepts
        // both single-line arrays and multi-line arrays:
        //
        //   extra_sources = [
        //       "a.c",
        //       "b.c",
        //       "c.c"
        //   ]
        //
        // The parser is permissive: it ignores whitespace and commas
        // and keeps scanning lines until it finds the closing `]`. A
        // closing `]` in a quoted string would trip this, but that's
        // not a legitimate filename character anyway.
        if (matched && strncmp(s, "extra_sources", 13) == 0 && strchr(s, '=')) {
            char* eq = strchr(s, '=') + 1;
            while (*eq == ' ') eq++;
            if (*eq != '[') continue;
            eq++; // skip '['

            // Line-by-line loop. `frag` is the remaining unparsed
            // portion of the current line. We walk entries until we
            // hit the closing `]`; when we reach end-of-fragment
            // without finding it, we fgets the next line and keep
            // going. Continuation lines get the same whitespace +
            // comment strip as the outer loop.
            char* frag = eq;
            int closed = 0;
            int overflowed = 0;
            while (!closed) {
                // Consume entries in `frag` until `]` or end.
                while (*frag && *frag != ']') {
                    while (*frag == ' ' || *frag == ',' || *frag == '\t') frag++;
                    if (*frag == ']' || !*frag) break;
                    if (*frag == '"') {
                        frag++;
                        char* end = strchr(frag, '"');
                        if (!end) break;   // malformed — bail out
                        *end = '\0';
                        size_t cur = strlen(out);
                        size_t piece = strlen(frag);
                        size_t need = (out[0] ? 1 : 0) + piece + 1;
                        if (cur + need > out_size) {
                            truncated = 1;
                            overflowed = 1;
                            break;
                        }
                        if (out[0]) strncat(out, " ", out_size - cur - 1);
                        strncat(out, frag, out_size - strlen(out) - 1);
                        frag = end + 1;
                    } else {
                        frag++;
                    }
                }
                if (*frag == ']' || overflowed) {
                    closed = 1;
                    break;
                }
                // Continuation: pull the next line.
                if (!fgets(line, sizeof(line), f)) {
                    // Malformed TOML — unterminated array at EOF.
                    // Treat as end; don't block the build here.
                    closed = 1;
                    break;
                }
                char* t = line;
                while (*t == ' ' || *t == '\t') t++;
                size_t tln = strlen(t);
                while (tln > 0 && (t[tln-1] == '\n' || t[tln-1] == '\r' || t[tln-1] == ' ')) {
                    t[--tln] = '\0';
                }
                if (!*t || *t == '#') {
                    frag = t;   // empty line / comment — frag is "" so we fgets again next iter
                    continue;
                }
                frag = t;
            }
            break;
        }
    }
    fclose(f);
    return truncated;
}

// --------------------------------------------------------------------------
// Build GCC/MinGW command for linking an Aether-compiled C file
// Optimisation-and-instrumentation flag fragment for the gcc invocation.
// `optimize` picks -O2 vs -O0 -g; `g_coverage` overrides to -O0 -g and
// adds --coverage (gcc shorthand for -fprofile-arcs -ftest-coverage at
// compile + -lgcov at link). -O0 is load-bearing for coverage: at -O2
// gcc inlines / merges blocks, and the .gcov line attribution gets
// scrambled (a hit on line 7 might show up on line 9).
static const char* opt_flags(bool optimize) {
    /* -Wformat: with the interop lowering no longer casting literal format
     * strings to void* (#1252), the C compiler can check printf-family
     * extern calls; #line directives map its warning to the user's .ae
     * source. User cflags from aether.toml append after these flags, so
     * -Wno-format remains available to opt out. */
    if (g_coverage) return "-O0 -g --coverage -Wformat";
    return optimize ? "-O2 -Wformat" : "-O0 -g -Wformat";
}

void build_gcc_cmd(char* cmd, size_t size,
                          const char* c_file, const char* out_file,
                          bool optimize, const char* extra_files) {
    const char* link_flags = get_link_flags();
    const char* extra = extra_files ? extra_files : "";

    // User cflags from aether.toml apply to every build path — `ae build`,
    // `ae run`, and any internal invocation. Previously they were gated
    // behind `optimize` (only the release path picked them up), which
    // meant `-D<feature>` flags and warning-suppression that extern C
    // shims relied on silently broke `ae run`.
    const char* user_cflags = get_cflags();

#ifdef _WIN32
    // Ensure GCC is available (auto-downloads WinLibs on first run if needed).
    if (!ensure_gcc_windows()) {
        snprintf(cmd, size, "exit 1");  // will fail; error already printed
        return;
    }
    // Windows (MinGW): no -pthread (Win32 threads via aether_thread.h), no -lm (CRT).
    // -lws2_32 is required for Winsock2 (aether_http/net always compiled into runtime).
    // -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt are required when OpenSSL
    // is linked — static libssl/libcrypto pull in Win Crypto/GDI/Advapi
    // symbols. Always included so the link succeeds regardless of whether
    // the user's build ends up pulling OpenSSL in via std.net / std.cryptography.
    // openssl_libs / zlib_libs are baked in at `ae` build time from pkg-config
    // (same handling as the POSIX branch below); empty strings when the
    // library wasn't detected, in which case the stdlib wrappers fall into
    // their "unavailable" stubs at runtime.
    // -static links libwinpthread/libgcc into the binary so it runs without MinGW DLLs.
    // Quote s_gcc_bin in case the path contains spaces.
#ifdef AETHER_OPENSSL_LIBS
    const char* openssl_libs = AETHER_OPENSSL_LIBS;
#else
    const char* openssl_libs = "";
#endif
#ifdef AETHER_ZLIB_LIBS
    const char* zlib_libs = AETHER_ZLIB_LIBS;
#else
    const char* zlib_libs = "";
#endif
#ifdef AETHER_NGHTTP2_LIBS
    /* libnghttp2 powers the HTTP/2 server-side path (#260 Tier 2).
     * Empty when the build didn't detect nghttp2 — the server
     * surface stays valid (http_server_set_h2 returns the
     * "unavailable" sentinel) but the link doesn't pull the lib. */
    const char* nghttp2_libs = AETHER_NGHTTP2_LIBS;
#else
    const char* nghttp2_libs = "";
#endif
#ifdef AETHER_PCRE2_LIBS
    /* libpcre2-8 powers std.regex. Empty when not detected; the
     * std.regex surface stays valid and every entry point returns
     * a clean "built without libpcre2-8" via regex.last_error(). */
    const char* pcre2_libs = AETHER_PCRE2_LIBS;
#else
    const char* pcre2_libs = "";
#endif
#ifdef AETHER_AUDIO_LIBS
    /* std.audio's vendored miniaudio backend link flags (pthread/dl/m on
     * Linux, audio frameworks on macOS). Empty on platforms without them. */
    const char* audio_libs = AETHER_AUDIO_LIBS;
#else
    const char* audio_libs = "";
#endif
    char opt[600];
    if (user_cflags[0])
        snprintf(opt, sizeof(opt), "-static %s %s", opt_flags(optimize), user_cflags);
    else
        snprintf(opt, sizeof(opt), "-static %s", opt_flags(optimize));
    // -ldbghelp is required for the panic stack-trace path
    // (CaptureStackBackTrace is in kernel32 / always-linked, but
    // SymInitialize/SymFromAddr live in dbghelp). Issue #347.
    const char* win_link_libs = "-lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -ldbghelp";
    char lib_dir[1024];
    if (tc.has_lib) {
        strncpy(lib_dir, tc.lib, sizeof(lib_dir) - 1);
        lib_dir[sizeof(lib_dir) - 1] = '\0';
        char* bs = strrchr(lib_dir, '\\');
        char* fs = strrchr(lib_dir, '/');
        char* slash = (!bs) ? fs : (!fs) ? bs : (bs > fs ? bs : fs);
        if (slash) *slash = '\0';
        int w = snprintf(cmd, size,
            "\"%s\" %s %s \"%s\" %s -L\"%s\" -laether -o \"%s\" %s %s %s %s %s %s %s",
            s_gcc_bin, opt, tc.include_flags, c_file, extra, lib_dir, out_file, openssl_libs, zlib_libs, nghttp2_libs, pcre2_libs, audio_libs, win_link_libs, link_flags);
        if (w >= (int)size) {
            fprintf(stderr,
                "Warning: gcc link command truncated at %d bytes (buffer %zu).\n",
                w, size);
        }
    } else {
        int w = snprintf(cmd, size,
            "\"%s\" %s %s \"%s\" %s %s -o \"%s\" %s %s %s %s %s %s %s",
            s_gcc_bin, opt, tc.include_flags, c_file, extra, tc.runtime_srcs, out_file, openssl_libs, zlib_libs, nghttp2_libs, pcre2_libs, audio_libs, win_link_libs, link_flags);
        if (w >= (int)size) {
            fprintf(stderr,
                "Warning: gcc link command truncated at %d bytes (buffer %zu).\n",
                w, size);
        }
    }
#else
    // POSIX (Linux/macOS): -pthread for POSIX threads, -lm for math
    // C-backend compiler: honor $AE_CC then $CC (mirrors the Makefile's CC=),
    // else default to gcc. C-backend only; aetherc selection is untouched.
    const char* cc = c_backend_env_override();
    if (cc) {
        // Pre-flight the chosen compiler. CC may carry flags ("gcc -m32"), so
        // resolve only its first token via `command -v`.
        char first[256];
        size_t n = strcspn(cc, " \t");
        if (n >= sizeof(first)) n = sizeof(first) - 1;
        snprintf(first, sizeof(first), "%.*s", (int)n, cc);
        char probe[512];
        snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", first);
        if (system(probe) != 0) {
            fprintf(stderr, "Error: C compiler '%s' (from $%s) not found.\n",
                    first, (getenv("AE_CC") && *getenv("AE_CC")) ? "AE_CC" : "CC");
            snprintf(cmd, size, "false");
            return;
        }
    } else {
        cc = "gcc";
        // Pre-flight check: ensure gcc (or cc) is available
        if (system("command -v gcc >/dev/null 2>&1") != 0 &&
            system("command -v cc >/dev/null 2>&1") != 0) {
            fprintf(stderr, "Error: C compiler not found (gcc or cc).\n");
#ifdef __APPLE__
            fprintf(stderr, "Install Xcode Command Line Tools: xcode-select --install\n");
#else
            fprintf(stderr, "Install GCC: sudo apt install gcc  (Debian/Ubuntu)\n");
            fprintf(stderr, "             sudo dnf install gcc  (Fedora)\n");
#endif
            snprintf(cmd, size, "false");
            return;
        }
    }
    char opt[600];
    // --emit=lib adds -fPIC -shared so the output is loadable via dlopen.
    // --emit=both (exe + lib from one source) is not supported by this
    // helper — the caller should invoke it twice with different modes,
    // or a future refactor can produce both artifacts in one gcc call.
    //
    // #993: on Windows/MinGW also pass -Wl,--export-all-symbols so the
    // `aether_<name>` / @c_callback catalog exports are visible in the .dll
    // regardless of GCC's auto-export heuristic — which silently flips OFF the
    // moment any symbol (e.g. an --extra C shim) carries an explicit
    // __declspec(dllexport). On ELF/Mach-O the catalog symbols are exported by
    // default visibility, so the flag is Windows-only.
#ifdef _WIN32
    const char* emit_lib_flags = (g_emit_lib && !g_emit_exe)
        ? "-fPIC -shared -Wl,--export-all-symbols " : "";
#else
    const char* emit_lib_flags = (g_emit_lib && !g_emit_exe) ? "-fPIC -shared " : "";
#endif
    // Coverage builds skip -pipe — gcov works fine with it, but it
    // adds nothing when -O0 -g is already forced. Keeping the flag
    // string short helps the cmd-buffer size budget.
    const char* base_opt = g_coverage ? opt_flags(optimize)
                          : (optimize ? "-O2 -pipe" : "-O0 -g -pipe");
    if (user_cflags[0])
        snprintf(opt, sizeof(opt), "%s%s %s", emit_lib_flags, base_opt, user_cflags);
    else
        snprintf(opt, sizeof(opt), "%s%s", emit_lib_flags, base_opt);

    // Append aether_config.c to the compile when building a lib so the
    // aether_config_* accessors are bundled into the .so. The .c file
    // lives in runtime/ under dev mode and in include/aether/runtime/
    // (or similar) on installed toolchains.
    // config_c wraps `candidate` in ` "..."` — sized one tier up so
    // gcc's -Wformat-truncation heuristic doesn't fire on the
    // wrapper bytes (snprintf would truncate safely either way).
    char config_c[2056] = "";
    if (g_emit_lib) {
        char candidate[2048];
        snprintf(candidate, sizeof(candidate), "%s/runtime/aether_config.c", tc.root);
        if (path_exists(candidate)) {
            snprintf(config_c, sizeof(config_c), " \"%s\"", candidate);
        }
    }

    // Optional OpenSSL linker flags — baked in at `ae` build time from
    // pkg-config. When OpenSSL wasn't detected, this is an empty string
    // and HTTPS calls error cleanly at runtime.
#ifdef AETHER_OPENSSL_LIBS
    const char* openssl_libs = AETHER_OPENSSL_LIBS;
#else
    const char* openssl_libs = "";
#endif

    // Same story for zlib — used by std.zlib.deflate/inflate. Empty
    // when zlib wasn't detected; std.zlib wrappers then report
    // "zlib unavailable" at runtime.
#ifdef AETHER_ZLIB_LIBS
    const char* zlib_libs = AETHER_ZLIB_LIBS;
#else
    const char* zlib_libs = "";
#endif

    // libnghttp2 — HTTP/2 server-side path (#260 Tier 2). Empty
    // when nghttp2 wasn't detected; http_server_set_h2 then
    // returns "HTTP/2 unavailable: built without libnghttp2".
#ifdef AETHER_NGHTTP2_LIBS
    const char* nghttp2_libs = AETHER_NGHTTP2_LIBS;
#else
    const char* nghttp2_libs = "";
#endif

    // libpcre2-8 — std.regex (Perl-compatible regex with captures,
    // $-substitutions, Unicode). Empty when not detected; std.regex
    // surfaces a clean "built without libpcre2-8" via last_error().
#ifdef AETHER_PCRE2_LIBS
    const char* pcre2_libs = AETHER_PCRE2_LIBS;
#else
    const char* pcre2_libs = "";
#endif

    // libcasper + cap_* services — std.casper delegates DNS / passwd /
    // sysctl past Capsicum capability mode. FreeBSD-only; empty on
    // every other platform, where std.casper links its stub path.
#ifdef AETHER_CASPER_LIBS
    const char* casper_libs = AETHER_CASPER_LIBS;
#else
    const char* casper_libs = "";
#endif

    // std.audio — vendored miniaudio backend (pthread/dl/m on Linux, audio
    // frameworks on macOS). Empty on platforms without them.
#ifdef AETHER_AUDIO_LIBS
    const char* audio_libs = AETHER_AUDIO_LIBS;
#else
    const char* audio_libs = "";
#endif

    if (tc.has_lib) {
        char lib_dir[1024];
        strncpy(lib_dir, tc.lib, sizeof(lib_dir) - 1);
        lib_dir[sizeof(lib_dir) - 1] = '\0';
        char* slash = strrchr(lib_dir, '/');
        if (slash) *slash = '\0';

        /* -rdynamic on POSIX: adds the executable's static-linked
         * symbols (everything from libaether.a) to the dynamic
         * symbol table, so std.http.script_gateway and std.dl
         * can dlopen plugins that reference runtime symbols
         * (http_response_set_*, string_concat, etc.) without the
         * .so having to link against libaether itself. macOS gets
         * the same effect via -Wl,-export_dynamic which `-rdynamic`
         * maps to. Without this flag, a host built by `ae build`
         * would dlopen a script.so with RTLD_NOW and the resolver
         * would fail to find any libaether symbol — silently on
         * macOS via dynamic_lookup, hard-failing on Linux. */
        // Order matters: host-bridge .a files reference symbols in
        // libaether.a (aether_shared_map_*, etc.), so they must appear
        // BEFORE -laether on the link line — gcc resolves undefined
        // references left-to-right through static archives.
        int w = snprintf(cmd, size,
            "%s %s %s \"%s\"%s %s -rdynamic -L%s %s -laether -o \"%s\" -pthread -lm %s %s %s %s %s %s %s %s",
            cc, opt, tc.include_flags, c_file, config_c, extra, lib_dir, g_host_bridge_link, out_file, openssl_libs, zlib_libs, nghttp2_libs, pcre2_libs, casper_libs, audio_libs, link_flags, g_binimport_link);
        if (w >= (int)size) {
            fprintf(stderr,
                "Warning: gcc link command truncated at %d bytes (buffer %zu) — "
                "your extra_sources plus includes won't fit; rebuild `ae` with "
                "a larger cmd buffer or split into multiple [[bin]] entries.\n",
                w, size);
        }
    } else {
        // Order matters: host-bridge .a files reference runtime
        // symbols defined in tc.runtime_srcs (aether_shared_map_*,
        // etc.), so they appear BEFORE the runtime source list.
        int w = snprintf(cmd, size,
            "%s %s %s \"%s\"%s %s %s %s -rdynamic -o \"%s\" -pthread -lm %s %s %s %s %s %s %s %s",
            cc, opt, tc.include_flags, c_file, config_c, extra, g_host_bridge_link, tc.runtime_srcs, out_file, openssl_libs, zlib_libs, nghttp2_libs, pcre2_libs, casper_libs, audio_libs, link_flags, g_binimport_link);
        if (w >= (int)size) {
            fprintf(stderr,
                "Warning: gcc link command truncated at %d bytes (buffer %zu) — "
                "your extra_sources plus includes won't fit; rebuild `ae` with "
                "a larger cmd buffer or split into multiple [[bin]] entries.\n",
                w, size);
        }
    }
#endif
}

static int build_wasm_cmd(char* cmd, size_t size,
                          const char* c_file, const char* out_file) {
    // Build include paths from toolchain root
    char includes[8192];
    if (tc.include_flags[0]) {
        strncpy(includes, tc.include_flags, sizeof(includes) - 1);
        includes[sizeof(includes) - 1] = '\0';
    } else {
        static const char* include_dirs[] = {
            "runtime", "runtime/actors", "runtime/scheduler",
            "runtime/utils", "runtime/memory", "runtime/config",
            "std", "std/string", "std/io", "std/math",
            "std/net", "std/collections", "std/json", NULL
        };
        includes[0] = '\0';
        for (int i = 0; include_dirs[i]; i++) {
            char flag[2048];
            snprintf(flag, sizeof(flag), "-I%s/%s ", tc.root, include_dirs[i]);
            strncat(includes, flag, sizeof(includes) - strlen(includes) - 1);
        }
    }

    // Runtime source files (cooperative scheduler, not multicore)
    static const char* wasm_runtime_files[] = {
        "runtime/scheduler/aether_scheduler_coop.c",
        "runtime/scheduler/scheduler_optimizations.c",
        "runtime/config/aether_optimization_config.c",
        "runtime/memory/aether_arena.c",
        "runtime/memory/aether_pool.c",
        "runtime/memory/aether_memory_stats.c",
        "runtime/utils/aether_tracing.c",
        "runtime/utils/aether_bounds_check.c",
        "runtime/utils/aether_test.c",
        "runtime/memory/aether_arena_optimized.c",
        "runtime/aether_runtime_types.c",
        "runtime/utils/aether_cpu_detect.c",
        "runtime/utils/aether_simd_vectorized.c",
        "runtime/aether_runtime.c",
        "runtime/aether_numa.c",
        "runtime/actors/aether_send_buffer.c",
        "runtime/actors/aether_send_message.c",
        "runtime/actors/aether_actor_thread.c",
        "runtime/actors/aether_panic.c",
        "std/string/aether_string.c",
        "std/math/aether_math.c",
        "std/net/aether_http.c",
        "std/net/aether_http_server.c",
        "std/net/aether_http_pool.c",
        "std/net/aether_net.c",
        "std/net/aether_actor_bridge.c",
        "std/collections/aether_collections.c",
        "std/json/aether_json.c",
        "std/fs/aether_fs.c",
        "std/log/aether_log.c",
        "std/io/aether_io.c",
        "std/os/aether_os.c",
        "std/collections/aether_set.c",
        "std/collections/aether_pqueue.c",
        "std/collections/aether_intarr.c",
        "std/collections/aether_floatarr.c",
        "std/collections/aether_longarr.c",
        "std/collections/aether_bits.c",
        "std/collections/aether_stringlist.c",
        "std/collections/aether_stringseq.c",
        NULL
    };
    char runtime[8192];
    runtime[0] = '\0';
    for (int i = 0; wasm_runtime_files[i]; i++) {
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s ", tc.root, wasm_runtime_files[i]);
        strncat(runtime, path, sizeof(runtime) - strlen(runtime) - 1);
    }

    snprintf(cmd, size,
        "emcc -O2 -DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING "
        "%s \"%s\" %s -o \"%s\" -lm "
        "-Wall -Wextra -Wno-unused-parameter -Wno-unused-function "
        "-Wno-unused-variable -Wno-missing-field-initializers -Wno-unused-label",
        includes, c_file, runtime, out_file);

    return 1;
}

// --------------------------------------------------------------------------
// Binary-import prepass: consume a precompiled `--emit=lib` artifact as
// an Aether `import`. When a program does `import foo` and there is no
// `foo` source module but a `libfoo.so` / `foo.so` is on the search
// path, we read that artifact's `aether_lib_meta()` catalog (the v2
// schema, including closure-context records) and synthesize a small
// Aether interface stub — `@extern(...)` declarations for the function
// exports and trailing-block `builder` wrappers for the builder DSL
// entry points. The stub is dropped into a temp dir that is prepended
// to the module search path, so the existing source-import machinery
// (typecheck, namespace prefixing, builder registration) rehydrates the
// library with full call-site fidelity — `foo.greet(x)` and
// `foo.route(p) { ... }` read exactly as if compiled in the same cycle.
// The artifact itself is added to the link line. POSIX-only (gated on
// dlopen); a no-op on Windows, where DLL hosting is a follow-up.
// --------------------------------------------------------------------------

#ifndef _WIN32
// Split a rendered signature "(A, B) -> R" into an Aether parameter list
// ("p0: A, p1: B"), a bare argument list ("p0, p1"), and the return type
// ("R", or "void"). Top-level comma split tracking paren depth; the ABI
// and builder signatures we see here are flat (no nested closure types).
static void ae_split_signature(const char* sig,
                               char* params, size_t pcap,
                               char* args, size_t acap,
                               char* ret, size_t rcap) {
    params[0] = '\0'; args[0] = '\0';
    snprintf(ret, rcap, "void");
    if (!sig) return;
    const char* lp = strchr(sig, '(');
    const char* arrow = strstr(sig, "->");
    if (arrow) {
        const char* r = arrow + 2;
        while (*r == ' ') r++;
        snprintf(ret, rcap, "%s", r);
        size_t rl = strlen(ret);
        while (rl > 0 && (ret[rl-1] == ' ' || ret[rl-1] == '\n')) ret[--rl] = '\0';
    }
    if (!lp) return;
    const char* rp = NULL;
    int depth = 0;
    for (const char* p = lp; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (--depth == 0) { rp = p; break; } }
    }
    if (!rp || rp <= lp + 1) return;  // "()" — no params
    char inside[1024];
    size_t n = (size_t)(rp - (lp + 1));
    if (n >= sizeof(inside)) n = sizeof(inside) - 1;
    memcpy(inside, lp + 1, n);
    inside[n] = '\0';

    size_t poff = 0, aoff = 0;
    int idx = 0, d = 0;
    const char* start = inside;
    for (char* p = inside; ; p++) {
        if (*p == '(') d++;
        else if (*p == ')') d--;
        if ((*p == ',' && d == 0) || *p == '\0') {
            size_t tn = (size_t)(p - start);
            while (tn > 0 && *start == ' ') { start++; tn--; }
            char ty[128];
            if (tn >= sizeof(ty)) tn = sizeof(ty) - 1;
            memcpy(ty, start, tn);
            ty[tn] = '\0';
            while (tn > 0 && ty[tn-1] == ' ') ty[--tn] = '\0';
            if (ty[0]) {
                poff += snprintf(params + poff, poff < pcap ? pcap - poff : 0,
                                 "%sp%d: %s", idx ? ", " : "", idx, ty);
                aoff += snprintf(args + aoff, aoff < acap ? acap - aoff : 0,
                                 "%sp%d", idx ? ", " : "", idx);
                idx++;
            }
            if (*p == '\0') break;
            start = p + 1;
        }
    }
}

typedef const _AeLibInfoMeta* (*ae_meta_fn_t)(void);

// dlopen `so_path`, read its aether_lib_meta catalog, and write an Aether
// interface stub to `out`. Returns 0 on success, -1 if the artifact has
// no readable metadata.
static int ae_generate_binimport_stub(const char* so_path, FILE* out) {
    void* h = dlopen(so_path, RTLD_LAZY | RTLD_LOCAL);
    if (!h) return -1;
    ae_meta_fn_t mf = (ae_meta_fn_t)dlsym(h, "aether_lib_meta");
    if (!mf) { dlclose(h); return -1; }
    const _AeLibInfoMeta* m = mf();
    if (!m) { dlclose(h); return -1; }

    fprintf(out, "// Auto-generated Aether interface for %s\n", so_path);
    fprintf(out, "// Synthesized by `ae` from the artifact's aether_lib_meta\n");
    fprintf(out, "// catalog (schema %s). Do not edit; regenerated each build.\n",
            m->schema_version ? m->schema_version : "?");
    fprintf(out, "import std.map\n\n");

    char params[1100], args[600], ret[160];

    // Function-table exports → `@extern("<c_symbol>") <name>(params) -> ret`.
    for (int i = 0; i < m->function_count && m->functions; i++) {
        const _AeLibInfoFn* f = &m->functions[i];
        if (!f->aether_name || !f->c_symbol) continue;
        ae_split_signature(f->signature, params, sizeof(params),
                            args, sizeof(args), ret, sizeof(ret));
        if (strcmp(ret, "void") == 0) {
            fprintf(out, "@extern(\"%s\") %s(%s)\n", f->c_symbol, f->aether_name, params);
        } else {
            fprintf(out, "@extern(\"%s\") %s(%s) -> %s\n",
                    f->c_symbol, f->aether_name, params, ret);
        }
    }

    // Constant-table exports → a plain `const NAME = <value>` line per
    // catalog constant (schema >= 1.2). The catalog `value` is already a
    // source-ready Aether literal (quoted+escaped for strings, verbatim for
    // numbers/bools), so it drops in after `const NAME = `. The existing
    // source-import machinery then namespaces it as `<module>.NAME` for free —
    // no call-site changes for consumers. Guarded on the typed pointer so a
    // "1.0"/"1.1" artifact (no constant slot) emits nothing here.
    for (int i = 0; i < m->constant_count && m->constants; i++) {
        const _AeLibInfoConst* k = &m->constants[i];
        if (!k->name || !k->value) continue;
        fprintf(out, "const %s = %s\n", k->name, k->value);
    }

    // Builder DSL entry points → an extern forwarder taking the trailing
    // `_builder` config map plus a `builder` wrapper that the consumer's
    // call site drives with a trailing block. The library function's bare
    // symbol has the shape `<name>(<params>, void* _builder)` (the builder
    // config map is passed as the final argument).
    for (int i = 0; i < m->closure_count && m->closures; i++) {
        const _AeLibInfoClosure* c = &m->closures[i];
        if (!c->role || strcmp(c->role, "builder") != 0 || !c->name || !c->name[0]) continue;
        ae_split_signature(c->signature, params, sizeof(params),
                            args, sizeof(args), ret, sizeof(ret));
        int is_void = (strcmp(ret, "void") == 0);
        const char* comma = params[0] ? ", " : "";
        const char* acomma = args[0] ? ", " : "";
        fprintf(out, "\n@extern(\"%s\") __aeb_%s(%s%s_builder: ptr)%s%s\n",
                c->name, c->name, params, comma,
                is_void ? "" : " -> ", is_void ? "" : ret);
        fprintf(out, "builder %s(%s) {\n", c->name, params);
        fprintf(out, "    %s__aeb_%s(%s%s_builder)\n",
                is_void ? "" : "return ", c->name, args, acomma);
        fprintf(out, "}\n");
    }

    dlclose(h);  // m points into the .so; everything was emitted above.
    return 0;
}

// True if a *source* module named `mod` resolves on the current search
// path (CWD, src/, and each --lib dir). Mirrors the compiler's local
// resolver closely enough to decide "source vs binary" for a bare import.
static int ae_source_module_exists(const char* mod) {
    char p[1200];
    const char* bases[] = { ".", "src" };
    for (size_t b = 0; b < sizeof(bases)/sizeof(bases[0]); b++) {
        snprintf(p, sizeof(p), "%s/%s.ae", bases[b], mod);          if (path_exists(p)) return 1;
        snprintf(p, sizeof(p), "%s/%s/module.ae", bases[b], mod);   if (path_exists(p)) return 1;
    }
    for (int i = 0; i < tc.lib_dir_count; i++) {
        snprintf(p, sizeof(p), "%s/%s.ae", tc.lib_dirs[i], mod);        if (path_exists(p)) return 1;
        snprintf(p, sizeof(p), "%s/%s/module.ae", tc.lib_dirs[i], mod); if (path_exists(p)) return 1;
    }
    return 0;
}

// Locate a binary artifact for module `mod` (libMOD.so / MOD.so /
// libMOD.dylib / MOD.dylib) on the search path. Both extensions are
// tried on every POSIX platform: a shared object is identified by its
// contents, not its suffix, and `ae build --emit=lib -o libfoo.so`
// produces a `.so`-named artifact even on macOS — dlopen and the linker
// accept it regardless. macOS-native `.dylib` is tried first there.
// Writes the resolved path into `out` and returns 1 if found.
static int ae_find_binimport_so(const char* mod, char* out, size_t outcap) {
    const char* exts[] = {
#ifdef __APPLE__
        ".dylib", ".so"
#else
        ".so", ".dylib"
#endif
    };
    const char* dirs[8 + 2];
    int nd = 0;
    dirs[nd++] = ".";
    for (int i = 0; i < tc.lib_dir_count && nd < (int)(sizeof(dirs)/sizeof(dirs[0])); i++) {
        dirs[nd++] = tc.lib_dirs[i];
    }
    for (int d = 0; d < nd; d++) {
        for (size_t e = 0; e < sizeof(exts)/sizeof(exts[0]); e++) {
            snprintf(out, outcap, "%s/lib%s%s", dirs[d], mod, exts[e]);
            if (path_exists(out)) return 1;
            snprintf(out, outcap, "%s/%s%s", dirs[d], mod, exts[e]);
            if (path_exists(out)) return 1;
        }
    }
    return 0;
}

// Resolve `path` to an absolute path (best-effort) for use in -rpath and
// on the link line, so the produced binary finds the .so at run time
// regardless of the cwd it is launched from.
static void ae_abspath(const char* path, char* out, size_t outcap) {
    char* rp = realpath(path, NULL);
    if (rp) { snprintf(out, outcap, "%s", rp); free(rp); }
    else    { snprintf(out, outcap, "%s", path); }
}

// Scan `main_file` for `import <bare>` statements that resolve to a
// binary artifact rather than source, generate an interface stub for
// each into a shared temp dir (prepended to the module search path), and
// record the artifact on the link line. Best-effort: any failure leaves
// the build to proceed (and fail later) as an all-source build would.
static void prepare_binary_imports(const char* main_file) {
    FILE* f = fopen(main_file, "r");
    if (!f) return;

    char stubdir[256] = "";
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "import", 6) != 0 || (p[6] != ' ' && p[6] != '\t')) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        // Module token: identifier chars only. A '.' means std./contrib./
        // dotted path — never a bare binary import, skip.
        char mod[128];
        size_t mi = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && mi < sizeof(mod) - 1) {
            mod[mi++] = *p++;
        }
        mod[mi] = '\0';
        if (mi == 0 || *p == '.') continue;
        if (ae_source_module_exists(mod)) continue;

        char so_path[1200];
        if (!ae_find_binimport_so(mod, so_path, sizeof(so_path))) continue;

        if (!stubdir[0]) {
            snprintf(stubdir, sizeof(stubdir), "/tmp/ae-binimport-XXXXXX");
            if (!mkdtemp(stubdir)) { stubdir[0] = '\0'; break; }
        }
        char stub_path[512];
        snprintf(stub_path, sizeof(stub_path), "%s/%s.ae", stubdir, mod);
        FILE* sf = fopen(stub_path, "w");
        if (!sf) continue;
        int rc = ae_generate_binimport_stub(so_path, sf);
        fclose(sf);
        if (rc != 0) { remove(stub_path); continue; }

        // Make the stub resolvable and link the artifact (absolute path +
        // rpath so the produced binary finds it at run time). The host's
        // -rdynamic + static libaether satisfy the .so's runtime symbols.
        tc_lib_dir_append_one(stubdir);
        char abs_so[1200], dir[1200];
        ae_abspath(so_path, abs_so, sizeof(abs_so));
        snprintf(dir, sizeof(dir), "%s", abs_so);
        char* slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        // Emit the rpath UNQUOTED — `-Wl,-rpath,<dir>`, parallel to the
        // unquoted `-L%s` this file already uses. Quoting it
        // (`-Wl,-rpath,"<dir>"`) leaked literal quote characters into the
        // recorded rpath on macOS (`dyld: tried '"/.../tmp"/lib.dylib'`),
        // so the dylib — whose install name `ae build --emit=lib` rewrites
        // to `@rpath/<base>` on macOS — was never found at run time.
        // Module/lib/temp dirs don't contain spaces, same assumption -L
        // relies on. The .so itself stays quoted (it's a plain input file
        // and links fine on both platforms).
        size_t off = strlen(g_binimport_link);
        snprintf(g_binimport_link + off, sizeof(g_binimport_link) - off,
                 " \"%s\" -Wl,-rpath,%s", abs_so, dir);
        if (tc.verbose) {
            fprintf(stderr, "ae: binary import '%s' -> %s (stub %s)\n",
                    mod, abs_so, stub_path);
        }
    }
    fclose(f);
}
#else
static void prepare_binary_imports(const char* main_file) { (void)main_file; }
#endif

// Scan `main_file` for `import contrib.host.<lang>` statements and
// queue the matching bridge static archive onto the link line.
//
// The bridge .c (contrib/host/<lang>/aether_host_<lang>.c) compiles
// to libaether_host_<lang>.a (see Makefile:1324). Linking the .a is
// what supplies symbols like `python_run` — the BRIDGE's own ABI,
// distinct from the host language's runtime symbols (Py_Initialize
// et al.) that the bridge in turn calls. Without this scan, an
// import like `import contrib.host.python` compiled because the
// headers resolved, but at runtime failed with `undefined symbol:
// python_run` because the .a was never linked. Users had to repeat
// the import as `link_flags = "-laether_host_python"` in
// aether.toml — busywork easily forgotten (the ctr_notes.md Bug 4
// trace from 2026-06-03).
//
// Entry-file-only: only the top-level .ae passed to `ae build` /
// `ae run` is scanned. If a library you import in turn imports
// `contrib.host.python`, you must still write the import yourself
// in your top-level file. Widening to transitive imports later is
// purely additive — same predicate, more files to scan.
//
// Hard error if the .a is missing (the user opted in via the
// import; silently dropping it just defers the failure to a more
// confusing runtime error). The two search paths mirror the two
// install layouts in tools/ae.c:1013-1017:
//   - install layout: <lib_dir>/libaether_host_<lang>.a
//     (Makefile install-contrib target installs here)
//   - dev layout:     <lib_dir>/contrib/libaether_host_<lang>.a
//     (`make contrib` builds here, without installing)
// where `lib_dir` is the directory containing libaether.a, same as
// build_gcc_cmd derives it from `tc.lib`.
//
// POSIX-only — host bridges aren't compiled on the Windows matrix.
#ifndef _WIN32
// Back-compat aliases: some bridge directories were renamed for
// clarity, but old import paths must keep resolving. `js` was
// renamed to `duktape` (engine name; allows `--with=quickjs` etc.
// to coexist later); `import contrib.host.js` still works by
// linking the duktape bridge .a transparently here.
static const char* host_bridge_lang_alias(const char* lang) {
    if (strcmp(lang, "js") == 0) return "duktape";
    // Rhombus is a #lang on the Racket runtime, so both import paths link
    // the one shared bridge archive (libaether_host_racket.a — it carries
    // both the racket_* and rhombus_* ABI). No separate rhombus .a is built.
    if (strcmp(lang, "rhombus") == 0) return "racket";
    return lang;
}

static bool host_bridge_a_path(const char* lang, char* out, size_t outsz) {
    if (!tc.has_lib) return false;
    char lib_dir[1024];
    strncpy(lib_dir, tc.lib, sizeof(lib_dir) - 1);
    lib_dir[sizeof(lib_dir) - 1] = '\0';
    char* slash = strrchr(lib_dir, '/');
    if (slash) *slash = '\0';

    const char* effective = host_bridge_lang_alias(lang);

    // Install layout: <lib_dir>/libaether_host_<lang>.a
    snprintf(out, outsz, "%s/libaether_host_%s.a", lib_dir, effective);
    if (path_exists(out)) return true;
    // Dev layout: <lib_dir>/contrib/libaether_host_<lang>.a (build/contrib/)
    snprintf(out, outsz, "%s/contrib/libaether_host_%s.a", lib_dir, effective);
    if (path_exists(out)) return true;
    return false;
}

static void prepare_host_bridge_imports(const char* main_file) {
    FILE* f = fopen(main_file, "r");
    if (!f) return;

    // Track which languages we've already queued so the same import
    // appearing twice doesn't double-link the .a (gcc would tolerate
    // it but the noise hides real problems).
    char seen[256] = "";

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "import", 6) != 0 || (p[6] != ' ' && p[6] != '\t')) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "contrib.host.", 13) != 0) continue;
        p += 13;
        // Extract <lang> — identifier chars only.
        char lang[64];
        size_t li = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && li < sizeof(lang) - 1) {
            lang[li++] = *p++;
        }
        lang[li] = '\0';
        if (li == 0) continue;

        // Dedup against `seen` (space-delimited, surrounded by spaces
        // so "py" doesn't match "python").
        char needle[80];
        snprintf(needle, sizeof(needle), " %s ", lang);
        if (strstr(seen, needle)) continue;
        size_t soff = strlen(seen);
        snprintf(seen + soff, sizeof(seen) - soff, "%s%s ",
                 soff == 0 ? " " : "", lang);

        char a_path[1200];
        if (!host_bridge_a_path(lang, a_path, sizeof(a_path))) {
            fclose(f);
            fprintf(stderr,
                "ae: cannot find libaether_host_%s.a — required by "
                "`import contrib.host.%s` in %s.\n"
                "  Build the contrib bridges with `make contrib` (dev tree)\n"
                "  or `make install-contrib` (installed layout); the .a is\n"
                "  expected next to libaether.a or in a sibling contrib/ dir.\n",
                lang, lang, main_file);
            exit(1);
        }

        // Append the .a as a direct file path (more deterministic than
        // -L+-l in the build-tree case where multiple lib dirs might
        // shadow each other). gcc treats a bare .a on the command line
        // as an input archive, the way -laether_host_<lang> would.
        size_t off = strlen(g_host_bridge_link);
        snprintf(g_host_bridge_link + off,
                 sizeof(g_host_bridge_link) - off,
                 " \"%s\"", a_path);

        // Per-bridge transitive link deps. The six interpreter bridges
        // dlopen their host language at runtime and need nothing extra
        // at link time. tinygo is the first bridge with its OWN
        // link-time dep — `tinygo_call_dynamic` calls into libffi when
        // the bridge .a was built with AETHER_HAS_LIBFFI. Without this
        // append, the link fails with `undefined reference to
        // ffi_prep_cif` whenever the bridge was compiled with libffi
        // available (ctr_notes.md Finding 2). Users would otherwise
        // have to add `link_flags = "-lffi"` to their aether.toml
        // manually — exactly the kind of "import is the trigger"
        // footgun that contrib/host/python/README.md §8 promises
        // bridges should avoid.
        //
        // The probe is by-symbol, not by-language: when libffi-dev
        // wasn't present at bridge-build time the AETHER_HAS_LIBFFI
        // block is #ifdef-out, the .a has no undefined ffi_* symbols,
        // and we MUST NOT pass `-lffi` (the host's link would fail
        // with "cannot find -lffi"). `nm -u` lists only undefined
        // symbols; one popen per build, no measurable cost. Skip on
        // platforms without nm — the link error is then the same
        // diagnostic users had before this fix and the manual
        // aether.toml workaround still applies.
        const char* effective_alias = host_bridge_lang_alias(lang);
        const char* trans_flags = NULL;
        if (strcmp(effective_alias, "tinygo") == 0) {
            char nm_cmd[1300];
            snprintf(nm_cmd, sizeof(nm_cmd),
                     "nm -u \"%s\" 2>/dev/null | grep -q ffi_prep_cif",
                     a_path);
            if (system(nm_cmd) == 0) {
                trans_flags = " -lffi";
            }
        }
        if (trans_flags) {
            off = strlen(g_host_bridge_link);
            snprintf(g_host_bridge_link + off,
                     sizeof(g_host_bridge_link) - off, "%s", trans_flags);
        }

        // Racket (and rhombus, which aliases to it) is the first STATIC-linked
        // host: there is no shared libracketcs to dlopen, so the importer must
        // link the built Racket CS's libracketcs.a + -rdynamic + the runtime's
        // system deps. The archive path comes from $AETHER_RACKET_LIB (the
        // orchestrator owns the probe, mirroring AETHER_*_SONAME for the dlopen
        // bridges). Without it we leave the link as-is — the bridge .a's
        // unresolved racket_* symbols then produce a clear linker error naming
        // the missing libracketcs, and the README documents the env var.
        if (strcmp(effective_alias, "racket") == 0) {
            const char* rkt_lib = getenv("AETHER_RACKET_LIB");
            if (rkt_lib && *rkt_lib) {
                // libracketcs.a + -rdynamic + Racket CS's link deps. The lib
                // list matches the RKTIO_CONFIGURE_ARGS LIBS of a stock CS
                // build (-ldl -lm -lrt -lncurses -lz) plus -lpthread; harmless
                // extras are dropped by the linker if unreferenced.
                off = strlen(g_host_bridge_link);
                snprintf(g_host_bridge_link + off,
                         sizeof(g_host_bridge_link) - off,
                         " \"%s\" -rdynamic -lm -ldl -lpthread -lz -lncurses",
                         rkt_lib);
            }
        }

        if (tc.verbose) {
            fprintf(stderr, "ae: host bridge 'contrib.host.%s' -> %s%s\n",
                    lang, a_path, trans_flags ? trans_flags : "");
        }
    }
    fclose(f);
}
#else
static void prepare_host_bridge_imports(const char* main_file) { (void)main_file; }
#endif

// --------------------------------------------------------------------------
// Commands
// --------------------------------------------------------------------------

static int cmd_run(int argc, char** argv) {
    const char* file = NULL;
    /* 8 KiB matches toml_extra below + the fgets line buffer in
     * get_extra_sources_for_bin. Needs to fit --extra CLI args plus
     * the full TOML extra_sources concatenated. */
    char extra_files[8192] = "";

    /* Index in argv where the program's own arguments begin — everything
     * after a literal `--`. These are forwarded verbatim to the running
     * program (like `cargo run -- args`), so a config-is-code entry point
     * can do `ae run supervisor.ae -- make -j8` and see make/-j8 in its
     * own argv. -1 = no `--` seen, nothing to forward. */
    int prog_args_start = -1;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            prog_args_start = i + 1;  /* rest are the program's args */
            break;                    /* stop flag parsing at the separator */
        } else if (strcmp(argv[i], "--extra") == 0 && i + 1 < argc) {
            if (extra_files[0]) strncat(extra_files, " ", sizeof(extra_files) - strlen(extra_files) - 1);
            strncat(extra_files, argv[++i], sizeof(extra_files) - strlen(extra_files) - 1);
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            /* Issue #413: each `--lib X` appends to the lib search
             * path with the platform separator. A single value may
             * itself be a separator-string (`a:b` POSIX / `a;b`
             * Win); the aetherc side splits before resolving.
             * Repeated flags and separator strings compose. */
            tc_lib_dir_append(argv[++i]);
        } else if (argv[i][0] != '-' && !file) {
            file = argv[i];
        }
    }

    // Resolve directory argument (e.g. "." or "myproject/") to src/main.ae
    if (file && dir_exists(file)) {
        static char resolved_run_file[512];
        snprintf(resolved_run_file, sizeof(resolved_run_file), "%s/src/main.ae", file);
        if (path_exists(resolved_run_file)) {
            file = resolved_run_file;
        } else {
            char toml_path[512];
            snprintf(toml_path, sizeof(toml_path), "%s/aether.toml", file);
            if (path_exists(toml_path))
                fprintf(stderr, "Error: No src/main.ae found in %s\n", file);
            else
                fprintf(stderr, "Error: '%s' is not an Aether project directory\n", file);
            return 1;
        }
    }

    // Project mode: no file argument, look for aether.toml
    if (!file && path_exists("aether.toml")) {
        if (path_exists("src/main.ae"))
            file = "src/main.ae";
        else {
            fprintf(stderr, "Error: aether.toml found but src/main.ae is missing.\n");
            fprintf(stderr, "Create src/main.ae or specify a file: ae run <file.ae>\n");
            return 1;
        }
    }

    if (!file) {
        fprintf(stderr, "Error: No input file specified.\n");
        fprintf(stderr, "Usage: ae run <file.ae>\n");
        fprintf(stderr, "   or: Create a project with 'ae init <name>'\n");
        return 1;
    }

    if (!path_exists(file)) {
        fprintf(stderr, "Error: File not found: %s\n", file);
        return 1;
    }

    char c_file[2048], exe_file[2048], cmd[16384];

    // Merge toml [[bin]] extra_sources into extra_files BEFORE the cache
    // check. Otherwise editing an FFI shim listed in aether.toml wouldn't
    // invalidate the cached exe (extras content is part of the cache key).
    {
        char toml_extra_pre[8192] = "";
        if (get_extra_sources_for_bin(file, toml_extra_pre, sizeof(toml_extra_pre))) {
            fprintf(stderr,
                "Warning: aether.toml [[bin]] extra_sources for '%s' "
                "exceeded 8 KiB; tail entries were dropped. Split the "
                "array into fewer, larger shims or report as a toolchain "
                "bug.\n", file);
        }
        if (toml_extra_pre[0]) {
            if (extra_files[0]) strncat(extra_files, " ", sizeof(extra_files) - strlen(extra_files) - 1);
            strncat(extra_files, toml_extra_pre, sizeof(extra_files) - strlen(extra_files) - 1);
        }
    }

    // --- Cache check ---
    // ae run uses -O0 (fast dev builds). Check if we have a cached exe for
    // this exact source + compiler + extras combination.
    bool using_cache = false;
    char cached_exe[1024] = "";
    unsigned long long cache_key = compute_cache_key(file, extra_files, "O0", "run");
    if (cache_key != 0) {
        init_cache_dir();
        snprintf(cached_exe, sizeof(cached_exe), "%s/%016llx" EXE_EXT, s_cache_dir, cache_key);
        if (path_exists(cached_exe)) {
            if (tc.verbose) fprintf(stderr, "[cache] hit: %016llx\n", cache_key);
            snprintf(cmd, sizeof(cmd), "%s", cached_exe);
            int rc = run_cmd(cmd);
            if (rc < 0) {
                fprintf(stderr, "Program crashed (signal %d", -rc);
                if (-rc == 11) fprintf(stderr, ": segmentation fault");
                else if (-rc == 6) fprintf(stderr, ": aborted");
                fprintf(stderr, ")\n");
            }
            return rc;
        }
        if (tc.verbose) fprintf(stderr, "[cache] miss: %016llx\n", cache_key);
        using_cache = true;
    }

    // Determine temp .c file path and exe path
    // If caching: write exe directly to cache slot (no extra copy needed)
    // Use PID in temp filenames to avoid symlink attacks and collisions
    int pid = (int)getpid();
    if (tc.dev_mode) {
        snprintf(c_file, sizeof(c_file), "%s/build/_ae_%d.c", tc.root, pid);
    } else {
        snprintf(c_file, sizeof(c_file), "%s/_ae_%d.c", get_temp_dir(), pid);
    }
    if (using_cache) {
        // Link into a private temp beside the slot, publish by rename
        // after a successful build (#1032) — never let ld write the
        // final slot in place, or a concurrent hit execs a partial exe.
        snprintf(exe_file, sizeof(exe_file), "%s.tmp.%d", cached_exe, pid);
    } else if (tc.dev_mode) {
        snprintf(exe_file, sizeof(exe_file), "%s/build/_ae_%d" EXE_EXT, tc.root, pid);
    } else {
        snprintf(exe_file, sizeof(exe_file), "%s/_ae_%d" EXE_EXT, get_temp_dir(), pid);
    }

    // Binary-import prepass: synthesize interface stubs for any
    // `import foo` that resolves to a precompiled libfoo.so, and record
    // it on the link line. No-op for all-source programs.
    prepare_binary_imports(file);

    // Host-bridge prepass: `import contrib.host.<lang>` queues
    // libaether_host_<lang>.a onto the link line. Import-driven so a
    // pure-Aether program doesn't gain libpython et al. as runtime
    // dependencies it doesn't use.
    prepare_host_bridge_imports(file);

    // Step 1: Compile .ae to .c
    if (tc.verbose) printf("Compiling %s...\n", file);
    build_aetherc_cmd(cmd, sizeof(cmd), file, c_file);

    int aetherc_ret = tc.verbose ? run_cmd(cmd) : run_cmd_quiet(cmd);
    if (aetherc_ret != 0) {
        // Re-run with output visible so user can see the error
        build_aetherc_cmd(cmd, sizeof(cmd), file, c_file);
        run_cmd(cmd);
        fprintf(stderr, "Compilation failed.\n");
        return 1;
    }

    // Step 2: Compile .c to executable with runtime (-O0 for fast dev builds).
    // toml [[bin]] extra_sources were already merged into extra_files above
    // (before the cache check), so no further reading is needed here.
    const char* run_extra = extra_files[0] ? extra_files : NULL;
    build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, false, run_extra);
    // Show stderr (gcc warnings like -Wformat) even in non-verbose mode
    int gcc_ret = tc.verbose ? run_cmd(cmd) : run_cmd_show_warnings(cmd);
    if (gcc_ret != 0) {
        // Re-run with output for error diagnosis
        build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, false, run_extra);
        run_cmd(cmd);
        fprintf(stderr, "Build failed.\n");
        remove(c_file);
        remove(exe_file);  // partial link output, if any
        remove_dsym_bundle(exe_file);
        return 1;
    }

    // Clean up temp .c file (exe stays in cache if caching, else clean up too)
    remove(c_file);
    // macOS: drop the dsymutil bundle the -g link left beside the temp
    // exe — cache slots don't carry debug bundles, and the rename below
    // moves only the exe (#1032).
    remove_dsym_bundle(exe_file);

    // Publish the freshly-linked exe into its cache slot (#1032). The
    // rename is atomic, so concurrent invocations see the old complete
    // file, the new complete file, or a miss — never a partial slot.
    if (using_cache) {
        if (cache_publish(exe_file, cached_exe) == 0) {
            strncpy(exe_file, cached_exe, sizeof(exe_file) - 1);
            exe_file[sizeof(exe_file) - 1] = '\0';
        } else {
            // Exotic-filesystem rename failure: run the private temp
            // exe and clean it up like an uncached build.
            if (tc.verbose) fprintf(stderr, "[cache] publish failed for %016llx\n", cache_key);
            using_cache = false;
        }
    }

    // Step 3: Run, forwarding any post-`--` args to the program. Each is
    // wrapped in double quotes so a single arg with spaces stays one
    // token through run_cmd's tokenizer (posix_run / win_run). Args
    // containing a literal double-quote aren't representable through this
    // path — rare for a build command line; build the binary and invoke
    // it directly if you need that.
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe_file);
    if (prog_args_start >= 0) {
        size_t off = strlen(cmd);
        for (int i = prog_args_start; i < argc && off < sizeof(cmd) - 1; i++) {
            int w = snprintf(cmd + off, sizeof(cmd) - off, " \"%s\"", argv[i]);
            if (w < 0 || (size_t)w >= sizeof(cmd) - off) break;  /* truncated — stop cleanly */
            off += (size_t)w;
        }
    }
    int rc = run_cmd(cmd);

    if (rc < 0) {
        fprintf(stderr, "Program crashed (signal %d", -rc);
        if (-rc == 11) fprintf(stderr, ": segmentation fault");
        else if (-rc == 6) fprintf(stderr, ": aborted");
        fprintf(stderr, ")\n");
        // Remove crashed binary from cache so next run recompiles
        if (using_cache) remove(exe_file);
    }

    // If not cached, remove the temp exe
    if (!using_cache) remove(exe_file);

    return rc;
}

static int cmd_check(int argc, char** argv) {
    const char* file = NULL;

    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-') {
            file = argv[i];
        }
    }

    // Project mode
    if (!file && path_exists("aether.toml")) {
        if (path_exists("src/main.ae"))
            file = "src/main.ae";
        else {
            fprintf(stderr, "Error: aether.toml found but src/main.ae is missing.\n");
            return 1;
        }
    }

    if (!file) {
        fprintf(stderr, "Usage: ae check <file.ae>\n");
        return 1;
    }

    if (!path_exists(file)) {
        fprintf(stderr, "Error: File not found: %s\n", file);
        return 1;
    }

    /* Build the same `--lib X --lib Y …` flag sequence the compile
     * path uses (cc_command_build); one flag per entry sidesteps
     * shell quoting on cmd.exe + MSYS2. Issue #413. */
    char lib_flags[2304] = "";
    size_t lf_off = 0;
    for (int i = 0; i < tc.lib_dir_count; i++) {
        int w = snprintf(lib_flags + lf_off, sizeof(lib_flags) - lf_off,
                         " --lib \"%s\"", tc.lib_dirs[i]);
        if (w < 0 || (size_t)w >= sizeof(lib_flags) - lf_off) break;
        lf_off += (size_t)w;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "\"%s\"%s --check \"%s\"",
             tc.compiler, lib_flags, file);
    return run_cmd(cmd);
}

// `ae inspect <file.ae>` — operator-facing summary of what a script
// declares (imports, capability posture, exports/entry, declarations).
// Delegates to `aetherc --emit=inspect`, which walks the post-typecheck
// AST and prints to stdout; no .c is written. Issue #473.
static int cmd_inspect(int argc, char** argv) {
    const char* file = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-') file = argv[i];
    }

    // Project mode: default to src/main.ae when run inside a project.
    if (!file && path_exists("aether.toml")) {
        if (path_exists("src/main.ae")) {
            file = "src/main.ae";
        } else {
            fprintf(stderr, "Error: aether.toml found but src/main.ae is missing.\n");
            return 1;
        }
    }
    if (!file) {
        fprintf(stderr, "Usage: ae inspect <file.ae>\n");
        return 1;
    }
    if (!path_exists(file)) {
        fprintf(stderr, "Error: File not found: %s\n", file);
        return 1;
    }

    /* One `--lib X` per entry, same as cmd_check — keeps import
     * resolution consistent so the reported imports resolve the way a
     * build would. Issue #413. */
    char lib_flags[2304] = "";
    size_t lf_off = 0;
    for (int i = 0; i < tc.lib_dir_count; i++) {
        int w = snprintf(lib_flags + lf_off, sizeof(lib_flags) - lf_off,
                         " --lib \"%s\"", tc.lib_dirs[i]);
        if (w < 0 || (size_t)w >= sizeof(lib_flags) - lf_off) break;
        lf_off += (size_t)w;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "\"%s\" --emit=inspect%s \"%s\"",
             tc.compiler, lib_flags, file);
    return run_cmd(cmd);
}

// Forward declaration — cmd_build_namespace delegates to cmd_build for the
// actual link step, but cmd_build is defined further down.
static int cmd_build(int argc, char** argv);

// =============================================================================
// Per-language SDK generation for `ae build --namespace`
//
// After the namespace .so is built, this layer reads the manifest JSON
// and the function list (both via aetherc) and emits one host-language
// SDK per binding target the manifest declared. v1: Python only; Java
// follows in a separate chunk.
//
// The generated SDKs all use the same shape so the user experience is
// consistent across languages:
//   - construct an instance pointing at the .so
//   - set_<input>(value) per input
//   - on_<event>(callback) per event
//   - <function>(args...) per script function
//   - describe() returns the manifest
// =============================================================================

/* Captured manifest fields used during SDK generation. Mirrors the JSON
 * shape; only the fields the generators need. */
typedef struct {
    char ns_name[128];
    char py_module[128];
    char rb_module[128];
    char java_pkg[256];
    char java_class[128];
    int  input_count;
    struct { char name[128]; char type[128]; } inputs[64];
    int  event_count;
    struct { char name[128]; char carries[64]; } events[64];
} CapturedManifest;

typedef struct {
    char name[128];
    char ret[64];
    int  param_count;
    struct { char name[128]; char type[64]; } params[16];
} CapturedFunction;

/* Run aetherc with the given args and capture stdout into out_buf. */
int aetherc_capture_stdout(const char* arg1, const char* in_path,
                                  const char* arg2_or_null,
                                  char* out_buf, size_t out_size) {
    char cmd[4096];
    if (arg2_or_null) {
        snprintf(cmd, sizeof(cmd), "\"%s\" %s \"%s\" \"%s\"",
                 tc.compiler, arg1, in_path, arg2_or_null);
    } else {
        snprintf(cmd, sizeof(cmd), "\"%s\" %s \"%s\" /dev/null",
                 tc.compiler, arg1, in_path);
    }
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    size_t total = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) {
        size_t n = strlen(buf);
        if (total + n + 1 >= out_size) break;
        memcpy(out_buf + total, buf, n);
        total += n;
    }
    out_buf[total] = '\0';
    return pclose(p);
}

/* Tiny ad-hoc JSON-ish field extractor. The aetherc JSON format is
 * stable and one-line-per-array-element, so simple substring + scanf
 * is sufficient — we don't pull in a real JSON parser. Returns 1 if
 * the field was found, 0 otherwise. Output is the unescaped string
 * content (no quotes); writes empty string on missing. */
static int json_extract_string_field(const char* json, const char* key,
                                     char* out, size_t out_size) {
    out[0] = '\0';
    /* Look for `"key":` */
    char needle[160];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) return 1; /* present, value null */
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            if (c == 'n') out[i++] = '\n';
            else if (c == 't') out[i++] = '\t';
            else out[i++] = c;
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return 1;
}

/* Parse the manifest JSON produced by aetherc --emit-namespace-manifest.
 * Returns 0 on success, -1 on failure. */
static int parse_manifest_json(const char* json, CapturedManifest* m) {
    memset(m, 0, sizeof(*m));
    json_extract_string_field(json, "namespace", m->ns_name, sizeof(m->ns_name));

    /* Bindings live under "bindings": { "java": { "package":, "class": },
     * "python": { "module": }, "go": { "package": } }. Scope each
     * sub-object before extracting so we don't grab the wrong "package"
     * (java's vs go's). */
    const char* java_obj   = strstr(json, "\"java\":");
    const char* python_obj = strstr(json, "\"python\":");
    const char* ruby_obj   = strstr(json, "\"ruby\":");
    const char* go_obj     = strstr(json, "\"go\":");
    if (java_obj)   {
        json_extract_string_field(java_obj, "package", m->java_pkg,   sizeof(m->java_pkg));
        json_extract_string_field(java_obj, "class",   m->java_class, sizeof(m->java_class));
    }
    if (python_obj) {
        json_extract_string_field(python_obj, "module", m->py_module, sizeof(m->py_module));
    }
    if (ruby_obj) {
        json_extract_string_field(ruby_obj, "module", m->rb_module, sizeof(m->rb_module));
    }
    /* Go binding stored but unused for now — emitter is a stub. */
    (void)go_obj;

    /* Inputs and events: each occurrence of `"name":` inside an array
     * element marks a new entry. We scan linearly to keep declaration
     * order. The JSON is one entry per pair like
     *   {"name": "X", "type": "Y"} or {"name": "X", "carries": "Y"}.
     */
    const char* inputs_start = strstr(json, "\"inputs\":");
    const char* events_start = strstr(json, "\"events\":");
    const char* bindings_start = strstr(json, "\"bindings\":");
    if (!inputs_start || !events_start || !bindings_start) return -1;

    /* Walk inputs. */
    const char* p = inputs_start;
    while ((p = strstr(p, "{\"name\":")) && p < events_start) {
        if (m->input_count >= 64) break;
        json_extract_string_field(p, "name", m->inputs[m->input_count].name,
                                  sizeof(m->inputs[0].name));
        json_extract_string_field(p, "type", m->inputs[m->input_count].type,
                                  sizeof(m->inputs[0].type));
        m->input_count++;
        p++;
    }
    /* Walk events. */
    p = events_start;
    while ((p = strstr(p, "{\"name\":")) && p < bindings_start) {
        if (m->event_count >= 64) break;
        json_extract_string_field(p, "name", m->events[m->event_count].name,
                                  sizeof(m->events[0].name));
        json_extract_string_field(p, "carries", m->events[m->event_count].carries,
                                  sizeof(m->events[0].carries));
        m->event_count++;
        p++;
    }
    return 0;
}

/* Parse the function list `name|return|p1:t1,p2:t2,...` (one per line). */
static int parse_function_list(const char* text, CapturedFunction* fns,
                               int max_fns) {
    int count = 0;
    const char* p = text;
    while (*p && count < max_fns) {
        const char* eol = strchr(p, '\n');
        if (!eol) break;
        size_t line_len = eol - p;
        if (line_len == 0) { p = eol + 1; continue; }

        char line[1024];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        char* bar1 = strchr(line, '|');
        if (!bar1) { p = eol + 1; continue; }
        *bar1 = '\0';
        char* bar2 = strchr(bar1 + 1, '|');
        if (!bar2) { p = eol + 1; continue; }
        *bar2 = '\0';

        CapturedFunction* f = &fns[count];
        memset(f, 0, sizeof(*f));
        strncpy(f->name, line, sizeof(f->name) - 1);
        strncpy(f->ret, bar1 + 1, sizeof(f->ret) - 1);

        /* params: comma-separated name:type */
        char* param_start = bar2 + 1;
        while (*param_start && f->param_count < 16) {
            char* comma = strchr(param_start, ',');
            char piece[256];
            size_t plen = comma ? (size_t)(comma - param_start) : strlen(param_start);
            if (plen >= sizeof(piece)) plen = sizeof(piece) - 1;
            memcpy(piece, param_start, plen);
            piece[plen] = '\0';

            char* colon = strchr(piece, ':');
            if (colon) {
                *colon = '\0';
                strncpy(f->params[f->param_count].name, piece,
                        sizeof(f->params[0].name) - 1);
                strncpy(f->params[f->param_count].type, colon + 1,
                        sizeof(f->params[0].type) - 1);
                f->param_count++;
            }

            if (!comma) break;
            param_start = comma + 1;
        }

        count++;
        p = eol + 1;
    }
    return count;
}

/* Skip functions the user marked or the pipeline synthesized:
 *   - main() is the synthesized empty entry
 *   - setup() is the manifest-builder entry from manifest.ae (we don't
 *     want to expose it as part of the namespace SDK) */
static int is_skipped_function(const char* name) {
    return strcmp(name, "main") == 0 || strcmp(name, "setup") == 0;
}

/* Map an Aether type spelling to a Python ctypes type name. Returns
 * NULL if the type isn't representable in v1 — caller should skip the
 * function with a warning. */
static const char* py_ctype_for(const char* aether_type) {
    if (strcmp(aether_type, "int")    == 0) return "ctypes.c_int32";
    if (strcmp(aether_type, "long")   == 0) return "ctypes.c_int64";
    if (strcmp(aether_type, "ulong")  == 0) return "ctypes.c_uint64";
    if (strcmp(aether_type, "float")  == 0) return "ctypes.c_float";
    if (strcmp(aether_type, "bool")   == 0) return "ctypes.c_int32";
    if (strcmp(aether_type, "string") == 0) return "ctypes.c_char_p";
    if (strcmp(aether_type, "ptr")    == 0) return "ctypes.c_void_p";
    if (strcmp(aether_type, "void")   == 0) return "None";
    return NULL;
}

/* Map an Aether type to a Ruby Fiddle type constant. Returns NULL for
 * types not representable in v1 (caller should skip the function with
 * a warning, same convention as Python and Java). */
static const char* rb_fiddle_type_for(const char* aether_type) {
    if (strcmp(aether_type, "int")    == 0) return "Fiddle::TYPE_INT";
    if (strcmp(aether_type, "long")   == 0) return "Fiddle::TYPE_LONG_LONG";
    if (strcmp(aether_type, "ulong")  == 0) return "Fiddle::TYPE_LONG_LONG";  /* unsigned view */
    if (strcmp(aether_type, "float")  == 0) return "Fiddle::TYPE_FLOAT";
    if (strcmp(aether_type, "bool")   == 0) return "Fiddle::TYPE_INT";
    if (strcmp(aether_type, "string") == 0) return "Fiddle::TYPE_VOIDP";  /* C string ptr */
    if (strcmp(aether_type, "ptr")    == 0) return "Fiddle::TYPE_VOIDP";
    if (strcmp(aether_type, "void")   == 0) return "Fiddle::TYPE_VOID";
    return NULL;
}

/* Convert snake_case to CamelCase for class / event method names. */
static void to_camel(const char* in, char* out, size_t out_size) {
    size_t i = 0;
    int next_upper = 1;
    for (const char* p = in; *p && i + 1 < out_size; p++) {
        if (*p == '_') { next_upper = 1; continue; }
        out[i++] = next_upper ? (char)toupper((unsigned char)*p) : *p;
        next_upper = 0;
    }
    out[i] = '\0';
}

/* Convert PascalCase or camelCase to snake_case for Ruby method names.
 * Inserts '_' before each uppercase that follows a lowercase or digit.
 *   "OrderPlaced"   -> "order_placed"
 *   "TradeKilled"   -> "trade_killed"
 *   "HTTPResponse"  -> "http_response" (best-effort; consecutive caps
 *                                       collapse into a single run). */
static void to_snake(const char* in, char* out, size_t out_size) {
    size_t i = 0;
    for (const char* p = in; *p && i + 1 < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (p > in && isupper(c)) {
            unsigned char prev = (unsigned char)*(p - 1);
            unsigned char next = (unsigned char)*(p + 1);
            int prev_lower = islower(prev) || isdigit(prev);
            int next_lower = next && islower(next);
            if ((prev_lower || next_lower) && i + 1 < out_size) {
                out[i++] = '_';
            }
        }
        if (i + 1 < out_size) out[i++] = (char)tolower(c);
    }
    out[i] = '\0';
}

/* Generate the Python SDK file for a namespace. Single self-contained
 * .py module — no imports beyond stdlib (ctypes, pathlib). */
static int emit_python_sdk(const CapturedManifest* m,
                           const CapturedFunction* fns, int fn_count,
                           const char* lib_path,
                           const char* out_dir) {
    if (!m->py_module[0]) return 0;  /* no python binding declared */

    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/%s.py", out_dir, m->py_module);
    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write %s\n", out_path);
        return -1;
    }

    /* Convert namespace name to a Python class name (snake_case → CamelCase). */
    char cls[128];
    to_camel(m->ns_name, cls, sizeof(cls));

    fprintf(f,
"\"\"\"Auto-generated Aether namespace binding for `%s`.\n"
"\n"
"Do not edit by hand — regenerated by `ae build --namespace`.\n"
"\n"
"Usage:\n"
"    from %s import %s\n"
"    ns = %s()\n"
"    ns.on_<event>(lambda id: ...)\n"
"    ns.set_<input>(value)\n"
"    result = ns.<function>(args)\n"
"\"\"\"\n"
"import ctypes\n"
"import pathlib\n"
"from typing import Callable, List, Optional\n"
"\n",
        m->ns_name, m->py_module, cls, cls);

    /* Manifest mirror types — small dataclasses populated by walking the
     * AetherNamespaceManifest struct returned by aether_describe(). The
     * struct layout MUST match runtime/aether_host.h. */
    fprintf(f,
"# --- Discovery: mirror of AetherNamespaceManifest in runtime/aether_host.h.\n"
"# Layout MUST stay in sync with the C struct — change both at once.\n"
"\n"
"class _InputDecl(ctypes.Structure):\n"
"    _fields_ = [(\"name\", ctypes.c_char_p),\n"
"                (\"type_signature\", ctypes.c_char_p)]\n"
"\n"
"class _EventDecl(ctypes.Structure):\n"
"    _fields_ = [(\"name\", ctypes.c_char_p),\n"
"                (\"carries_type\", ctypes.c_char_p)]\n"
"\n"
"class _JavaBinding(ctypes.Structure):\n"
"    _fields_ = [(\"package_name\", ctypes.c_char_p),\n"
"                (\"class_name\", ctypes.c_char_p)]\n"
"\n"
"class _PythonBinding(ctypes.Structure):\n"
"    _fields_ = [(\"module_name\", ctypes.c_char_p)]\n"
"\n"
"class _RubyBinding(ctypes.Structure):\n"
"    _fields_ = [(\"module_name\", ctypes.c_char_p)]\n"
"\n"
"class _GoBinding(ctypes.Structure):\n"
"    _fields_ = [(\"package_name\", ctypes.c_char_p)]\n"
"\n"
"class _NamespaceManifest(ctypes.Structure):\n"
"    _fields_ = [(\"namespace_name\", ctypes.c_char_p),\n"
"                (\"input_count\", ctypes.c_int),\n"
"                (\"inputs\", _InputDecl * 64),\n"
"                (\"event_count\", ctypes.c_int),\n"
"                (\"events\", _EventDecl * 64),\n"
"                (\"java\", _JavaBinding),\n"
"                (\"python\", _PythonBinding),\n"
"                (\"ruby\", _RubyBinding),\n"
"                (\"go\", _GoBinding)]\n"
"\n"
"\n"
"class Manifest:\n"
"    \"\"\"Typed view of the namespace's compile-time manifest.\"\"\"\n"
"    def __init__(self, c_manifest: _NamespaceManifest):\n"
"        self.namespace_name = c_manifest.namespace_name.decode() if c_manifest.namespace_name else None\n"
"        self.inputs = [(c_manifest.inputs[i].name.decode(),\n"
"                        c_manifest.inputs[i].type_signature.decode())\n"
"                       for i in range(c_manifest.input_count)]\n"
"        self.events = [(c_manifest.events[i].name.decode(),\n"
"                        c_manifest.events[i].carries_type.decode())\n"
"                       for i in range(c_manifest.event_count)]\n"
"        self.java_package = c_manifest.java.package_name.decode() if c_manifest.java.package_name else None\n"
"        self.java_class   = c_manifest.java.class_name.decode()   if c_manifest.java.class_name   else None\n"
"        self.python_module = c_manifest.python.module_name.decode() if c_manifest.python.module_name else None\n"
"        self.ruby_module   = c_manifest.ruby.module_name.decode()   if c_manifest.ruby.module_name   else None\n"
"        self.go_package    = c_manifest.go.package_name.decode()    if c_manifest.go.package_name    else None\n"
"\n"
"    def __repr__(self):\n"
"        return f\"Manifest(namespace={self.namespace_name!r}, inputs={self.inputs}, events={self.events})\"\n"
"\n");

    /* Default lib path — relative to where the .py lives. The user can
     * override by passing lib_path to the constructor. */
    const char* lib_basename = strrchr(lib_path, '/');
    lib_basename = lib_basename ? lib_basename + 1 : lib_path;
    fprintf(f,
"# Default location of the namespace .so/.dylib. The constructor accepts\n"
"# an override for projects that ship the lib elsewhere.\n"
"_DEFAULT_LIB = pathlib.Path(__file__).parent / \"%s\"\n"
"\n"
"\n"
"class %s:\n"
"    \"\"\"Aether namespace `%s` exposed as a Python class.\"\"\"\n"
"\n"
"    def __init__(self, lib_path: Optional[str] = None):\n"
"        self._lib = ctypes.CDLL(str(lib_path) if lib_path else str(_DEFAULT_LIB))\n"
"        self._callbacks: List = []  # keep refs so the C side keeps working\n"
"\n"
"        # Discovery\n"
"        self._lib.aether_describe.restype = ctypes.POINTER(_NamespaceManifest)\n"
"        self._lib.aether_describe.argtypes = []\n"
"\n"
"        # Event registration (declared in runtime/aether_host.h)\n"
"        self._event_handler_t = ctypes.CFUNCTYPE(None, ctypes.c_int64)\n"
"        self._lib.aether_event_register.restype  = ctypes.c_int\n"
"        self._lib.aether_event_register.argtypes = [ctypes.c_char_p, self._event_handler_t]\n"
"\n",
        lib_basename, cls, m->ns_name);

    /* Bind each script function. */
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;

        const char* ret_ct = py_ctype_for(fn->ret);
        if (!ret_ct) {
            fprintf(stderr, "Warning: skipping Python binding for %s — return type %s not supported\n",
                    fn->name, fn->ret);
            continue;
        }

        /* Verify all params are bindable. */
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!py_ctype_for(fn->params[p].type)) {
                fprintf(stderr, "Warning: skipping Python binding for %s — param %s has unsupported type %s\n",
                        fn->name, fn->params[p].name, fn->params[p].type);
                ok = 0;
                break;
            }
        }
        if (!ok) continue;

        /* C-side aether_<name> bind: argtypes + restype. */
        fprintf(f, "        self._lib.aether_%s.restype = %s\n", fn->name,
                strcmp(fn->ret, "void") == 0 ? "None" : ret_ct);
        fprintf(f, "        self._lib.aether_%s.argtypes = [", fn->name);
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            fputs(py_ctype_for(fn->params[p].type), f);
        }
        fprintf(f, "]\n");
    }

    fprintf(f, "\n");

    /* Per-input setter — stores Python-side, no C call yet (inputs are
     * consumed by scripts at execution time; passing them through is
     * future work tied to host_call(). For v1, set_<input> is a no-op
     * placeholder so the API surface is consistent.) */
    for (int i = 0; i < m->input_count; i++) {
        char setter_name[160];
        snprintf(setter_name, sizeof(setter_name), "set_%s", m->inputs[i].name);
        fprintf(f,
"    def %s(self, value):\n"
"        \"\"\"Stash %s for the script to read. v1: stored on the instance only;\n"
"        a future host_call() bridge will surface it to the running script.\"\"\"\n"
"        self.%s = value\n"
"\n",
            setter_name, m->inputs[i].name, m->inputs[i].name);
    }

    /* Per-event registration. */
    for (int i = 0; i < m->event_count; i++) {
        const char* ev = m->events[i].name;
        fprintf(f,
"    def on_%s(self, handler: Callable[[int], None]):\n"
"        \"\"\"Register a handler for the `%s` event. Holds the callback ref so\n"
"        Python's GC doesn't reclaim the trampoline while C still has a pointer.\"\"\"\n"
"        cb = self._event_handler_t(handler)\n"
"        self._callbacks.append(cb)  # keepalive\n"
"        rc = self._lib.aether_event_register(b\"%s\", cb)\n"
"        if rc != 0:\n"
"            raise RuntimeError(f\"aether_event_register(%s) failed: rc={rc}\")\n"
"\n",
            ev, ev, ev, ev);
    }

    /* Per-function method wrapper. */
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;
        if (!py_ctype_for(fn->ret)) continue;
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!py_ctype_for(fn->params[p].type)) { ok = 0; break; }
        }
        if (!ok) continue;

        fprintf(f, "    def %s(self", fn->name);
        for (int p = 0; p < fn->param_count; p++) {
            fprintf(f, ", %s", fn->params[p].name);
        }
        fprintf(f, "):\n");
        fprintf(f, "        \"\"\"Call the Aether function `%s`.\"\"\"\n", fn->name);

        /* Marshal string args via .encode() */
        for (int p = 0; p < fn->param_count; p++) {
            if (strcmp(fn->params[p].type, "string") == 0) {
                fprintf(f, "        _%s = %s.encode() if isinstance(%s, str) else %s\n",
                        fn->params[p].name, fn->params[p].name,
                        fn->params[p].name, fn->params[p].name);
            }
        }

        fprintf(f, "        result = self._lib.aether_%s(", fn->name);
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            if (strcmp(fn->params[p].type, "string") == 0) {
                fprintf(f, "_%s", fn->params[p].name);
            } else {
                fprintf(f, "%s", fn->params[p].name);
            }
        }
        fprintf(f, ")\n");

        /* Unmarshal string return via .decode() */
        if (strcmp(fn->ret, "string") == 0) {
            fprintf(f, "        return result.decode() if result else None\n");
        } else if (strcmp(fn->ret, "void") == 0) {
            fprintf(f, "        return None\n");
        } else {
            fprintf(f, "        return result\n");
        }
        fprintf(f, "\n");
    }

    /* describe() */
    fprintf(f,
"    def describe(self) -> Manifest:\n"
"        \"\"\"Return the namespace's compile-time manifest as a typed view.\"\"\"\n"
"        ptr = self._lib.aether_describe()\n"
"        if not ptr:\n"
"            raise RuntimeError(\"aether_describe returned NULL\")\n"
"        return Manifest(ptr.contents)\n");

    fclose(f);
    printf("Generated Python SDK: %s\n", out_path);
    return 0;
}

/* Generate the Ruby SDK file. Single self-contained .rb that uses
 * Fiddle (Ruby's stdlib FFI). Same shape as the Python SDK — the
 * pattern translates almost line-for-line. The user-facing API:
 *
 *     require_relative 'calc_sdk'
 *     ns = CalcSdk::Calc.new('./libcalc.so')
 *     ns.set_limit(100)
 *     ns.on_computed { |id| puts "computed #{id}" }
 *     ns.double_it(7)               # => 14
 *     ns.describe.namespace_name    # => "calc"
 */
static int emit_ruby_sdk(const CapturedManifest* m,
                         const CapturedFunction* fns, int fn_count,
                         const char* lib_path,
                         const char* out_dir) {
    if (!m->rb_module[0]) return 0;  /* no ruby binding declared */

    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/%s.rb", out_dir, m->rb_module);
    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write %s\n", out_path);
        return -1;
    }

    /* Module name from the manifest's `ruby("module")` declaration —
     * conventionally snake_case. Class name is the namespace's name
     * mapped to CamelCase. */
    char outer_module[160];
    to_camel(m->rb_module, outer_module, sizeof(outer_module));
    char cls[128];
    to_camel(m->ns_name, cls, sizeof(cls));

    const char* lib_basename = strrchr(lib_path, '/');
    lib_basename = lib_basename ? lib_basename + 1 : lib_path;

    fprintf(f,
"# Auto-generated Aether namespace binding for `%s`.\n"
"#\n"
"# Do not edit by hand — regenerated by `ae build --namespace`.\n"
"#\n"
"# Usage:\n"
"#     require_relative '%s'\n"
"#     ns = %s::%s.new\n"
"#     ns.on_<event> { |id| ... }\n"
"#     ns.set_<input>(value)\n"
"#     result = ns.<function>(args)\n"
"#\n"
"# Requires Ruby's stdlib Fiddle module (ships with MRI Ruby 1.9.2+).\n"
"require 'fiddle'\n"
"require 'fiddle/import'\n"
"\n"
"module %s\n"
"\n"
"# Default location of the namespace .so/.dylib. Constructor accepts an\n"
"# override for projects that ship the lib elsewhere.\n"
"DEFAULT_LIB = File.expand_path('%s', __dir__)\n"
"\n",
        m->ns_name, m->rb_module, outer_module, cls, outer_module, lib_basename);

    /* Manifest mirror — tied to runtime/aether_host.h. Layout MUST stay
     * binary-compatible. */
    fprintf(f,
"# Mirror of AetherNamespaceManifest in runtime/aether_host.h.\n"
"# Layout MUST stay in sync with the C struct — change both at once.\n"
"# These mirror types are unused at runtime today (the Manifest class\n"
"# walks the struct manually with raw pointer reads to avoid CStruct\n"
"# version differences across Fiddle releases) but document the layout\n"
"# for future readers.\n"
"\n");

    /* Manifest typed view — populated from the ptr returned by
     * aether_describe. Walks the same fields as Python's Manifest class. */
    fprintf(f,
"class Manifest\n"
"  attr_reader :namespace_name, :inputs, :events,\n"
"              :java_package, :java_class, :python_module,\n"
"              :ruby_module, :go_package\n"
"\n"
"  def initialize(raw_ptr)\n"
"    base = raw_ptr.to_i\n"
"    # namespace_name: const char* at offset 0\n"
"    @namespace_name = _read_cstr_at(base, 0)\n"
"    # input_count: int at offset 8 (after const char* on 64-bit)\n"
"    input_count = _read_int_at(base, 8)\n"
"    # inputs: AetherInputDecl[64] at offset 16 (4 bytes int + 4 padding)\n"
"    @inputs = []\n"
"    input_count.times do |i|\n"
"      off = 16 + i * 16  # each entry: 2 pointers = 16 bytes on 64-bit\n"
"      @inputs << [_read_cstr_at(base, off), _read_cstr_at(base, off + 8)]\n"
"    end\n"
"    # event_count: int at offset 16 + 16*64 = 1040\n"
"    events_base = 16 + 16 * 64\n"
"    event_count = _read_int_at(base, events_base)\n"
"    @events = []\n"
"    events_arr = events_base + 8  # skip int + 4 padding\n"
"    event_count.times do |i|\n"
"      off = events_arr + i * 16\n"
"      @events << [_read_cstr_at(base, off), _read_cstr_at(base, off + 8)]\n"
"    end\n"
"    # bindings: AetherJavaBinding (16 bytes), AetherPythonBinding (8),\n"
"    #          AetherRubyBinding (8), AetherGoBinding (8)\n"
"    bindings = events_arr + 16 * 64\n"
"    @java_package   = _read_cstr_at(base, bindings)\n"
"    @java_class     = _read_cstr_at(base, bindings + 8)\n"
"    @python_module  = _read_cstr_at(base, bindings + 16)\n"
"    @ruby_module    = _read_cstr_at(base, bindings + 24)\n"
"    @go_package     = _read_cstr_at(base, bindings + 32)\n"
"  end\n"
"\n"
"  def to_s\n"
"    \"Manifest(namespace=#{@namespace_name.inspect}, inputs=#{@inputs.size}, events=#{@events.size})\"\n"
"  end\n"
"\n"
"  private\n"
"\n"
"  def _read_cstr_at(base, offset)\n"
"    # Each pointer field is 8 bytes on 64-bit. Fiddle::Pointer.new(addr)\n"
"    # gives us a typed view; reading the pointer slot then dereferencing\n"
"    # the pointer yields the C string.\n"
"    slot = Fiddle::Pointer.new(base + offset)\n"
"    addr = slot[0, Fiddle::SIZEOF_VOIDP].unpack1('Q')\n"
"    return nil if addr.zero?\n"
"    Fiddle::Pointer.new(addr).to_s\n"
"  end\n"
"\n"
"  def _read_int_at(base, offset)\n"
"    slot = Fiddle::Pointer.new(base + offset)\n"
"    slot[0, 4].unpack1('l')\n"
"  end\n"
"end\n"
"\n");

    /* The main SDK class. Wrap the Fiddle dlopen handle and bind every
     * exported function once at constructor time. */
    fprintf(f,
"class %s\n"
"  attr_accessor",
        cls);
    /* List the input ivars as accessors. */
    for (int i = 0; i < m->input_count; i++) {
        fprintf(f, "%s :%s", i == 0 ? "" : ",", m->inputs[i].name);
    }
    if (m->input_count == 0) fprintf(f, " :_unused");
    fprintf(f, "\n\n");

    fprintf(f,
"  def initialize(lib_path = nil)\n"
"    @lib = Fiddle.dlopen(lib_path || DEFAULT_LIB)\n"
"    @callbacks = []  # keepalive — the C side holds raw fn pointers\n"
"\n"
"    # Discovery + event registration helpers from runtime/aether_host.h.\n"
"    @h_aether_describe = Fiddle::Function.new(\n"
"      @lib['aether_describe'], [], Fiddle::TYPE_VOIDP)\n"
"    @h_aether_event_register = Fiddle::Function.new(\n"
"      @lib['aether_event_register'],\n"
"      [Fiddle::TYPE_VOIDP, Fiddle::TYPE_VOIDP],\n"
"      Fiddle::TYPE_INT)\n"
"\n");

    /* Bind each script function. */
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;
        const char* ret_ft = rb_fiddle_type_for(fn->ret);
        if (!ret_ft) {
            fprintf(stderr, "Warning: skipping Ruby binding for %s — return type %s not supported\n",
                    fn->name, fn->ret);
            continue;
        }
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!rb_fiddle_type_for(fn->params[p].type)) {
                fprintf(stderr, "Warning: skipping Ruby binding for %s — param %s has unsupported type %s\n",
                        fn->name, fn->params[p].name, fn->params[p].type);
                ok = 0; break;
            }
        }
        if (!ok) continue;

        fprintf(f, "    @h_%s = Fiddle::Function.new(\n", fn->name);
        fprintf(f, "      @lib['aether_%s'],\n", fn->name);
        fprintf(f, "      [");
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            fputs(rb_fiddle_type_for(fn->params[p].type), f);
        }
        fprintf(f, "],\n");
        fprintf(f, "      %s)\n", ret_ft);
    }
    fprintf(f, "  end\n\n");

    /* Per-input setter. Ruby has accessors above; setX wraps for symmetry
     * with the Python/Java APIs. */
    for (int i = 0; i < m->input_count; i++) {
        fprintf(f,
"  def set_%s(value)\n"
"    # v1: stored on the instance only; future host_call() bridge will\n"
"    # surface it to the running script.\n"
"    @%s = value\n"
"  end\n\n",
            m->inputs[i].name, m->inputs[i].name);
    }

    /* Per-event handler with proper trampoline keepalive. Ruby methods
     * are snake_case, so PascalCase event names (OrderPlaced) become
     * on_order_placed. */
    for (int i = 0; i < m->event_count; i++) {
        const char* ev = m->events[i].name;
        char ev_snake[160];
        to_snake(ev, ev_snake, sizeof(ev_snake));
        fprintf(f,
"  # Register a handler for the `%s` event. The block receives the int64 id.\n"
"  # Holds the trampoline ref so Ruby's GC doesn't reclaim it while C still\n"
"  # has the function pointer.\n"
"  def on_%s(&handler)\n"
"    cb = Fiddle::Closure::BlockCaller.new(\n"
"      Fiddle::TYPE_VOID, [Fiddle::TYPE_LONG_LONG], &handler)\n"
"    @callbacks << cb  # keepalive\n"
"    name_ptr = Fiddle::Pointer[\"%s\"]\n"
"    rc = @h_aether_event_register.call(name_ptr, cb)\n"
"    raise \"aether_event_register(%s) failed: rc=#{rc}\" if rc != 0\n"
"  end\n\n",
            ev, ev_snake, ev, ev);
    }

    /* Per-function method. Marshal strings to/from C string pointers. */
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;
        if (!rb_fiddle_type_for(fn->ret)) continue;
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!rb_fiddle_type_for(fn->params[p].type)) { ok = 0; break; }
        }
        if (!ok) continue;

        fprintf(f, "  def %s(", fn->name);
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            fprintf(f, "%s", fn->params[p].name);
        }
        fprintf(f, ")\n");

        /* Marshal string args to Fiddle::Pointer wrapped C strings. */
        for (int p = 0; p < fn->param_count; p++) {
            if (strcmp(fn->params[p].type, "string") == 0) {
                fprintf(f, "    _%s = %s.is_a?(String) ? Fiddle::Pointer[%s] : %s\n",
                        fn->params[p].name, fn->params[p].name,
                        fn->params[p].name, fn->params[p].name);
            }
        }

        fprintf(f, "    result = @h_%s.call(", fn->name);
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            if (strcmp(fn->params[p].type, "string") == 0) {
                fprintf(f, "_%s", fn->params[p].name);
            } else {
                fprintf(f, "%s", fn->params[p].name);
            }
        }
        fprintf(f, ")\n");

        if (strcmp(fn->ret, "string") == 0) {
            /* result is an integer address; wrap in Fiddle::Pointer to
             * read the C string. */
            fprintf(f,
"    return nil if result.nil? || result == 0\n"
"    Fiddle::Pointer.new(result.to_i).to_s\n");
        } else if (strcmp(fn->ret, "void") == 0) {
            fprintf(f, "    nil\n");
        } else {
            fprintf(f, "    result\n");
        }
        fprintf(f, "  end\n\n");
    }

    /* describe() */
    fprintf(f,
"  # Return the namespace's compile-time manifest as a typed view.\n"
"  def describe\n"
"    ptr = @h_aether_describe.call\n"
"    raise 'aether_describe returned NULL' if ptr.nil? || ptr.to_i == 0\n"
"    Manifest.new(Fiddle::Pointer.new(ptr.to_i))\n"
"  end\n"
"end  # class\n"
"\n"
"end  # module\n");

    fclose(f);
    printf("Generated Ruby SDK: %s\n", out_path);
    return 0;
}

/* Map an Aether type to the Panama ValueLayout symbolic name (used in
 * FunctionDescriptor) and to the Java method-handle invokeExact return
 * cast / param type. Returns NULL if the type isn't representable. */
static const char* java_layout_for(const char* aether_type) {
    if (strcmp(aether_type, "int")    == 0) return "JAVA_INT";
    if (strcmp(aether_type, "long")   == 0) return "JAVA_LONG";
    if (strcmp(aether_type, "ulong")  == 0) return "JAVA_LONG";   /* signed view */
    if (strcmp(aether_type, "float")  == 0) return "JAVA_FLOAT";
    if (strcmp(aether_type, "bool")   == 0) return "JAVA_INT";
    if (strcmp(aether_type, "string") == 0) return "ADDRESS";
    if (strcmp(aether_type, "ptr")    == 0) return "ADDRESS";
    return NULL;
}
static const char* java_jtype_for(const char* aether_type) {
    if (strcmp(aether_type, "int")    == 0) return "int";
    if (strcmp(aether_type, "long")   == 0) return "long";
    if (strcmp(aether_type, "ulong")  == 0) return "long";
    if (strcmp(aether_type, "float")  == 0) return "float";
    if (strcmp(aether_type, "bool")   == 0) return "int";
    if (strcmp(aether_type, "string") == 0) return "String";
    if (strcmp(aether_type, "ptr")    == 0) return "MemorySegment";
    if (strcmp(aether_type, "void")   == 0) return "void";
    return NULL;
}

/* Convert snake_case to camelCase for Java method names. Simpler than
 * to_camel above — Java methods start lowercase. */
static void to_lower_camel(const char* in, char* out, size_t out_size) {
    size_t i = 0;
    int next_upper = 0;
    int first = 1;
    for (const char* p = in; *p && i + 1 < out_size; p++) {
        if (*p == '_') { next_upper = 1; continue; }
        if (first) { out[i++] = (char)tolower((unsigned char)*p); first = 0; }
        else       { out[i++] = next_upper ? (char)toupper((unsigned char)*p) : *p; }
        next_upper = 0;
    }
    out[i] = '\0';
}

/* Generate a Java SDK file. Targets Java 22+ (Panama stable). The
 * generated class is self-contained — no external deps beyond the JDK
 * — so consumers compile with `javac` and run with
 *   java --enable-native-access=ALL-UNNAMED -cp ... MyApp
 * (or the more restrictive --enable-native-access=<module>). */
static int emit_java_sdk(const CapturedManifest* m,
                         const CapturedFunction* fns, int fn_count,
                         const char* lib_path,
                         const char* out_dir) {
    if (!m->java_class[0] || !m->java_pkg[0]) return 0;

    /* package name → directory path: com.example.foo → com/example/foo */
    char pkg_dir[1024];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", out_dir, m->java_pkg);
    for (char* p = pkg_dir + strlen(out_dir); *p; p++) {
        if (*p == '.') *p = '/';
    }
    mkdirs(pkg_dir);

    char out_path[1280];
    snprintf(out_path, sizeof(out_path), "%s/%s.java", pkg_dir, m->java_class);
    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write %s\n", out_path);
        return -1;
    }

    /* Default lib path — relative to the .java's compiled class location
     * is brittle, so we accept a constructor argument and document the
     * default as the basename of the .so for users who put both files
     * side by side in their resources. */
    const char* lib_basename = strrchr(lib_path, '/');
    lib_basename = lib_basename ? lib_basename + 1 : lib_path;

    fprintf(f,
"/*\n"
" * Auto-generated Aether namespace binding for `%s`.\n"
" * Do not edit by hand — regenerated by `ae build --namespace`.\n"
" *\n"
" * Requires Java 22+ (Foreign Function & Memory API). Run with:\n"
" *   java --enable-native-access=ALL-UNNAMED -cp ... YourApp\n"
" *\n"
" * Usage:\n"
" *   %s.%s ns = new %s.%s(\"./%s\");\n"
" *   ns.on<EventName>(id -> ...);\n"
" *   ns.set<InputName>(value);\n"
" *   var result = ns.<functionName>(args);\n"
" */\n"
"package %s;\n"
"\n"
"import java.lang.foreign.*;\n"
"import java.lang.invoke.*;\n"
"import java.nio.file.*;\n"
"import java.util.*;\n"
"import java.util.function.*;\n"
"import static java.lang.foreign.ValueLayout.*;\n"
"\n",
        m->ns_name,
        m->java_pkg, m->java_class, m->java_pkg, m->java_class, lib_basename,
        m->java_pkg);

    /* Class header + state. */
    fprintf(f,
"public class %s implements AutoCloseable {\n"
"\n"
"    private final Arena arena = Arena.ofShared();\n"
"    private final SymbolLookup lib;\n"
"    private final Linker linker = Linker.nativeLinker();\n"
"\n"
"    /** Holds upcall stubs so the JVM doesn't reclaim them while the\n"
"     *  C side still has function pointers. */\n"
"    private final List<MemorySegment> _callbackKeepalive = new ArrayList<>();\n"
"\n",
        m->java_class);

    /* Cached method handles for every function + the runtime helpers. */
    fprintf(f,
"    private final MethodHandle h_aether_event_register;\n"
"    private final MethodHandle h_aether_describe;\n");
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;
        if (!java_jtype_for(fn->ret)) continue;
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!java_jtype_for(fn->params[p].type)) { ok = 0; break; }
        }
        if (!ok) continue;
        fprintf(f, "    private final MethodHandle h_%s;\n", fn->name);
    }

    /* Input fields (v1: stored on the instance, public so callers can
     * also read them back). */
    fprintf(f, "\n");
    for (int i = 0; i < m->input_count; i++) {
        /* Input types come from the manifest as freeform strings
         * ("int", "string", "fn(string) -> bool", "map", etc.). For v1,
         * Java fields are typed only when the type is in the simple
         * vocabulary; everything else falls back to Object. */
        const char* jt = java_jtype_for(m->inputs[i].type);
        if (!jt) jt = "Object";
        fprintf(f, "    public %s %s;\n", jt, m->inputs[i].name);
    }
    fprintf(f, "\n");

    /* Constructor */
    fprintf(f,
"    public %s(String libPath) {\n"
"        this.lib = SymbolLookup.libraryLookup(Path.of(libPath), arena);\n"
"        h_aether_event_register = linker.downcallHandle(\n"
"            lib.find(\"aether_event_register\").orElseThrow(),\n"
"            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));\n"
"        h_aether_describe = linker.downcallHandle(\n"
"            lib.find(\"aether_describe\").orElseThrow(),\n"
"            FunctionDescriptor.of(ADDRESS));\n",
        m->java_class);

    /* Bind each script function. */
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;
        if (!java_jtype_for(fn->ret)) continue;
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!java_jtype_for(fn->params[p].type)) { ok = 0; break; }
        }
        if (!ok) {
            fprintf(stderr, "Warning: skipping Java binding for %s — unsupported type\n", fn->name);
            continue;
        }
        fprintf(f, "        h_%s = linker.downcallHandle(\n", fn->name);
        fprintf(f, "            lib.find(\"aether_%s\").orElseThrow(),\n", fn->name);
        fprintf(f, "            FunctionDescriptor.");
        if (strcmp(fn->ret, "void") == 0) {
            fprintf(f, "ofVoid(");
            for (int p = 0; p < fn->param_count; p++) {
                if (p > 0) fputs(", ", f);
                fputs(java_layout_for(fn->params[p].type), f);
            }
            fputs("));\n", f);
        } else {
            fprintf(f, "of(%s", java_layout_for(fn->ret));
            for (int p = 0; p < fn->param_count; p++) {
                fprintf(f, ", %s", java_layout_for(fn->params[p].type));
            }
            fputs("));\n", f);
        }
    }
    fprintf(f, "    }\n\n");

    /* Per-input setter (camelCase). */
    for (int i = 0; i < m->input_count; i++) {
        char input_camel[160];
        to_camel(m->inputs[i].name, input_camel, sizeof(input_camel));
        char setter[168];  // input_camel + "set" + NUL with headroom
        snprintf(setter, sizeof(setter), "set%s", input_camel);
        const char* jt = java_jtype_for(m->inputs[i].type);
        if (!jt) jt = "Object";
        fprintf(f,
"    public void %s(%s value) {\n"
"        /* v1: stored on the instance; future host_call() will surface to script. */\n"
"        this.%s = value;\n"
"    }\n\n",
            setter, jt, m->inputs[i].name);
    }

    /* Per-event registrar: on<EventName>(LongConsumer handler). */
    for (int i = 0; i < m->event_count; i++) {
        const char* ev = m->events[i].name;
        char ev_camel[160];
        to_camel(ev, ev_camel, sizeof(ev_camel));
        fprintf(f,
"    public void on%s(LongConsumer handler) {\n"
"        try {\n"
"            /* Look up LongConsumer.accept (a public interface method) and\n"
"             * bind to the user-supplied lambda. We don't bind directly\n"
"             * via lookup().bind(handler, ...) because the lambda's class\n"
"             * is nestmate-private and the lookup from this generated\n"
"             * class can't reach it. */\n"
"            MethodHandle target = MethodHandles.publicLookup()\n"
"                .findVirtual(LongConsumer.class, \"accept\",\n"
"                    MethodType.methodType(void.class, long.class))\n"
"                .bindTo(handler);\n"
"            MemorySegment stub = linker.upcallStub(\n"
"                target,\n"
"                FunctionDescriptor.ofVoid(JAVA_LONG),\n"
"                arena);\n"
"            _callbackKeepalive.add(stub);\n"
"            int rc = (int) h_aether_event_register.invokeExact(\n"
"                arena.allocateFrom(\"%s\"), stub);\n"
"            if (rc != 0) throw new RuntimeException(\"aether_event_register %s: rc=\" + rc);\n"
"        } catch (Throwable t) { throw new RuntimeException(t); }\n"
"    }\n\n",
            ev_camel, ev, ev);
    }

    /* Per-function method (lowerCamel). */
    for (int i = 0; i < fn_count; i++) {
        const CapturedFunction* fn = &fns[i];
        if (is_skipped_function(fn->name)) continue;
        if (!java_jtype_for(fn->ret)) continue;
        int ok = 1;
        for (int p = 0; p < fn->param_count; p++) {
            if (!java_jtype_for(fn->params[p].type)) { ok = 0; break; }
        }
        if (!ok) continue;

        char m_camel[160];
        to_lower_camel(fn->name, m_camel, sizeof(m_camel));
        const char* jret = java_jtype_for(fn->ret);

        fprintf(f, "    public %s %s(", jret, m_camel);
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            fprintf(f, "%s %s", java_jtype_for(fn->params[p].type), fn->params[p].name);
        }
        fprintf(f, ") {\n");
        fprintf(f, "        try {\n");

        /* Marshal string args via arena.allocateFrom. */
        for (int p = 0; p < fn->param_count; p++) {
            if (strcmp(fn->params[p].type, "string") == 0) {
                fprintf(f, "            MemorySegment _%s = arena.allocateFrom(%s);\n",
                        fn->params[p].name, fn->params[p].name);
            }
        }

        /* Build the invokeExact arg list. */
        const char* invoke_cast =
            strcmp(jret, "void")  == 0 ? "" :
            strcmp(jret, "int")   == 0 ? "(int) " :
            strcmp(jret, "long")  == 0 ? "(long) " :
            strcmp(jret, "float") == 0 ? "(float) " :
            "(MemorySegment) ";

        if (strcmp(jret, "void") == 0) {
            fprintf(f, "            h_%s.invokeExact(", fn->name);
        } else if (strcmp(fn->ret, "string") == 0) {
            fprintf(f, "            MemorySegment _r = (MemorySegment) h_%s.invokeExact(", fn->name);
        } else {
            fprintf(f, "            return %sh_%s.invokeExact(", invoke_cast, fn->name);
        }
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) fputs(", ", f);
            if (strcmp(fn->params[p].type, "string") == 0) {
                fprintf(f, "_%s", fn->params[p].name);
            } else {
                fprintf(f, "%s", fn->params[p].name);
            }
        }
        fprintf(f, ");\n");

        if (strcmp(jret, "void") == 0) {
            /* nothing to return */
        } else if (strcmp(fn->ret, "string") == 0) {
            fprintf(f,
"            if (_r.equals(MemorySegment.NULL)) return null;\n"
"            return _r.reinterpret(Long.MAX_VALUE).getString(0);\n");
        }

        fprintf(f,
"        } catch (Throwable t) { throw new RuntimeException(t); }\n"
"    }\n\n");
    }

    /* Manifest accessor — describe(). */
    fprintf(f,
"    /** Native-side manifest layout — must mirror runtime/aether_host.h. */\n"
"    private static final MemoryLayout INPUT_DECL = MemoryLayout.structLayout(\n"
"        ADDRESS.withName(\"name\"),\n"
"        ADDRESS.withName(\"type_signature\"));\n"
"    private static final MemoryLayout EVENT_DECL = MemoryLayout.structLayout(\n"
"        ADDRESS.withName(\"name\"),\n"
"        ADDRESS.withName(\"carries_type\"));\n"
"\n"
"    /** Typed view of the namespace's compile-time manifest. */\n"
"    public static final class Manifest {\n"
"        public final String namespaceName;\n"
"        public final List<String[]> inputs;  // each: { name, type }\n"
"        public final List<String[]> events;  // each: { name, carries }\n"
"        public final String javaPackage, javaClass, pythonModule,\n"
"                            rubyModule, goPackage;\n"
"\n"
"        Manifest(String ns, List<String[]> in, List<String[]> ev,\n"
"                 String jp, String jc, String pm, String rm, String gp) {\n"
"            this.namespaceName = ns;\n"
"            this.inputs = in;\n"
"            this.events = ev;\n"
"            this.javaPackage = jp; this.javaClass = jc;\n"
"            this.pythonModule = pm; this.rubyModule = rm;\n"
"            this.goPackage = gp;\n"
"        }\n"
"        @Override public String toString() {\n"
"            return \"Manifest(namespace=\\\"\" + namespaceName + \"\\\", inputs=\" + inputs.size()\n"
"                + \", events=\" + events.size() + \")\";\n"
"        }\n"
"    }\n"
"\n"
"    /** Walk the AetherNamespaceManifest static struct in the .so and\n"
"     *  return a typed copy. Layout must stay in sync with the C struct. */\n"
"    public Manifest describe() {\n"
"        try {\n"
"            MemorySegment p = (MemorySegment) h_aether_describe.invokeExact();\n"
"            if (p.equals(MemorySegment.NULL))\n"
"                throw new RuntimeException(\"aether_describe returned NULL\");\n"
"            MemorySegment view = p.reinterpret(8 + 4 + 16 * 64 + 4 + 16 * 64 + 16 + 8 + 8 + 8 + 8);\n"
"            String ns = view.get(ADDRESS, 0).reinterpret(Long.MAX_VALUE).getString(0);\n"
"            int inputCount = view.get(JAVA_INT, 8);\n"
"            List<String[]> inputs = new ArrayList<>();\n"
"            long base = 16; // after namespace_name(8) + input_count(4) + 4 padding\n"
"            for (int i = 0; i < inputCount; i++) {\n"
"                long off = base + (long)i * 16;\n"
"                MemorySegment nm = view.get(ADDRESS, off);\n"
"                MemorySegment ty = view.get(ADDRESS, off + 8);\n"
"                inputs.add(new String[]{\n"
"                    nm.equals(MemorySegment.NULL) ? null : nm.reinterpret(Long.MAX_VALUE).getString(0),\n"
"                    ty.equals(MemorySegment.NULL) ? null : ty.reinterpret(Long.MAX_VALUE).getString(0)});\n"
"            }\n"
"            long eventsBase = 16 + 16L * 64;     // after inputs[64]\n"
"            int eventCount = view.get(JAVA_INT, eventsBase);\n"
"            long eventsArr = eventsBase + 8;     // skip int+pad\n"
"            List<String[]> events = new ArrayList<>();\n"
"            for (int i = 0; i < eventCount; i++) {\n"
"                long off = eventsArr + (long)i * 16;\n"
"                MemorySegment nm = view.get(ADDRESS, off);\n"
"                MemorySegment ca = view.get(ADDRESS, off + 8);\n"
"                events.add(new String[]{\n"
"                    nm.equals(MemorySegment.NULL) ? null : nm.reinterpret(Long.MAX_VALUE).getString(0),\n"
"                    ca.equals(MemorySegment.NULL) ? null : ca.reinterpret(Long.MAX_VALUE).getString(0)});\n"
"            }\n"
"            long bindings = eventsArr + 16L * 64;\n"
"            MemorySegment jp = view.get(ADDRESS, bindings);\n"
"            MemorySegment jc = view.get(ADDRESS, bindings + 8);\n"
"            MemorySegment pm = view.get(ADDRESS, bindings + 16);\n"
"            MemorySegment rm = view.get(ADDRESS, bindings + 24);\n"
"            MemorySegment gp = view.get(ADDRESS, bindings + 32);\n"
"            return new Manifest(ns, inputs, events,\n"
"                jp.equals(MemorySegment.NULL) ? null : jp.reinterpret(Long.MAX_VALUE).getString(0),\n"
"                jc.equals(MemorySegment.NULL) ? null : jc.reinterpret(Long.MAX_VALUE).getString(0),\n"
"                pm.equals(MemorySegment.NULL) ? null : pm.reinterpret(Long.MAX_VALUE).getString(0),\n"
"                rm.equals(MemorySegment.NULL) ? null : rm.reinterpret(Long.MAX_VALUE).getString(0),\n"
"                gp.equals(MemorySegment.NULL) ? null : gp.reinterpret(Long.MAX_VALUE).getString(0));\n"
"        } catch (Throwable t) { throw new RuntimeException(t); }\n"
"    }\n"
"\n"
"    @Override public void close() { arena.close(); }\n"
"}\n");

    fclose(f);
    printf("Generated Java SDK: %s\n", out_path);
    return 0;
}

/* Driver: gather manifest + function list, dispatch to per-language emitters. */
static void emit_namespace_bindings(const char* manifest_path,
                                    const char* concat_path,
                                    const char* lib_path,
                                    const char* dir) {
    /* Run aetherc --emit-namespace-manifest to capture JSON. */
    char json[16384];
    if (aetherc_capture_stdout("--emit-namespace-manifest", manifest_path,
                               NULL, json, sizeof(json)) != 0) {
        fprintf(stderr, "Warning: --emit-namespace-manifest failed; skipping SDK generation\n");
        return;
    }

    CapturedManifest m;
    if (parse_manifest_json(json, &m) != 0) {
        fprintf(stderr, "Warning: could not parse manifest JSON; skipping SDK generation\n");
        return;
    }

    /* Run aetherc --list-functions on the synthetic concat file. */
    char fn_list[16384];
    if (aetherc_capture_stdout("--list-functions", concat_path,
                               NULL, fn_list, sizeof(fn_list)) != 0) {
        fprintf(stderr, "Warning: --list-functions failed; skipping SDK generation\n");
        return;
    }

    CapturedFunction fns[64];
    int fn_count = parse_function_list(fn_list, fns, 64);

    /* Determine where to write SDKs. Place them next to the .so so
     * users can `cp libfoo.so foo_module.py /target/` together. */
    char out_dir[1024];
    strncpy(out_dir, lib_path, sizeof(out_dir) - 1);
    out_dir[sizeof(out_dir) - 1] = '\0';
    char* slash = strrchr(out_dir, '/');
    if (slash) *slash = '\0';
    else strcpy(out_dir, ".");

    if (m.py_module[0]) {
        emit_python_sdk(&m, fns, fn_count, lib_path, out_dir);
    }
    if (m.rb_module[0]) {
        emit_ruby_sdk(&m, fns, fn_count, lib_path, out_dir);
    }
    if (m.java_class[0] && m.java_pkg[0]) {
        emit_java_sdk(&m, fns, fn_count, lib_path, out_dir);
    }

    (void)dir;  /* may be needed for relative-path resolution later */
}

// =============================================================================
// `ae build --namespace <dir>` — build a namespace into a single .so
//
// A namespace is a directory containing:
//   - manifest.ae               (declares namespace name, inputs, events,
//                                 bindings — see std.host module DSL)
//   - one or more sibling *.ae  (contribute their top-level functions
//                                 to the namespace; auto-discovered by
//                                 directory convention)
//
// The pipeline:
//   1. Find <dir>/manifest.ae. Error if missing.
//   2. Run aetherc --emit-namespace-describe to produce a .c stub
//      containing the static AetherNamespaceManifest + aether_describe().
//   3. Discover sibling .ae files (everything under <dir> except
//      manifest.ae and files marked @private — annotation deferred to
//      a later chunk; for v1 every sibling is included).
//   4. Concatenate all sibling .ae files into one synthetic .ae and
//      compile via the existing --emit=lib pipeline. (Single-file
//      compile fits the one-file-per-build constraint of aetherc.)
//   5. Link the describe.c stub alongside the resulting .c into a
//      single libnamespace.so.
//
// The default output is lib<namespace>.so (or .dylib on macOS), placed
// in the current directory unless -o is supplied.
// =============================================================================

#include <dirent.h>

int cmd_build_namespace(int argc, char** argv) {
    const char* dir = NULL;
    const char* output_name = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--namespace") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_name = argv[++i];
        }
    }

    if (!dir) {
        fprintf(stderr, "Error: --namespace requires a directory argument\n");
        return 1;
    }
    if (!dir_exists(dir)) {
        fprintf(stderr, "Error: namespace directory '%s' not found\n", dir);
        return 1;
    }

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.ae", dir);
    if (!path_exists(manifest_path)) {
        fprintf(stderr, "Error: %s not found — every namespace needs a manifest.ae\n", manifest_path);
        return 1;
    }

#ifdef __APPLE__
    const char* lib_ext = ".dylib";
#elif defined(_WIN32)
    const char* lib_ext = ".dll";
#else
    const char* lib_ext = ".so";
#endif

    /* Set up a temp workspace for the synthesized .ae, the .c outputs,
     * and the describe stub. Nothing here outlives the build. */
    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/aether_ns_%d", get_temp_dir(), (int)getpid());
    mkdirs(tmpdir);

    /* Step 1: produce the describe.c stub from manifest.ae. */
    char describe_c[1056];  // tmpdir[1024] + "/aether_describe.c" + NUL
    snprintf(describe_c, sizeof(describe_c), "%s/aether_describe.c", tmpdir);
    char cmd[16384];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" --emit-namespace-describe \"%s\" \"%s\"",
        tc.compiler, manifest_path, describe_c);
    if (run_cmd_quiet(cmd) != 0) {
        fprintf(stderr, "Error: aetherc --emit-namespace-describe failed\n");
        fprintf(stderr, "       cmd: %s\n", cmd);
        return 1;
    }
    if (!path_exists(describe_c)) {
        fprintf(stderr, "Error: describe stub was not produced at %s\n", describe_c);
        return 1;
    }

    /* Step 2: discover sibling .ae files (skip manifest.ae). Sort by
     * name for reproducible build output. */
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "Error: opendir(%s) failed\n", dir);
        return 1;
    }
    char  siblings[64][512];
    int   sibling_count = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        size_t n = strlen(name);
        if (n < 4) continue;
        if (strcmp(name + n - 3, ".ae") != 0) continue;
        if (strcmp(name, "manifest.ae") == 0) continue;
        if (sibling_count >= 64) break;
        snprintf(siblings[sibling_count++], 512, "%s/%s", dir, name);
    }
    closedir(d);

    if (sibling_count == 0) {
        fprintf(stderr, "Error: namespace '%s' contains a manifest but no scripts (*.ae)\n", dir);
        return 1;
    }

    /* sort with qsort+strcmp for determinism */
    for (int i = 1; i < sibling_count; i++) {
        for (int j = i; j > 0 && strcmp(siblings[j], siblings[j-1]) < 0; j--) {
            char tmp[512];
            strncpy(tmp, siblings[j], sizeof(tmp));
            strncpy(siblings[j], siblings[j-1], sizeof(siblings[j]));
            strncpy(siblings[j-1], tmp, sizeof(siblings[j-1]));
        }
    }

    /* Step 3: concatenate the siblings into one synthetic .ae. We
     * deduplicate `import` lines (a script uses `import std.host` for
     * notify/manifest builders; concatenating two such siblings would
     * import twice). Everything else passes through unchanged. */
    char concat_path[1056];  // tmpdir[1024] + "/_namespace.ae" + NUL
    snprintf(concat_path, sizeof(concat_path), "%s/_namespace.ae", tmpdir);
    FILE* concat = fopen(concat_path, "w");
    if (!concat) { perror("fopen concat"); return 1; }

    /* Track imports we've already emitted to avoid duplicates. */
    char seen_imports[64][128];
    int  seen_count = 0;
    int  has_main = 0;

    for (int i = 0; i < sibling_count; i++) {
        FILE* in = fopen(siblings[i], "r");
        if (!in) {
            fprintf(stderr, "Error: cannot read sibling %s\n", siblings[i]);
            fclose(concat);
            return 1;
        }
        fprintf(concat, "// === from %s ===\n", siblings[i]);
        char line[2048];
        while (fgets(line, sizeof(line), in)) {
            /* Detect duplicate import lines. */
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "import ", 7) == 0) {
                int dup = 0;
                for (int s = 0; s < seen_count; s++) {
                    if (strcmp(seen_imports[s], p) == 0) { dup = 1; break; }
                }
                if (dup) continue;
                if (seen_count < 64) {
                    strncpy(seen_imports[seen_count], p, sizeof(seen_imports[0]) - 1);
                    seen_imports[seen_count][sizeof(seen_imports[0]) - 1] = '\0';
                    seen_count++;
                }
            }
            /* Skip duplicate main()s — keep only the first. */
            if (strncmp(p, "main(", 5) == 0 || strncmp(p, "main (", 6) == 0) {
                if (has_main) {
                    /* Skip until matching close brace. Naive but
                     * sufficient for a synthesized namespace where
                     * scripts shouldn't normally have main(). */
                    int depth = 0;
                    int seen_open = 0;
                    while (fgets(line, sizeof(line), in)) {
                        for (char* q = line; *q; q++) {
                            if (*q == '{') { depth++; seen_open = 1; }
                            else if (*q == '}') { depth--; if (seen_open && depth <= 0) goto done_main; }
                        }
                    }
                done_main:
                    continue;
                }
                has_main = 1;
            }
            fputs(line, concat);
        }
        fputs("\n", concat);
        fclose(in);
    }

    /* If no script declared main(), emit a synthetic one so --emit=lib
     * is happy (it tolerates main() but the lib drops it). */
    if (!has_main) {
        fputs("\nmain() {}\n", concat);
    }
    fclose(concat);

    /* Step 4: derive the output library path. The artifacts live INSIDE
     * <dir> by default so they sit next to the manifest and scripts that
     * produced them — easy to ship as a unit. -o overrides that.
     *
     * Naming: lib<basename>.so (or .dylib), where <basename> is the
     * tail component of <dir>:
     *   --namespace trading/   →  trading/libtrading.so
     *   --namespace .          →  ./lib<cwd_basename>.so
     */
    /* Normalize dir: strip trailing slash so target_dir works for both
     * "aether" and "aether/". */
    char target_dir[1024];
    {
        strncpy(target_dir, dir, sizeof(target_dir) - 1);
        target_dir[sizeof(target_dir) - 1] = '\0';
        size_t dlen = strlen(target_dir);
        while (dlen > 1 && target_dir[dlen - 1] == '/') {
            target_dir[--dlen] = '\0';
        }
    }

    /* Derive the library basename. Prefer -o, then the manifest's
     * namespace name (read by re-invoking aetherc to dump the JSON
     * manifest), then the directory's basename as a fallback. The
     * manifest name is what users actually want — `namespace("trading")`
     * → libtrading.so. */
    char base_name[512];
    if (output_name) {
        strncpy(base_name, output_name, sizeof(base_name) - 1);
        base_name[sizeof(base_name) - 1] = '\0';
    } else {
        char ns_json[16384];
        char ns_name[256] = "";
        if (aetherc_capture_stdout("--emit-namespace-manifest", manifest_path,
                                   NULL, ns_json, sizeof(ns_json)) == 0) {
            json_extract_string_field(ns_json, "namespace", ns_name, sizeof(ns_name));
        }
        if (ns_name[0]) {
            strncpy(base_name, ns_name, sizeof(base_name) - 1);
            base_name[sizeof(base_name) - 1] = '\0';
        } else {
            /* Fallback: directory basename. */
            const char* base = target_dir;
            if (strcmp(target_dir, ".") == 0) {
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) {
                    const char* slash = strrchr(cwd, '/');
                    base = slash ? slash + 1 : cwd;
                }
            } else {
                const char* slash = strrchr(target_dir, '/');
                if (slash) base = slash + 1;
            }
            strncpy(base_name, base, sizeof(base_name) - 1);
            base_name[sizeof(base_name) - 1] = '\0';
        }
    }

    /* Full output path with lib<base><ext>, anchored under target_dir. */
    char out_path[2400];
    snprintf(out_path, sizeof(out_path), "%s/lib%s%s", target_dir, base_name, lib_ext);

    /* Step 5: build the synthetic .ae as --emit=lib, then re-link with
     * the describe.c stub appended. We piggy-back on the existing
     * pipeline: invoke cmd_build with --emit=lib --extra <describe.c>.
     * cmd_build's output-name override (lib<X>.so) only fires when -o
     * is omitted; we pass an explicit -o that already has the lib<>
     * prefix and the .ext, but cmd_build appends EXE_EXT to the -o
     * value as-is. To make sure no extra extension creeps in, we strip
     * the trailing lib_ext and let cmd_build's lib-mode logic re-add
     * it (or actually, since we pass -o, cmd_build uses the value
     * literally — see cmd_build l.1532). So pass the path WITHOUT
     * the .so/.dylib/.dll suffix and let cmd_build's existing override
     * take effect. */
    char out_no_ext[1024];
    strncpy(out_no_ext, out_path, sizeof(out_no_ext) - 1);
    out_no_ext[sizeof(out_no_ext) - 1] = '\0';
    char* dot = strrchr(out_no_ext, '.');
    if (dot && (strcmp(dot, ".so") == 0 || strcmp(dot, ".dylib") == 0 || strcmp(dot, ".dll") == 0)) {
        *dot = '\0';
    }

    g_emit_lib = true;
    g_emit_exe = false;

    char* sub_argv[10];
    int sub_argc = 0;
    sub_argv[sub_argc++] = (char*)concat_path;
    sub_argv[sub_argc++] = (char*)"--emit=lib";
    sub_argv[sub_argc++] = (char*)"--extra";
    sub_argv[sub_argc++] = (char*)describe_c;
    sub_argv[sub_argc++] = (char*)"-o";
    sub_argv[sub_argc++] = out_no_ext;

    int rc = cmd_build(sub_argc, sub_argv);
    if (rc == 0) {
        /* cmd_build with -o uses the value literally with EXE_EXT (empty
         * on POSIX), so the actual file at this point is `out_no_ext`
         * with no extension. Rename to add the proper lib extension. */
        if (path_exists(out_no_ext) && !path_exists(out_path)) {
            if (rename(out_no_ext, out_path) != 0) {
                /* Rename failed; report what's actually there. */
                fprintf(stderr, "Warning: built %s but couldn't rename to %s\n",
                        out_no_ext, out_path);
                printf("Built namespace: %s\n", out_no_ext);
                return rc;
            }
        }
#ifdef __APPLE__
        /* macOS clang bakes the `-o` value into the dylib's install_name
         * at link time (Linux ld does not record a SONAME unless asked).
         * Because we pass `-o out_no_ext` to get the base name right and
         * rename afterwards, the library now has install_name equal to
         * the extension-less interim path. Any consumer statically linked
         * against the dylib inherits that broken path as its load-time
         * dependency (e.g. `./libgreet`), which dyld cannot resolve.
         *
         * Rewrite the id to @rpath/<basename> so consumers that pass
         * -Wl,-rpath,<dir> at link time can find the lib regardless of
         * where it was built. */
        {
            const char* base = strrchr(out_path, '/');
            base = base ? base + 1 : out_path;
            char id_cmd[4096];
            snprintf(id_cmd, sizeof(id_cmd),
                     "install_name_tool -id '@rpath/%s' '%s' 2>/dev/null",
                     base, out_path);
            if (system(id_cmd) != 0) {
                fprintf(stderr, "Warning: install_name_tool failed on %s; "
                                "consumers may fail to dlopen.\n", out_path);
            }
        }
#endif
        printf("Built namespace: %s\n", out_path);

        /* Step 6: per-language SDK generation. Reads the manifest JSON
         * + the function list, then dispatches to the emitter for each
         * binding target the manifest declared. */
        emit_namespace_bindings(manifest_path, concat_path, out_path, dir);
    }
    return rc;
}


static int cmd_build(int argc, char** argv) {
    const char* file = NULL;
    const char* output_name = NULL;
    /* 8 KiB matches toml_extra below + the fgets line buffer in
     * get_extra_sources_for_bin. Needs to fit --extra CLI args plus
     * the full TOML extra_sources concatenated. */
    char extra_files[8192] = "";

    const char* target = NULL;
    bool quick = false;

    // Reset emit mode to the default (exe-only) for this build.
    g_emit_exe = true;
    g_emit_lib = false;
    // Reset coverage flag — `ae build --coverage` enables it per-build.
    g_coverage = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_name = argv[++i];
        } else if (strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target = argv[++i];
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            target = argv[i] + 9;
        } else if (strcmp(argv[i], "--extra") == 0 && i + 1 < argc) {
            if (extra_files[0]) strncat(extra_files, " ", sizeof(extra_files) - strlen(extra_files) - 1);
            strncat(extra_files, argv[++i], sizeof(extra_files) - strlen(extra_files) - 1);
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            /* Issue #413: same append semantics as `ae run` — see
             * cmd_run's --lib handler for the full rationale.
             * Repeated flags + separator-strings both feed the
             * aetherc-side multi-entry search path. */
            tc_lib_dir_append(argv[++i]);
        } else if (strncmp(argv[i], "--with=", 7) == 0) {
            // Capability opt-ins for --emit=lib. Forwarded verbatim to
            // aetherc; parsing, validation, and the reject messages all
            // happen there to keep the single source of truth.
            strncpy(g_with_caps, argv[i] + 7, sizeof(g_with_caps) - 1);
            g_with_caps[sizeof(g_with_caps) - 1] = '\0';
        } else if (strcmp(argv[i], "--coverage") == 0) {
            // Inject `--coverage` into the gcc invocation so the binary
            // emits .gcda files at runtime and .gcno files alongside .o
            // at build time. The user/test runs the binary, then `gcov`
            // walks the .gcda files and (thanks to PR #352's #line
            // directives) produces .ae.gcov reports attributed back to
            // .ae source. Forces -O0 -g — gcov line attribution is
            // unreliable at -O2 because of inlining and block merging.
            g_coverage = true;
        } else if (strncmp(argv[i], "--emit=", 7) == 0) {
            const char* val = argv[i] + 7;
            if (strcmp(val, "exe") == 0) {
                g_emit_exe = true;
                g_emit_lib = false;
            } else if (strcmp(val, "lib") == 0) {
                g_emit_exe = false;
                g_emit_lib = true;
            } else if (strcmp(val, "both") == 0) {
                /* `ae build --emit=both` produces both an executable and
                 * a shared library from a single source. Implementation:
                 * dispatch cmd_build twice — first as --emit=exe, then
                 * as --emit=lib — using a duplicated argv with the flag
                 * rewritten in place. Two gcc calls, yes, but that's
                 * what producing two ELFs from one source genuinely
                 * costs — the .c file content for exe and lib differ
                 * on whether `main` is emitted, so a single gcc call
                 * can't produce both shapes anyway.
                 *
                 * Output paths: when the user passes `-o NAME` we keep
                 * NAME for the exe pass and append the platform lib
                 * extension (`NAME.dylib` / `NAME.so`) for the lib
                 * pass — otherwise the lib pass would overwrite the
                 * exe at the same path. When `-o` is absent both
                 * passes use their defaults (exe = `<src-base>`,
                 * lib = `lib<src-base>.<ext>`) which already differ.
                 *
                 * If the exe pass fails the lib pass is skipped and
                 * the exe's exit code is returned so the user sees
                 * the precise error.  */
                int o_idx = -1;
                for (int j = 0; j < argc - 1; j++) {
                    if (strcmp(argv[j], "-o") == 0) { o_idx = j + 1; break; }
                }
                char lib_out_buf[1024] = {0};
                char* lib_out_override = NULL;
                if (o_idx > 0) {
#ifdef __APPLE__
                    const char* lib_ext = ".dylib";
#elif defined(_WIN32)
                    const char* lib_ext = ".dll";
#else
                    const char* lib_ext = ".so";
#endif
                    snprintf(lib_out_buf, sizeof(lib_out_buf), "%s%s",
                             argv[o_idx], lib_ext);
                    lib_out_override = lib_out_buf;
                }
                char** dup_exe = (char**)malloc(sizeof(char*) * (size_t)argc);
                char** dup_lib = (char**)malloc(sizeof(char*) * (size_t)argc);
                if (!dup_exe || !dup_lib) {
                    fprintf(stderr, "Error: out of memory dispatching --emit=both\n");
                    free(dup_exe); free(dup_lib);
                    return 1;
                }
                for (int j = 0; j < argc; j++) {
                    if (j == i) {
                        dup_exe[j] = (char*)"--emit=exe";
                        dup_lib[j] = (char*)"--emit=lib";
                    } else if (j == o_idx && lib_out_override) {
                        dup_exe[j] = argv[j];
                        dup_lib[j] = lib_out_override;
                    } else {
                        dup_exe[j] = argv[j];
                        dup_lib[j] = argv[j];
                    }
                }
                int rc_exe = cmd_build(argc, dup_exe);
                int rc_lib = (rc_exe == 0) ? cmd_build(argc, dup_lib) : 0;
                free(dup_exe); free(dup_lib);
                if (rc_exe != 0) return rc_exe;
                return rc_lib;
            } else if (strcmp(val, "csrc") == 0) {
                /* #996: --emit=csrc — emit the portable generated C + a catalog
                 * header, and STOP (no gcc). Uses --emit=lib codegen (same
                 * aether_<name> catalog). g_emit_csrc makes the build path skip
                 * the compile/link step and derive the .c/.h output paths. */
                g_emit_exe = false;
                g_emit_lib = true;
                g_emit_csrc = true;
            } else {
                fprintf(stderr, "Error: --emit must be one of: exe, lib, csrc (got '%s')\n", val);
                return 1;
            }
        } else if (strcmp(argv[i], "--namespace") == 0 && i + 1 < argc) {
            // Handled in a dedicated function defined above.
            return cmd_build_namespace(argc, argv);
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }

    // aether.toml walk-up: if cwd has no toml but an ancestor does,
    // chdir there so [[bin]] / extra_sources / cflags resolution
    // works the same as if the user had run `ae build` from the
    // project root. Closes #280 (2).
    find_and_chdir_to_aether_toml(&file);

    // Read target from aether.toml if not specified on CLI
    if (!target && path_exists("aether.toml")) {
        static char toml_target[64];
        TomlDocument* doc = toml_parse_file("aether.toml");
        if (doc) {
            const char* val = toml_get_value(doc, "build", "target");
            if (val && strcmp(val, "native") != 0) {
                strncpy(toml_target, val, sizeof(toml_target) - 1);
                toml_target[sizeof(toml_target) - 1] = '\0';
                target = toml_target;
            }
            toml_free_document(doc);
        }
    }

    // Validate target. Beyond native/wasm, a cross triple routes the
    // build through the zig cc backend (#1105).
    const char* ztriple = cross_target_to_zig(target);
    if (target && strcmp(target, "wasm") != 0 && strcmp(target, "native") != 0 && !ztriple) {
        fprintf(stderr, "Error: Unknown target '%s'.\n", target);
        fprintf(stderr, "Valid targets: native, wasm, or a cross triple "
                        "(aarch64-macos, x86_64-macos, aarch64-linux, x86_64-linux, "
                        "aarch64-freebsd, x86_64-freebsd, x86_64-windows, "
                        "aarch64-windows).\n");
        return 1;
    }
    int is_wasm = target && strcmp(target, "wasm") == 0;
    int is_cross = ztriple != NULL;

    // Resolve directory argument (e.g. "." or "myproject/") to src/main.ae
    if (file && dir_exists(file)) {
        static char resolved_build_file[1040];  // file path + "/src/main.ae" + NUL
        snprintf(resolved_build_file, sizeof(resolved_build_file), "%s/src/main.ae", file);
        if (path_exists(resolved_build_file)) {
            file = resolved_build_file;
        } else {
            char toml_path[1040];  // file path + "/aether.toml" + NUL
            snprintf(toml_path, sizeof(toml_path), "%s/aether.toml", file);
            if (path_exists(toml_path))
                fprintf(stderr, "Error: No src/main.ae found in %s\n", file);
            else
                fprintf(stderr, "Error: '%s' is not an Aether project directory\n", file);
            return 1;
        }
    }

    // Project mode
    if (!file && path_exists("aether.toml")) {
        if (path_exists("src/main.ae"))
            file = "src/main.ae";
        else {
            fprintf(stderr, "Error: aether.toml found but src/main.ae is missing.\n");
            fprintf(stderr, "Create src/main.ae or specify a file: ae build <file.ae>\n");
            return 1;
        }
    }

    if (!file) {
        fprintf(stderr, "Error: No input file specified.\n");
        fprintf(stderr, "Usage: ae build <file.ae> [-o output] [--extra file.c] [--quick] [--target=<triple>]\n");
        fprintf(stderr, "  --quick    Compile with -O0 -g for faster iteration (default: -O2)\n");
        fprintf(stderr, "  --target   Cross-compile via zig cc: wasm, aarch64-macos, x86_64-macos,\n");
        fprintf(stderr, "             aarch64-linux, x86_64-linux, aarch64-freebsd, x86_64-freebsd,\n");
        fprintf(stderr, "             x86_64-windows, aarch64-windows (-> foo.exe; self-contained)\n");
        fprintf(stderr, "             (freebsd needs AETHER_SYSROOT=<base sysroot>; see aether-crossbuild)\n");
        return 1;
    }

    // [[bin]] name → path resolution. If the positional argument
    // doesn't exist as a file but matches the `name = "..."` of a
    // [[bin]] entry in aether.toml, treat it as that bin's path.
    // Cargo's rule: `cargo build --bin foo` requires the name; we
    // accept it as a positional for shorter typing. Closes #280 (1).
    static char bin_resolved_path[1024];
    if (!path_exists(file)) {
        if (find_bin_path_by_name(file, bin_resolved_path, sizeof(bin_resolved_path))) {
            file = bin_resolved_path;
        }
    }

    if (!path_exists(file)) {
        fprintf(stderr, "Error: File not found: %s\n", file);
        return 1;
    }

    const char* base = get_basename(file);
    char c_file[2048], exe_file[2048], cmd[16384];

    if (output_name) {
        // Explicit -o: use the path as-is
        snprintf(c_file, sizeof(c_file), "%s.c", output_name);
        snprintf(exe_file, sizeof(exe_file), "%s" EXE_EXT, output_name);
    } else if (path_exists("aether.toml")) {
        // Project mode: output to target/
        mkdirs("target");
        snprintf(c_file, sizeof(c_file), "target/%s.c", base);
        snprintf(exe_file, sizeof(exe_file), "target/%s" EXE_EXT, base);
    } else if (tc.dev_mode) {
        snprintf(c_file, sizeof(c_file), "%s/build/%s.c", tc.root, base);
        snprintf(exe_file, sizeof(exe_file), "%s/build/%s" EXE_EXT, tc.root, base);
    } else {
        snprintf(c_file, sizeof(c_file), "%s.c", base);
        snprintf(exe_file, sizeof(exe_file), "%s" EXE_EXT, base);
    }

    // Override output extension for wasm target
    if (is_wasm) {
        // Replace .exe or binary with .js (emcc produces .js + .wasm pair)
        char* dot = strrchr(exe_file, '.');
        if (dot && strcmp(dot, EXE_EXT) == 0) {
            strcpy(dot, ".js");
        } else {
            strncat(exe_file, ".js", sizeof(exe_file) - strlen(exe_file) - 1);
        }
    }

    // Windows cross target: ensure the output ends in .exe. On the Linux/macOS
    // cross-host EXE_EXT is empty, so `-o foo` would produce an extensionless
    // file for a Windows target — append .exe if it's not already there so the
    // artifact is named the way Windows (and the user) expects.
    if (ztriple && strstr(ztriple, "windows")) {
        size_t el = strlen(exe_file);
        if (el < 4 || strcasecmp(exe_file + el - 4, ".exe") != 0) {
            strncat(exe_file, ".exe", sizeof(exe_file) - el - 1);
        }
    }

    // Override output name for --emit=lib: swap <name> for lib<name>.so
    // (or .dylib on macOS). Only applies when the user didn't supply -o
    // with an explicit name; if they did, we honor their choice.
    if (g_emit_lib && !g_emit_exe && !is_wasm && !output_name) {
#ifdef __APPLE__
        const char* lib_ext = ".dylib";
#elif defined(_WIN32)
        const char* lib_ext = ".dll";
#else
        const char* lib_ext = ".so";
#endif
        // Find the basename portion in exe_file and insert "lib" prefix.
        // Strategy: walk back from the end to the last separator, copy the
        // prefix, append "lib", then the basename with its extension swapped.
        char buf[2048];
        const char* last_sep = exe_file;
        for (const char* p = exe_file; *p; p++) {
            if (*p == '/' || *p == '\\') last_sep = p + 1;
        }
        size_t prefix_len = (size_t)(last_sep - exe_file);
        if (prefix_len >= sizeof(buf)) prefix_len = sizeof(buf) - 1;
        memcpy(buf, exe_file, prefix_len);
        buf[prefix_len] = '\0';
        // Strip EXE_EXT (empty on POSIX) from the basename before adding lib_ext.
        char basename_noext[512];
        strncpy(basename_noext, last_sep, sizeof(basename_noext) - 1);
        basename_noext[sizeof(basename_noext) - 1] = '\0';
        if (EXE_EXT[0]) {
            size_t elen = strlen(EXE_EXT);
            size_t blen = strlen(basename_noext);
            if (blen >= elen && strcmp(basename_noext + blen - elen, EXE_EXT) == 0) {
                basename_noext[blen - elen] = '\0';
            }
        }
        // Stage through a wider scratch buffer so gcc -Wformat-truncation
        // sees enough room for the worst-case prefix + "lib" + basename +
        // lib_ext concatenation; we then copy back into exe_file's
        // existing 2048-byte slot.
        char composed[3072];
        snprintf(composed, sizeof(composed), "%slib%s%s", buf, basename_noext, lib_ext);
        strncpy(exe_file, composed, sizeof(exe_file) - 1);
        exe_file[sizeof(exe_file) - 1] = '\0';
    }

    // Pre-flight: verify emcc for wasm target before starting compilation
    if (is_wasm && run_cmd_quiet("emcc --version") != 0) {
        fprintf(stderr, "Error: Emscripten (emcc) not found on PATH.\n");
        fprintf(stderr, "Install: https://emscripten.org/docs/getting_started/downloads.html\n");
        fprintf(stderr, "  git clone https://github.com/emscripten-core/emsdk.git\n");
        fprintf(stderr, "  cd emsdk && ./emsdk install latest && ./emsdk activate latest\n");
        fprintf(stderr, "  source ./emsdk_env.sh\n");
        return 1;
    }

    // Pre-flight for cross builds: zig provides the backend compiler +
    // target libc/linker, and the program must be dependency-free (PR 1).
    if (is_cross) {
        // Cross builds produce executables only for now; --emit=lib /
        // --emit=both would emit library-shaped C (no main) that the
        // executable link rejects. Reject up front, like unknown targets.
        if (g_emit_lib) {
            fprintf(stderr,
                "Error: cross-compilation (--target=%s) supports executables only; "
                "--emit=lib and --emit=both are not supported yet.\n", target);
            return 1;
        }
        if (run_cmd_quiet("zig version") != 0) {
            fprintf(stderr, "Error: zig not found on PATH (required to cross-compile for %s).\n",
                    target);
            fprintf(stderr, "Install zig 0.11+: https://ziglang.org/download/  (macOS: brew install zig)\n");
            return 1;
        }
        char mod[64];
        if (cross_uses_unsupported_module(file, mod, sizeof(mod))) {
            fprintf(stderr,
                "Note: '%s' uses %s. Cross binaries are built without OpenSSL / zlib /\n"
                "nghttp2 / PCRE2, so features that need them (HTTPS/TLS, hashing, base64,\n"
                "regex, compression, HTTP/2) report errors at runtime on %s, exactly like\n"
                "a native build on a host without those libraries. Plain sockets and pure\n"
                "helpers still work. Building anyway.\n",
                file, mod, target);
        }
    }

    // Merge toml [[bin]] extra_sources into extra_files BEFORE the cache
    // check so an FFI shim edit invalidates the cached exe (extras
    // content is part of the cache key).
    {
        char toml_extra_pre[8192] = "";
        if (get_extra_sources_for_bin(file, toml_extra_pre, sizeof(toml_extra_pre))) {
            fprintf(stderr,
                "Warning: aether.toml [[bin]] extra_sources for '%s' "
                "exceeded 8 KiB; tail entries were dropped. Split the "
                "array into fewer, larger shims or report as a toolchain "
                "bug.\n", file);
        }
        if (toml_extra_pre[0]) {
            if (extra_files[0]) strncat(extra_files, " ", sizeof(extra_files) - strlen(extra_files) - 1);
            strncat(extra_files, toml_extra_pre, sizeof(extra_files) - strlen(extra_files) - 1);
        }
    }

    // --- Build cache ---
    // Cache native --emit=exe builds only. wasm uses a different toolchain
    // (emcc emits .js + .wasm) and --emit=lib produces a different artefact
    // type; both deserve their own cache shape later. --namespace mode
    // produces SDKs in subdirectories, also out of scope.
    bool cache_eligible = !is_wasm && !is_cross && g_emit_exe && !g_emit_lib;
    char cached_exe[1024] = "";
    unsigned long long cache_key = 0;
    if (cache_eligible) {
        cache_key = compute_cache_key(file, extra_files,
                                      quick ? "O0" : "O2",
                                      "build");
        if (cache_key != 0) {
            init_cache_dir();
            snprintf(cached_exe, sizeof(cached_exe), "%s/%016llx" EXE_EXT,
                     s_cache_dir, cache_key);
            if (path_exists(cached_exe)) {
                if (tc.verbose) fprintf(stderr, "[cache] hit: %016llx\n", cache_key);
                if (copy_file(cached_exe, exe_file)) {
                    printf("Built (cache hit): %s\n", exe_file);
                    return 0;
                }
                if (tc.verbose) fprintf(stderr, "[cache] copy failed; falling through to rebuild\n");
            } else if (tc.verbose) {
                fprintf(stderr, "[cache] miss: %016llx\n", cache_key);
            }
        }
    }

    if (is_cross)      printf("Building %s (cross: %s)...\n", file, target);
    else               printf("Building %s%s...\n", file, is_wasm ? " (wasm)" : "");

    // Binary-import prepass: synthesize interface stubs for any
    // `import foo` resolving to a precompiled libfoo.so, link it in.
    prepare_binary_imports(file);

    // Host-bridge prepass: queue libaether_host_<lang>.a for any
    // `import contrib.host.<lang>` in the entry file. See cmd_run.
    prepare_host_bridge_imports(file);

    // Step 1: .ae to .c
    build_aetherc_cmd(cmd, sizeof(cmd), file, c_file);

    // Always run visible on failure; print diagnostic on Windows
    int aetherc_ret = tc.verbose ? run_cmd(cmd) : run_cmd_quiet(cmd);
    if (aetherc_ret != 0) {
        fprintf(stderr, "[diag] aetherc returned %d for: %s\n", aetherc_ret, file);
        fprintf(stderr, "[diag] cmd: %s\n", cmd);
        // Retry visible
        build_aetherc_cmd(cmd, sizeof(cmd), file, c_file);
        int retry_ret = run_cmd(cmd);
        fprintf(stderr, "[diag] retry returned %d\n", retry_ret);
        fprintf(stderr, "Compilation failed.\n");
        return 1;
    }

    /* #996 --emit=csrc: aetherc has written the portable `.c`, the catalog `.h`
     * and the machine-readable `.catalog.json` (via --emit-catalog-header /
     * --emit-catalog-json, appended by build_aetherc_cmd). No gcc: the artifact
     * IS the source. Keep the .c (don't remove it), report the paths, and stop. */
    if (g_emit_csrc) {
        char h_file[2048], j_file[2048];
        snprintf(h_file, sizeof(h_file), "%s", c_file);
        size_t hl = strlen(h_file);
        if (hl > 2 && h_file[hl-2] == '.' && h_file[hl-1] == 'c') h_file[hl-1] = 'h';
        snprintf(j_file, sizeof(j_file), "%s", c_file);
        size_t jl = strlen(j_file);
        if (jl > 2 && j_file[jl-2] == '.' && j_file[jl-1] == 'c') j_file[jl-2] = '\0';
        size_t jb = strlen(j_file);
        snprintf(j_file + jb, sizeof(j_file) - jb, ".catalog.json");
        printf("Emitted C source: %s\n", c_file);
        printf("Emitted header:   %s\n", h_file);
        printf("Emitted catalog:  %s\n", j_file);
        printf("Compile it against the runtime with `ae cflags` (or feed to WASM / static-link).\n");
        return 0;
    }

    // Step 2: .c to executable (or wasm) with runtime.
    // toml [[bin]] extra_sources were already merged into extra_files
    // above (before the cache check), so no further reading is needed.
    // Cross builds run a multi-step compile/archive/link sequence that
    // surfaces its own errors, so they bypass the shared command run.
    int build_ret;
    if (is_cross) {
        const char* extra = extra_files[0] ? extra_files : NULL;
        build_ret = run_cross_build(c_file, exe_file, !quick, extra, ztriple);
        if (build_ret != 0) {
            fprintf(stderr, "Build failed.\n");
            return 1;
        }
    } else {
        if (is_wasm) {
            if (!build_wasm_cmd(cmd, sizeof(cmd), c_file, exe_file)) {
                return 1;
            }
        } else {
            const char* extra = extra_files[0] ? extra_files : NULL;
            build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, !quick, extra);
        }
        /* Warnings-visible like the `ae run` path: with #1252 fixed the C
         * compiler's -Wformat findings map to the user's .ae lines, and a
         * fully quiet compile would hide them. Errors still re-run loud. */
        build_ret = tc.verbose ? run_cmd(cmd) : run_cmd_show_warnings(cmd);
        if (build_ret != 0) {
            // Retry with visible output for error messages
            if (is_wasm) {
                build_wasm_cmd(cmd, sizeof(cmd), c_file, exe_file);
            } else {
                const char* extra = extra_files[0] ? extra_files : NULL;
                build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, !quick, extra);
            }
            run_cmd(cmd);
            fprintf(stderr, "Build failed.\n");
            return 1;
        }
    }

    // Clean up intermediate C file — ae build produces a binary, not C source
    remove(c_file);

#ifdef __APPLE__
    /* macOS clang bakes the `-o` value into the dylib's install_name at
     * link time. A dylib built via `--emit=lib -o libfoo` ends up with
     * install_name `libfoo` (no extension, no directory), which dyld
     * cannot resolve when a statically-linked consumer tries to load it.
     * Rewrite the id to @rpath/<basename> so consumers that pass
     * -Wl,-rpath,<dir> at link time can find the lib.
     *
     * cmd_build_namespace does its own install_name fixup after its
     * post-rename step — this block is for direct `ae build --emit=lib`. */
    if (g_emit_lib && !g_emit_exe) {
        const char* base = strrchr(exe_file, '/');
        base = base ? base + 1 : exe_file;
        char id_cmd[4096];
        snprintf(id_cmd, sizeof(id_cmd),
                 "install_name_tool -id '@rpath/%s' '%s' 2>/dev/null",
                 base, exe_file);
        if (system(id_cmd) != 0) {
            fprintf(stderr, "Warning: install_name_tool failed on %s; "
                            "consumers may fail to dlopen.\n", exe_file);
        }
    }
#endif

    // Populate the build cache so the next identical-input build is a
    // copy-from-cache instead of an aetherc + gcc round-trip. Copy to a
    // private temp beside the slot, publish by atomic rename (#1032) —
    /* Ad-hoc re-sign + quarantine-clear BEFORE the cache copy, so both
     * the fresh exe and its cached clone skip the one-time syspolicyd
     * evaluation stall on first run. */
    macos_prepare_binary(exe_file);

    // a concurrent `ae run` cache hit must never exec a half-copied exe.
    if (cache_eligible && cache_key != 0 && cached_exe[0]) {
        char cache_tmp[1100];
        snprintf(cache_tmp, sizeof(cache_tmp), "%s.tmp.%d", cached_exe, (int)getpid());
        if (!copy_file(exe_file, cache_tmp)) {
            if (tc.verbose) fprintf(stderr, "[cache] write failed for %016llx\n", cache_key);
        } else if (cache_publish(cache_tmp, cached_exe) != 0) {
            remove(cache_tmp);
            if (tc.verbose) fprintf(stderr, "[cache] publish failed for %016llx\n", cache_key);
        } else if (tc.verbose) {
            fprintf(stderr, "[cache] wrote: %016llx\n", cache_key);
        }
    }

    printf("Built: %s\n", exe_file);
    if (is_cross) {
        printf("       target %s: copy to a matching host to run.\n", target);
    }
    if (is_wasm) {
        // .wasm file is co-located with .js
        char wasm_file[2048];
        strncpy(wasm_file, exe_file, sizeof(wasm_file) - 1);
        char* js_ext = strrchr(wasm_file, '.');
        if (js_ext) strcpy(js_ext, ".wasm");
        printf("       %s\n", wasm_file);
        printf("Run with: node %s\n", exe_file);
    }
    return 0;
}

static int cmd_init(int argc, char** argv) {
    if (argc < 1 || argv[0][0] == '-') {
        fprintf(stderr, "Usage: ae init <name>\n");
        return 1;
    }

    const char* name = argv[0];

    if (dir_exists(name)) {
        fprintf(stderr, "Error: Directory '%s' already exists.\n", name);
        return 1;
    }

    printf("Creating new Aether project '%s'...\n\n", name);
    mkdirs(name);

    char path[1024];
    FILE* f;

    // aether.toml
    snprintf(path, sizeof(path), "%s/aether.toml", name);
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Error: Could not create %s\n", path); return 1; }
    fprintf(f, "[package]\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "version = \"0.1.0\"\n");
    fprintf(f, "description = \"A new Aether project\"\n");
    fprintf(f, "license = \"MIT\"\n\n");
    fprintf(f, "[[bin]]\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "path = \"src/main.ae\"\n\n");
    fprintf(f, "[dependencies]\n\n");
    fprintf(f, "[build]\n");
    fprintf(f, "target = \"native\"\n");
    fprintf(f, "# link_flags = \"-lsqlite3 -lcurl\"  # Add extra linker flags\n");
    fclose(f);

    // src/main.ae
    snprintf(path, sizeof(path), "%s/src", name);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/src/main.ae", name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "main() {\n");
        fprintf(f, "    print(\"Hello from %s!\\n\");\n", name);
        fprintf(f, "}\n");
        fclose(f);
    }

    // tests/
    snprintf(path, sizeof(path), "%s/tests", name);
    mkdirs(path);

    // README.md
    snprintf(path, sizeof(path), "%s/README.md", name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "# %s\n\nAn Aether project.\n\n", name);
        fprintf(f, "## Quick Start\n\n```bash\nae run\n```\n\n");
        fprintf(f, "## Build\n\n```bash\nae build\n```\n\n");
        fprintf(f, "## Test\n\n```bash\nae test\n```\n");
        fclose(f);
    }

    // .gitignore
    snprintf(path, sizeof(path), "%s/.gitignore", name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "target/\nbuild/\n*.o\naether.lock\n");
        fclose(f);
    }

    printf("  Created %s/aether.toml\n", name);
    printf("  Created %s/src/main.ae\n", name);
    printf("  Created %s/tests/\n", name);
    printf("  Created %s/README.md\n", name);
    printf("  Created %s/.gitignore\n\n", name);
    printf("Get started:\n");
    printf("  cd %s\n", name);
    printf("  ae run\n");

    return 0;
}

static int cmd_test(int argc, char** argv) {
    const char* target = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-') {
            if (!is_safe_path(argv[i])) {
                fprintf(stderr, "Error: Invalid characters in path\n");
                return 1;
            }
            target = argv[i];
            break;
        }
    }

    // Collect test files
    char test_files[256][512];
    int test_count = 0;

    if (target && path_exists(target) && !dir_exists(target)) {
        // Single file
        strncpy(test_files[0], target, sizeof(test_files[0]) - 1);
        test_files[0][sizeof(test_files[0]) - 1] = '\0';
        test_count = 1;
    } else {
        // Discover from directory
        const char* test_dir = "tests";
        if (target && dir_exists(target)) {
            static char resolved_test_dir[512];
            snprintf(resolved_test_dir, sizeof(resolved_test_dir), "%s/tests", target);
            test_dir = dir_exists(resolved_test_dir) ? resolved_test_dir : target;
        }

        if (!dir_exists(test_dir)) {
            printf("No tests/ directory found.\n");
            printf("Create tests in tests/ or run: ae test <file.ae>\n");
            return 0;
        }

        char find_cmd[1024];
#ifdef _WIN32
        snprintf(find_cmd, sizeof(find_cmd),
            "dir /b /s \"%s\\*.ae\" 2>nul", test_dir);
#else
        snprintf(find_cmd, sizeof(find_cmd),
            "find \"%s\" \\( -name 'test_*.ae' -o -name '*_test.ae' \\) -type f 2>/dev/null | sort",
            test_dir);
#endif
        FILE* pipe = popen(find_cmd, "r");
        if (pipe) {
            char line[512];
            while (fgets(line, sizeof(line), pipe) && test_count < 256) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strlen(line) == 0) continue;
                // Convention: only files named test_*.ae or *_test.ae are tests
                // (like pytest's test_*.py or Go's *_test.go)
                const char* base = strrchr(line, '/');
                if (!base) base = strrchr(line, '\\');
                base = base ? base + 1 : line;
                if (strncmp(base, "test_", 5) != 0) {
                    // Check *_test.ae pattern
                    const char* ext = strstr(base, "_test.ae");
                    if (!ext || strcmp(ext, "_test.ae") != 0) continue;
                }
                strncpy(test_files[test_count], line, sizeof(test_files[0]) - 1);
                test_files[test_count][sizeof(test_files[0]) - 1] = '\0';
                test_count++;
            }
            pclose(pipe);
        }
    }

    if (test_count == 0) {
        printf("No test files found.\n");
        return 0;
    }

    printf("Running %d test(s)...\n\n", test_count);

    int passed = 0, failed = 0;

    for (int i = 0; i < test_count; i++) {
        const char* test = test_files[i];
        printf("  %-45s ", test);
        fflush(stdout);

        char c_file[2048], exe_file[2048], cmd[16384];

        if (tc.dev_mode) {
            snprintf(c_file, sizeof(c_file), "%s/build/_test_%d.c", tc.root, i);
            snprintf(exe_file, sizeof(exe_file), "%s/build/_test_%d" EXE_EXT, tc.root, i);
        } else {
            snprintf(c_file, sizeof(c_file), "%s/_ae_test_%d.c", get_temp_dir(), i);
            snprintf(exe_file, sizeof(exe_file), "%s/_ae_test_%d" EXE_EXT, get_temp_dir(), i);
        }

        // Compile .ae to .c
        // GCC conservatively assumes argv paths may be PATH_MAX-sized; cmd[8192]
        // is sufficient for real-world paths (compiler + test + c_file < 8KB).
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
        build_aetherc_cmd(cmd, sizeof(cmd), test, c_file);
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
        if (run_cmd_quiet(cmd) != 0) {
            printf("FAIL (compile)\n");
            failed++;
            continue;
        }

        // Compile .c to executable
        build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, false, NULL);
        if (run_cmd_quiet(cmd) != 0) {
            printf("FAIL (build)\n");
            failed++;
            remove(c_file);
            continue;
        }

        // Run
        snprintf(cmd, sizeof(cmd), "\"%s\"", exe_file);
        int rc = run_cmd_quiet(cmd);
        if (rc == 0) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL (exit %d)\n", rc);
            failed++;
        }

        remove(c_file);
        remove(exe_file);
    }

    printf("\n%d passed, %d failed, %d total\n", passed, failed, test_count);
    return (failed > 0) ? 1 : 0;
}

static int cmd_add(int argc, char** argv) {
    if (argc < 1 || argv[0][0] == '-') {
        fprintf(stderr, "Usage: ae add <host>/<user>/<repo>[@version]\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  ae add github.com/user/repo\n");
        fprintf(stderr, "  ae add github.com/user/repo@v1.2.0\n");
        fprintf(stderr, "  ae add gitlab.com/user/repo\n");
        return 1;
    }

    // Parse package@version
    char pkg_buf[1024];
    strncpy(pkg_buf, argv[0], sizeof(pkg_buf) - 1);
    pkg_buf[sizeof(pkg_buf) - 1] = '\0';

    const char* version = NULL;
    char* at = strchr(pkg_buf, '@');
    if (at) {
        *at = '\0';
        version = at + 1;
    }
    const char* package = pkg_buf;

    if (!path_exists("aether.toml")) {
        fprintf(stderr, "Error: No aether.toml found. Run 'ae init <name>' first.\n");
        return 1;
    }

    // Validate: must look like a git-hostable URL (host.tld/user/repo)
    // Supports GitHub, GitLab, Bitbucket, Codeberg, self-hosted, etc.
    if (!strchr(package, '/') || !strchr(package, '.')) {
        fprintf(stderr, "Error: Package must be a git-hostable path.\n");
        fprintf(stderr, "Format: ae add <host>/<user>/<repo>[@version]\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  ae add github.com/user/repo\n");
        fprintf(stderr, "  ae add gitlab.com/user/repo@v1.0.0\n");
        fprintf(stderr, "  ae add codeberg.org/user/repo\n");
        return 1;
    }

    // Validate package name to prevent command injection
    if (!is_safe_shell_arg(package)) {
        fprintf(stderr, "Error: Package name contains invalid characters.\n");
        return 1;
    }

    printf("Adding %s%s%s...\n", package, version ? "@" : "", version ? version : "");

    // Cache directory — sized generously so GCC's -Wformat-truncation
    // doesn't complain (real paths are ~60 bytes, never close to limits)
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.aether/packages", get_home_dir());

    char pkg_dir[1024];
    snprintf(pkg_dir, sizeof(pkg_dir), "%.511s/%.511s", cache_dir, package);

    if (!dir_exists(pkg_dir)) {
        printf("Downloading...\n");
        char parent[1024];
        strncpy(parent, pkg_dir, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char* slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; mkdirs(parent); }

        char cmd[4096];
        if (version) {
            snprintf(cmd, sizeof(cmd), "git clone https://%s %s", package, pkg_dir);
        } else {
            snprintf(cmd, sizeof(cmd), "git clone --depth 1 https://%s %s", package, pkg_dir);
        }
        if (run_cmd(cmd) != 0) {
            fprintf(stderr, "Failed to download package.\n");
            fprintf(stderr, "Check that the repository exists: https://%s\n", package);
            return 1;
        }

        // Checkout specific version tag if requested
        if (version) {
            char tag[128];
            if (version[0] == 'v') {
                snprintf(tag, sizeof(tag), "%s", version);
            } else {
                snprintf(tag, sizeof(tag), "v%s", version);
            }
            snprintf(cmd, sizeof(cmd), "cd \"%s\" && git checkout %s 2>/dev/null || git checkout v%s 2>/dev/null",
                     pkg_dir, tag, version);
            if (run_cmd_quiet(cmd) != 0) {
                fprintf(stderr, "Error: Version '%s' not found.\n", version);
                // List available tags
                snprintf(cmd, sizeof(cmd), "cd \"%s\" && git tag -l 'v*' | sort -V | tail -10", pkg_dir);
                fprintf(stderr, "Available versions:\n");
                (void)run_cmd(cmd);
                return 1;
            }
            printf("Checked out %s\n", tag);
        }
    }

    // Add to aether.toml
    FILE* f = fopen("aether.toml", "r");
    if (!f) {
        fprintf(stderr, "Error: Could not read aether.toml\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        fprintf(stderr, "Error: Could not determine file size\n");
        return 1;
    }
    fseek(f, 0, SEEK_SET);
    char* content = malloc((size_t)sz + 1);
    if (!content) {
        fclose(f);
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }
    size_t nread = fread(content, 1, (size_t)sz, f);
    content[nread] = '\0';
    fclose(f);

    if (strstr(content, package)) {
        printf("Already in dependencies.\n");
        free(content);
        return 0;
    }

    char* deps = strstr(content, "[dependencies]");
    if (deps) {
        char* next_sect = strchr(deps + 14, '[');
        f = fopen("aether.toml", "w");
        if (!f) {
            fprintf(stderr, "Error: Could not write aether.toml\n");
            free(content);
            return 1;
        }
        if (next_sect) {
            fwrite(content, 1, next_sect - content, f);
            fprintf(f, "%s = \"%s\"\n", package, version ? version : "latest");
            fputs(next_sect, f);
        } else {
            fputs(content, f);
            fprintf(f, "%s = \"%s\"\n", package, version ? version : "latest");
        }
        fclose(f);
    } else {
        // No [dependencies] section — append one
        f = fopen("aether.toml", "a");
        if (!f) {
            fprintf(stderr, "Error: Could not write aether.toml\n");
            free(content);
            return 1;
        }
        fprintf(f, "\n[dependencies]\n");
        fprintf(f, "%s = \"%s\"\n", package, version ? version : "latest");
        fclose(f);
    }

    free(content);
    printf("Added %s to dependencies.\n", package);
    return 0;
}

static int cmd_examples(int argc, char** argv) {
    const char* examples_dir = "examples";
    if (argc > 0 && argv[0][0] != '-') {
        if (!is_safe_path(argv[0])) {
            fprintf(stderr, "Error: Invalid characters in path\n");
            return 1;
        }
        examples_dir = argv[0];
    }

    char files[512][512];
    int file_count = 0;

    char find_cmd[1024];
#ifdef _WIN32
    snprintf(find_cmd, sizeof(find_cmd), "dir /b /s \"%s\\*.ae\" 2>nul", examples_dir);
#else
    snprintf(find_cmd, sizeof(find_cmd), "find \"%s\" -name '*.ae' -type f 2>/dev/null | sort", examples_dir);
#endif
    FILE* pipe = popen(find_cmd, "r");
    if (pipe) {
        char line[512];
        while (fgets(line, sizeof(line), pipe) && file_count < 512) {
            line[strcspn(line, "\n\r")] = '\0';
            if (strlen(line) > 0) {
                strncpy(files[file_count], line, sizeof(files[0]) - 1);
                files[file_count][sizeof(files[0]) - 1] = '\0';
                file_count++;
            }
        }
        pclose(pipe);
    }

    if (file_count == 0) {
        printf("No .ae files found in %s/\n", examples_dir);
        return 0;
    }

    printf("Building %d example(s)...\n\n", file_count);

    mkdirs("build/examples");

    int pass = 0, fail = 0, skipped = 0;

    for (int i = 0; i < file_count; i++) {
        const char* src = files[i];

        // Skip module files (lib/) and project mains (packages/) —
        // these need `ae run` with module orchestration, not bare aetherc.
        if (strstr(src, "/lib/") || strstr(src, "\\lib\\") ||
            strstr(src, "/packages/") || strstr(src, "\\packages\\")) {
            skipped++;
            continue;
        }

        const char* slash = strrchr(src, '/');
        if (!slash) slash = strrchr(src, '\\');
        const char* name = slash ? slash + 1 : src;
        char base[256];
        strncpy(base, name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char* dot = strrchr(base, '.');
        if (dot) *dot = '\0';

        printf("  %-30s ", base);
        fflush(stdout);

        char c_file[2048], exe_file[2048], cmd[16384];
        snprintf(c_file, sizeof(c_file), "build/examples/%s.c", base);
        snprintf(exe_file, sizeof(exe_file), "build/examples/%s" EXE_EXT, base);

        // Find extra .c files in the same directory as the .ae source
        char src_dir[512];
        strncpy(src_dir, src, sizeof(src_dir) - 1);
        src_dir[sizeof(src_dir) - 1] = '\0';
        char* last_sep = strrchr(src_dir, '/');
        if (!last_sep) last_sep = strrchr(src_dir, '\\');
        if (last_sep) *last_sep = '\0';
        else strcpy(src_dir, ".");

        char extra_c[2048] = "";
        char find_c[1024];
#ifdef _WIN32
        snprintf(find_c, sizeof(find_c), "dir /b \"%s\\*.c\" 2>nul", src_dir);
#else
        snprintf(find_c, sizeof(find_c), "find \"%s\" -maxdepth 1 -name '*.c' 2>/dev/null", src_dir);
#endif
        FILE* c_pipe = popen(find_c, "r");
        if (c_pipe) {
            char c_line[512];
            while (fgets(c_line, sizeof(c_line), c_pipe)) {
                c_line[strcspn(c_line, "\n\r")] = '\0';
                if (strlen(c_line) == 0) continue;
                char c_path[1100];
#ifdef _WIN32
                snprintf(c_path, sizeof(c_path), "%s\\%s", src_dir, c_line);
#else
                snprintf(c_path, sizeof(c_path), "%s", c_line);
#endif
                if (strlen(extra_c) + strlen(c_path) + 2 < sizeof(extra_c)) {
                    strcat(extra_c, " ");
                    strcat(extra_c, c_path);
                }
            }
            pclose(c_pipe);
        }

        // Step 1: compile .ae -> .c
        // GCC conservatively assumes src (char* from glob) may be PATH_MAX-sized;
        // cmd[8192] is sufficient for real-world paths.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
        build_aetherc_cmd(cmd, sizeof(cmd), src, c_file);
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
        if (run_cmd_quiet(cmd) != 0) {
            printf("FAIL (compile)\n");
            fail++;
            continue;
        }

        // Step 2: link .c + extra -> exe
        const char* extra = extra_c[0] ? extra_c : NULL;
        build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, true, extra);
        if (run_cmd_quiet(cmd) != 0) {
            printf("FAIL (build)\n");
            fail++;
            remove(c_file);
            continue;
        }

        printf("OK\n");
        pass++;
        remove(c_file);
    }

    printf("\n%d passed, %d failed, %d total\n", pass, fail, file_count - skipped);
    printf("Binaries in build/examples/\n");
    return (fail > 0) ? 1 : 0;
}

// REPL session: accumulated lines that persist across evaluations.
// Each entry is a statement (assignment, function def, etc.) that gets
// replayed before the current input so variables/functions stay in scope.

// --------------------------------------------------------------------------
// Cache management command
// --------------------------------------------------------------------------

static int cmd_cache(int argc, char** argv) {
    const char* sub = argc > 0 ? argv[0] : "info";

    const char* home = get_home_dir();
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/.aether/cache", home);

    if (strcmp(sub, "clear") == 0) {
#ifdef _WIN32
        char pattern[600];
        snprintf(pattern, sizeof(pattern), "%s\\*", cache_path);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            printf("Cache is empty (no cache directory).\n");
            return 0;
        }
        int count = 0;
        do {
            if (fd.cFileName[0] == '.') continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s\\%s", cache_path, fd.cFileName);
            remove(full);
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR* d = opendir(cache_path);
        if (!d) {
            printf("Cache is empty (no cache directory).\n");
            return 0;
        }
        int count = 0;
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", cache_path, entry->d_name);
            remove(full);
            count++;
        }
        closedir(d);
#endif
        printf("Cleared %d cached build(s) from %s\n", count, cache_path);
        return 0;
    }

    // Default: show cache info
#ifdef _WIN32
    {
        char pattern[600];
        snprintf(pattern, sizeof(pattern), "%s\\*", cache_path);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            printf("Cache: empty\nLocation: %s\n", cache_path);
            return 0;
        }
        int count = 0;
        long long total_bytes = 0;
        do {
            if (fd.cFileName[0] == '.') continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s\\%s", cache_path, fd.cFileName);
            struct stat st;
            if (stat(full, &st) == 0) { total_bytes += st.st_size; count++; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        printf("Cache: %d build(s), %.1f MB\nLocation: %s\n",
               count, (double)total_bytes / (1024.0 * 1024.0), cache_path);
    }
#else
    {
        DIR* d = opendir(cache_path);
        if (!d) {
            printf("Cache: empty\nLocation: %s\n", cache_path);
            return 0;
        }
        int count = 0;
        long long total_bytes = 0;
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", cache_path, entry->d_name);
            struct stat st;
            if (stat(full, &st) == 0) { total_bytes += st.st_size; count++; }
        }
        closedir(d);
        printf("Cache: %d build(s), %.1f MB\nLocation: %s\n",
               count, (double)total_bytes / (1024.0 * 1024.0), cache_path);
    }
#endif
    printf("Use 'ae cache clear' to free space.\n");
    return 0;
}

// --------------------------------------------------------------------------
// `ae cflags` — pkg-config-style include + link flags for external tools.
// Issue #329 follow-on item 1. External tooling can `$(ae cflags)` instead
// of carrying its own copy of the include-path / lib-path layout (which
// the install always knows better than any caller does).
// --------------------------------------------------------------------------

static int cmd_cflags(int argc, char** argv) {
    bool want_cflags = true;
    bool want_libs   = true;

    // Optional refinement: callers that only need one half (compile- or
    // link-side) can pass --cflags / --libs to subset the output. With
    // no arguments, both are emitted on one line — pkg-config behaviour.
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--cflags") == 0) {
            want_libs = false;
        } else if (strcmp(argv[i], "--libs") == 0) {
            want_cflags = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ae cflags [--cflags|--libs]\n");
            printf("  Print -I and link flags so external builds can:\n");
            printf("      gcc your.c $(ae cflags) -o your\n");
            printf("  Without arguments, prints both. Pass --cflags or --libs to subset.\n");
            return 0;
        } else {
            fprintf(stderr, "ae cflags: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    int wrote_anything = 0;

    if (want_cflags && tc.include_flags[0]) {
        fputs(tc.include_flags, stdout);
        wrote_anything = 1;
    }

    if (want_libs) {
        // Library: always link against -laether plus the platform's
        // pthread / math libs that every Aether program needs. The
        // explicit `-L<dir>` keeps `-laether` resolvable even when the
        // install path isn't on the linker's default search list.
        if (tc.has_lib && tc.lib[0]) {
            char libdir[1024];
            strncpy(libdir, tc.lib, sizeof(libdir) - 1);
            libdir[sizeof(libdir) - 1] = '\0';
            // Strip /libaether.a from the end → libdir.
            char* slash = strrchr(libdir, '/');
            if (!slash) slash = strrchr(libdir, '\\');
            if (slash) *slash = '\0';
            if (wrote_anything) fputc(' ', stdout);
            printf("-L%s -laether", libdir);
            wrote_anything = 1;
        }
        if (wrote_anything) fputc(' ', stdout);
        fputs("-pthread -lm", stdout);
        wrote_anything = 1;

        // Transitive deps. libaether.a was compiled with whatever optional
        // libraries pkg-config detected at Aether-build time (PCRE2 for
        // std.regex; OpenSSL for std.cryptography and std.http TLS; zlib
        // for std.zlib and HTTP gzip; nghttp2 for h2). Downstream binaries
        // that use those modules need the SAME libs on their link line, or
        // they get `undefined reference to pcre2_*` and similar at link
        // time. `ae build` (cmd_build, ae.c:~1877) already appends these
        // strings; `ae cflags` did not — the docs promise "use $(ae cflags)
        // in your gcc line" but that promise was broken for any binary
        // touching the four optional modules above. Now matched.
        //
        // Empty-string guard: pkg-config-failed builds set the macro to
        // ""; we skip the empty case so downstream doesn't see stray
        // whitespace.
#ifdef AETHER_OPENSSL_LIBS
        if (AETHER_OPENSSL_LIBS[0]) { fputc(' ', stdout); fputs(AETHER_OPENSSL_LIBS, stdout); }
#endif
#ifdef AETHER_ZLIB_LIBS
        if (AETHER_ZLIB_LIBS[0])    { fputc(' ', stdout); fputs(AETHER_ZLIB_LIBS, stdout); }
#endif
#ifdef AETHER_NGHTTP2_LIBS
        if (AETHER_NGHTTP2_LIBS[0]) { fputc(' ', stdout); fputs(AETHER_NGHTTP2_LIBS, stdout); }
#endif
#ifdef AETHER_PCRE2_LIBS
        if (AETHER_PCRE2_LIBS[0])   { fputc(' ', stdout); fputs(AETHER_PCRE2_LIBS, stdout); }
#endif
    }

    if (wrote_anything) fputc('\n', stdout);
    return 0;
}

// --------------------------------------------------------------------------
// Help and main
// --------------------------------------------------------------------------

static void print_usage(void) {
    printf("Aether %s - Actor-based systems programming language\n\n", AE_VERSION);
    printf("Usage:\n");
    printf("  ae <command> [arguments]\n\n");
    printf("Commands:\n");
    printf("  init <name>          Create a new Aether project\n");
    printf("  run [file.ae]        Compile and run a program\n");
    printf("  build [file.ae]      Compile to executable\n");
    printf("  build --target wasm  Compile to WebAssembly (.js + .wasm)\n");
    printf("  check [file.ae]      Type-check without compiling\n");
    printf("  fmt [--check] [path] Format source (stdin->stdout, or files/dirs in place)\n");
    printf("  bindgen consts <h>   Import C macro constants from a header as Aether consts\n");
    printf("  inspect [file.ae]    Show what a script declares (imports, capabilities, exports, decls)\n");
    printf("  test [file|dir]      Discover and run tests\n");
    printf("  add <package>        Add a dependency\n");
    printf("  cache [clear]        Show or clear build cache\n");
    printf("  cflags               Print -I/-L/-laether for embedding in external builds\n");
    printf("  lib-path             Print the resolved module-search chain\n");
    printf("  examples             List and run example programs\n");
    printf("  repl                 Start interactive REPL\n");
    printf("  install [<v>]        Install a release (latest if omitted)\n");
    printf("  upgrade              Install the latest release and switch to it\n");
    printf("  use <v>              Switch to an installed version\n");
    printf("  version              Show version / list installed versions\n");
    printf("  version list         List all available releases\n");
    printf("  help                 Show this help\n");
    printf("\nExamples:\n");
    printf("  ae init myproject          Create a new project\n");
    printf("  ae run hello.ae            Run a single file\n");
    printf("  ae run                     Run project (uses aether.toml)\n");
    printf("  ae build app.ae -o myapp   Build an executable\n");
    printf("  ae test                    Run all tests in tests/\n");
    printf("  ae add github.com/u/pkg    Add a dependency\n");
    printf("\nOptions:\n");
    printf("  -v, --verbose        Show detailed output\n");
    printf("  --lib <dir>[%c<dir>...]  Module search path (PATH-style, left-to-right;\n",
           AETHER_LIB_PATH_SEP_CHAR);
    printf("                       repeated flag also accepted: --lib a --lib b)\n");
    printf("\nEnvironment:\n");
    printf("  AETHER_HOME          Aether installation directory\n");
    printf("  AETHER_LIB_DIR       Same shape as --lib; PATH-style list of module search dirs\n");
}

// `ae lib-info <path>` — dump the symbol catalog embedded in a
// `--emit=lib` artifact (issue #403). Opens the .so/.dylib/.dll via
// dlopen, dlsym for the canonical `aether_lib_meta` entry point,
// walks the returned struct, and prints in human-readable form.
//
// The schema is layout-compatible with runtime/aether_lib_meta.h's
// AetherLibMeta — the `_AeLibInfo*` mirror structs are declared near
// the top of this file (shared with the binary-import prepass).
// Updates to the schema must touch BOTH that declaration and the
// canonical header in lock-step.

/* `ae lib-path` — introspect the resolved module-search chain.
 *
 * Prints the directories `ae run` / `ae build` would search for
 * `import foo` resolution, one per line, in order. Same shape as
 * `python -c "import sys; print(*sys.path,sep='\n')"`. Resolves
 * from the same inputs the real build path uses — repeated
 * `--lib` flags, separator-string `--lib a:b`, AETHER_LIB_DIR env
 * var — so the output answers "what would the toolchain see right
 * now?" without having to read the user's shell config.
 *
 * Usage:
 *     ae lib-path                      # default chain (just `lib`)
 *     ae lib-path --lib a --lib b      # show what these flags resolve to
 *     AETHER_LIB_DIR=a:b ae lib-path   # show what the env var resolves to
 *
 * Issue #413. */
static int cmd_lib_path(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            tc_lib_dir_append(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ae lib-path [--lib <dir>%c<dir>...]...\n",
                   AETHER_LIB_PATH_SEP_CHAR);
            printf("  Print the resolved module-search chain in order.\n");
            printf("  Useful for debugging \"why isn't my import resolving?\"\n");
            printf("  Inputs (in priority order, highest first):\n");
            printf("    1. --lib flags on this command line\n");
            printf("    2. AETHER_LIB_DIR env var\n");
            printf("    3. default: `lib`\n");
            return 0;
        } else {
            fprintf(stderr, "ae lib-path: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    /* CLI --lib flags win; env var seeds the chain if no flags set it.
     * Default = `lib` if neither. Walk the resolved array directly so
     * the output matches what the real toolchain would see byte-for-
     * byte (same normalisation, same dedup, same trailing-slash rule). */
    if (tc.lib_dir_count == 0) {
        const char* env = getenv("AETHER_LIB_DIR");
        if (env && *env) {
            tc_lib_dir_append(env);
        }
    }
    /* Force LF-only output. On Windows, the C runtime opens stdout
     * in text mode by default, converting every `\n` to `\r\n`.
     * That breaks string-equality comparisons in shell tests
     * (LF vs CRLF) AND breaks Unix tools that consume the output.
     * Writing the bytes directly via `fwrite` to stdout in text mode
     * still triggers the conversion; the right call is
     * `_setmode(_fileno(stdout), _O_BINARY)` so subsequent writes
     * pass through unchanged. On POSIX the call is a no-op. */
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (tc.lib_dir_count == 0) {
        fputs("lib\n", stdout);
        return 0;
    }
    for (int i = 0; i < tc.lib_dir_count; i++) {
        fputs(tc.lib_dirs[i], stdout);
        fputc('\n', stdout);
    }
    return 0;
}

static int cmd_lib_info(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    fprintf(stderr,
        "ae lib-info: Windows DLL hosting is a follow-up. The metadata\n"
        "is still embedded in the produced artifact; consume it via\n"
        "LoadLibrary + GetProcAddress(\"aether_lib_meta\") for now.\n");
    return 1;
#else
    if (argc < 1) {
        fprintf(stderr,
            "Usage: ae lib-info <path-to-library>\n"
            "\n"
            "Prints the symbol catalog embedded in an `--emit=lib` artifact:\n"
            "  ae build --emit=lib script.ae -o build/script.so\n"
            "  ae lib-info build/script.so\n");
        return 1;
    }
    const char* path = argv[0];

    /* dlopen with RTLD_LAZY — we only need the metadata symbol; the
     * artifact's other functions don't have to bind successfully
     * (a missing runtime dependency would still let lib-info dump
     * what's there). RTLD_LOCAL keeps the library's symbols out of
     * the host's global namespace. */
    void* h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "ae lib-info: dlopen failed: %s\n", dlerror());
        return 1;
    }

    /* Clear stale dlerror state, then dlsym, then check. POSIX
     * specifies dlsym can legitimately return NULL for a defined
     * symbol, so dlerror is the canonical "did the lookup fail"
     * test. */
    (void)dlerror();
    typedef const _AeLibInfoMeta* (*meta_fn_t)(void);
    meta_fn_t meta_fn = (meta_fn_t)dlsym(h, "aether_lib_meta");
    const char* dl_err = dlerror();
    if (!meta_fn || dl_err) {
        fprintf(stderr,
            "ae lib-info: artifact has no `aether_lib_meta` export.\n"
            "Was it built with `--emit=lib`?\n");
        if (dl_err) fprintf(stderr, "  dlerror: %s\n", dl_err);
        dlclose(h);
        return 1;
    }
    const _AeLibInfoMeta* m = meta_fn();
    if (!m) {
        fprintf(stderr, "ae lib-info: aether_lib_meta() returned NULL\n");
        dlclose(h);
        return 1;
    }

    printf("Aether Library: %s\n", path);
    printf("  Schema:        %s\n",
           m->schema_version ? m->schema_version : "(none)");
    printf("  Aether:        %s\n",
           m->aether_version ? m->aether_version : "(unknown)");
    printf("  Source:        %s\n",
           (m->primary_source && m->primary_source[0])
             ? m->primary_source : "(unknown)");
    printf("  Functions:     %d\n", m->function_count);
    printf("  Closures:      %d\n", m->closure_count);
    /* constant_count lives past the "1.1" layout; guard on schema so a
     * pre-1.2 artifact (whose struct stops before this field) is never
     * misread. The codegen always writes "1.<minor>" with minor>=2 when
     * constants are present. */
    int has_consts_field = (m->schema_version &&
                            strcmp(m->schema_version, "1.0") != 0 &&
                            strcmp(m->schema_version, "1.1") != 0);
    if (has_consts_field) {
        printf("  Constants:     %d\n", m->constant_count);
    }
    printf("\n");

    if (m->function_count > 0 && m->functions) {
        for (int i = 0; i < m->function_count; i++) {
            const _AeLibInfoFn* f = &m->functions[i];
            const char* aname = f->aether_name ? f->aether_name : "?";
            const char* csym  = f->c_symbol    ? f->c_symbol    : "?";
            const char* sig   = f->signature   ? f->signature   : "(?) -> ?";
            const char* src   = (f->source_file && f->source_file[0])
                                ? f->source_file : "<unknown>";
            /* Format: name + signature + (c_symbol if different) + source. */
            printf("  - %s%s\n", aname, sig);
            if (strcmp(aname, csym) != 0) {
                printf("        c_symbol: %s\n", csym);
            }
            printf("        @ %s:%d\n", src, f->source_line);
        }
    }

    /* v2 closure-context records (schema >= 1.1). These describe the
     * closure surface the flattened C ABI drops — builder/trailing-block
     * DSL entry points, closure-typed params, and capturing closure
     * literals — so a downstream Aether consumer can reconstruct the
     * builder-DSL with full fidelity. Guarded on the typed pointer being
     * present so a "1.0" function-only artifact prints nothing extra. */
    if (m->closure_count > 0 && m->closures) {
        printf("\n  Closure surface:\n");
        for (int i = 0; i < m->closure_count; i++) {
            const _AeLibInfoClosure* c = &m->closures[i];
            const char* role = c->role ? c->role : "?";
            const char* encl = c->enclosing_export ? c->enclosing_export : "?";
            const char* sig  = c->signature ? c->signature : "";
            const char* nm   = c->name ? c->name : "";
            const char* src  = (c->source_file && c->source_file[0])
                               ? c->source_file : "<unknown>";
            if (nm[0] && strcmp(nm, encl) != 0) {
                printf("  - [%s] %s.%s %s\n", role, encl, nm, sig);
            } else {
                printf("  - [%s] %s %s\n", role, encl, sig);
            }
            for (int k = 0; k < c->capture_count && c->captures; k++) {
                const _AeLibInfoCap* cap = &c->captures[k];
                printf("        captures %s: %s\n",
                       cap->name ? cap->name : "?",
                       cap->type ? cap->type : "?");
            }
            printf("        @ %s:%d\n", src, c->source_line);
        }
    }

    /* v3 constant records (schema >= 1.2). Self-describing dump of the
     * exported scalar/string consts that now cross the .so boundary. Guarded
     * on the schema check above so a pre-1.2 artifact prints nothing extra. */
    if (has_consts_field && m->constant_count > 0 && m->constants) {
        printf("\n  Constants:\n");
        for (int i = 0; i < m->constant_count; i++) {
            const _AeLibInfoConst* k = &m->constants[i];
            printf("  - %s: %s = %s\n",
                   k->name  ? k->name  : "?",
                   k->type  ? k->type  : "?",
                   k->value ? k->value : "?");
        }
    }

    dlclose(h);
    return 0;
#endif
}

// ------------------------------------------------------------- ae fmt -------
// Read an entire file into a malloc'd NUL-terminated buffer (NULL on error).
static char* fmt_read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

// Write `content` to `path` atomically via a temp file + rename.
static int fmt_write_all(const char* path, const char* content) {
    size_t plen = strlen(path);
    char* tmp = (char*)malloc(plen + 16);
    if (!tmp) return -1;
    snprintf(tmp, plen + 16, "%s.aefmt.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    size_t clen = strlen(content);
    int ok = (fwrite(content, 1, clen, f) == clen);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); free(tmp); return -1; }
    remove(path);   // Windows rename won't replace an existing file; no-op risk on POSIX is fine
    if (rename(tmp, path) != 0) { remove(tmp); free(tmp); return -1; }
    free(tmp);
    return 0;
}

// Format one file. check_mode: detect only, don't write. Returns 1 if changed
// (or would change), 0 if already formatted, -1 on error.
static int fmt_one_file(const char* path, int check_mode) {
    char* src = fmt_read_all(path);
    if (!src) { fprintf(stderr, "ae fmt: cannot read %s\n", path); return -1; }
    int changed = 0; const char* err = NULL;
    char* out = ae_format_source_changed(src, &changed, &err);
    if (!out) {
        fprintf(stderr, "ae fmt: %s: %s\n", path, err ? err : "format error");
        free(src);
        return -1;
    }
    int rc = 0;
    if (changed) {
        if (check_mode) { printf("%s\n", path); rc = 1; }
        else if (fmt_write_all(path, out) != 0) { fprintf(stderr, "ae fmt: cannot write %s\n", path); rc = -1; }
        else { printf("%s\n", path); rc = 1; }
    }
    free(src); free(out);
    return rc;
}

static int fmt_has_ae_ext(const char* name) {
    size_t n = strlen(name);
    return n > 3 && strcmp(name + n - 3, ".ae") == 0;
}

// Recurse into a file or directory, formatting every .ae file found. Hidden
// directories (`.git`, `.aether`, ...) are skipped.
static void fmt_walk(const char* path, int check_mode, int* n_changed, int* n_err) {
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "ae fmt: no such path: %s\n", path); (*n_err)++; return; }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (!d) { fprintf(stderr, "ae fmt: cannot open dir: %s\n", path); (*n_err)++; return; }
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            size_t need = strlen(path) + 1 + strlen(e->d_name) + 1;
            char* child = (char*)malloc(need);
            if (!child) continue;
            snprintf(child, need, "%s/%s", path, e->d_name);
            struct stat cst;
            if (stat(child, &cst) == 0) {
                if (S_ISDIR(cst.st_mode)) {
                    if (e->d_name[0] != '.') fmt_walk(child, check_mode, n_changed, n_err);
                } else if (fmt_has_ae_ext(e->d_name)) {
                    int r = fmt_one_file(child, check_mode);
                    if (r == 1) (*n_changed)++; else if (r < 0) (*n_err)++;
                }
            }
            free(child);
        }
        closedir(d);
    } else {
        int r = fmt_one_file(path, check_mode);
        if (r == 1) (*n_changed)++; else if (r < 0) (*n_err)++;
    }
}

static char* fmt_read_stdin(void) {
    size_t cap = 8192, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), stdin)) > 0) {
        if (len + n + 1 > cap) { while (len + n + 1 > cap) cap *= 2; char* nb = (char*)realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
        memcpy(buf + len, tmp, n); len += n;
    }
    buf[len] = '\0';
    return buf;
}

// `ae fmt [--check] [path...]`
//   No path: read stdin, write formatted source to stdout.
//   Paths:   format each .ae file (recursing directories) in place.
//   --check: do not write; list files that would change, exit 1 if any do.
static int cmd_fmt(int argc, char** argv) {
    int check_mode = 0;
    const char** paths = (const char**)malloc(sizeof(char*) * (size_t)(argc > 0 ? argc : 1));
    if (!paths) { fprintf(stderr, "ae fmt: out of memory\n"); return 2; }
    int npaths = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0 || strcmp(argv[i], "-c") == 0) {
            check_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ae fmt [--check] [path...]\n"
                   "  Format Aether source. With no path, reads stdin and writes stdout.\n"
                   "  With paths, formats each .ae file (recursing into directories) in place.\n"
                   "  --check  Do not write; list files that would change, exit 1 if any do.\n");
            free(paths);
            return 0;
        } else {
            paths[npaths++] = argv[i];
        }
    }

    if (npaths == 0) {
        char* src = fmt_read_stdin();
        if (!src) { fprintf(stderr, "ae fmt: cannot read stdin\n"); free(paths); return 2; }
        const char* err = NULL;
        char* out = ae_format_source(src, &err);
        if (!out) { fprintf(stderr, "ae fmt: %s\n", err ? err : "format error"); free(src); free(paths); return 2; }
        int changed = strcmp(src, out) != 0;
        if (!check_mode) fputs(out, stdout);
        free(src); free(out); free(paths);
        return (check_mode && changed) ? 1 : 0;
    }

    int n_changed = 0, n_err = 0;
    for (int i = 0; i < npaths; i++) fmt_walk(paths[i], check_mode, &n_changed, &n_err);
    free(paths);
    if (n_err > 0) return 2;
    return (check_mode && n_changed > 0) ? 1 : 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Set UTF-8 console codepage so Aether programs can print Unicode correctly
    // on Windows CMD and PowerShell (default CP1252/OEM is not UTF-8).
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (argc < 2) {
        print_usage();
        return 1;
    }

    // Parse global flags before command
    int cmd_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            tc.verbose = true;
        } else {
            cmd_idx = i;
            break;
        }
    }

    const char* cmd = argv[cmd_idx];
    int sub_argc = argc - cmd_idx - 1;
    char** sub_argv = argv + cmd_idx + 1;

    // Parse verbose flag after command too
    for (int i = 0; i < sub_argc; i++) {
        if (strcmp(sub_argv[i], "-v") == 0 || strcmp(sub_argv[i], "--verbose") == 0) {
            tc.verbose = true;
        }
    }

    // Commands that don't need toolchain
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        /* `ae help <script.ae>` — heuristic diagnostics for closure-DSL
         * config scripts (issue #414). The disambiguator checks whether
         * the next argv is a path ending in `.ae` that actually exists;
         * bare `ae help` falls through to the usage banner. */
        if (sub_argc > 0 && ae_help_is_script_target(sub_argv[0])) {
            return ae_help_main(sub_argc, sub_argv);
        }
        print_usage();
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        return cmd_version(sub_argc, sub_argv);
    }
    // Top-level version-management aliases (no toolchain needed). These
    // make the intuitive commands work instead of only the longer
    // "ae version install/use" forms.
    if (strcmp(cmd, "install") == 0) {
        return cmd_install(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "upgrade") == 0 || strcmp(cmd, "update") == 0) {
        return cmd_upgrade();
    }
    if (strcmp(cmd, "use") == 0) {
        if (sub_argc < 1) {
            fprintf(stderr, "Usage: ae use <version>   (e.g. ae use v0.231.0)\n");
            fprintf(stderr, "Run 'ae version list' to see installed/available versions.\n");
            return 1;
        }
        return cmd_version_use(sub_argv[0]);
    }
    if (strcmp(cmd, "init") == 0) {
        return cmd_init(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "fmt") == 0) {
        return cmd_fmt(sub_argc, sub_argv);
    }
    // All other commands need the toolchain
    discover_toolchain();

    if (strcmp(cmd, "bindgen") == 0) {
        if (sub_argc < 1 || strcmp(sub_argv[0], "consts") != 0) {
            fprintf(stderr, "Usage: ae bindgen consts <header.h> [-I dir]... [--match PREFIX] [-o out.ae]\n");
            return 1;
        }
        /* The same C compiler the build uses; on Windows this is the
         * WinLibs gcc ensure_gcc_windows resolves. */
#ifdef _WIN32
        if (!ensure_gcc_windows()) return 1;
        return ae_bindgen_consts(s_gcc_bin, sub_argc - 1, sub_argv + 1);
#else
        return ae_bindgen_consts("cc", sub_argc - 1, sub_argv + 1);
#endif
    }
    if (strcmp(cmd, "run") == 0)      return cmd_run(sub_argc, sub_argv);
    if (strcmp(cmd, "build") == 0)    return cmd_build(sub_argc, sub_argv);
    if (strcmp(cmd, "check") == 0)    return cmd_check(sub_argc, sub_argv);
    if (strcmp(cmd, "inspect") == 0)  return cmd_inspect(sub_argc, sub_argv);
    if (strcmp(cmd, "test") == 0)     return cmd_test(sub_argc, sub_argv);
    if (strcmp(cmd, "examples") == 0) return cmd_examples(sub_argc, sub_argv);
    if (strcmp(cmd, "add") == 0)      return cmd_add(sub_argc, sub_argv);
    if (strcmp(cmd, "cache") == 0)    return cmd_cache(sub_argc, sub_argv);
    if (strcmp(cmd, "cflags") == 0)   return cmd_cflags(sub_argc, sub_argv);
    if (strcmp(cmd, "repl") == 0)     return cmd_repl();
    if (strcmp(cmd, "lib-info") == 0) return cmd_lib_info(sub_argc, sub_argv);
    if (strcmp(cmd, "lib-path") == 0) return cmd_lib_path(sub_argc, sub_argv);

    fprintf(stderr, "Unknown command '%s'. Run 'ae help' for usage.\n", cmd);
    return 1;
}
