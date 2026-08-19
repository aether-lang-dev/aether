#ifndef AE_INTERNAL_H
#define AE_INTERNAL_H

/* Shared surface between the `ae` driver's translation units (#1221).
 * ae.c was one 8.5k-line TU, so any edit recompiled everything; command
 * clusters now move into their own ae_*.c files and reach the driver's
 * state through this header. Everything here is one-program-internal:
 * external linkage exists only so the TUs can link together. */

#include <stdbool.h>
#include <stddef.h>

#include "../compiler/aether_lib_path.h"

typedef struct {
    char root[1024];           // Aether root directory
    char compiler[2048];       // Path to aetherc (root + /bin/aetherc = up to 1036 bytes)
    char lib[1024];            // Path to libaether.a (if exists)
    char*  include_flags;      // -I flags for GCC. Heap-grown: a fixed ceiling
                               // silently dropped entries once the install
                               // prefix was long enough (153 dirs under a
                               // /var/folders/.../T/tmp.XXXX/ path overflow
                               // 16 KB), and a build could then fail to find
                               // headers that are present.
    size_t include_flags_cap;  // bytes allocated for include_flags
    char*  runtime_srcs;       // Runtime .c files, source build. Heap-grown
                               // for the same reason as include_flags: 91
                               // MANIFEST paths under a 60-char prefix pass
                               // 8 KB, and the overflow used to silently
                               // substitute a shorter hand-written list.
    size_t runtime_srcs_cap;   // bytes allocated for runtime_srcs
    bool has_lib;              // Whether precompiled lib exists
    bool dev_mode;             // Running from source tree
    bool verbose;              // Verbose output
    /* Lib-search path forwarded to aetherc as one `--lib <dir>` flag
     * per entry. Stored as an array (rather than a separator-string
     * buffer) so we never re-construct a `dir1:dir2:dir3` string that
     * has to survive shell quoting through system() — cmd.exe and
     * MSYS2 between them mangle `;`-separated quoted strings unevenly,
     * and one-flag-per-entry sidesteps the entire surface. Each
     * `--lib X` from the user is parsed: if `X` is itself a separator-
     * string, each piece is appended; if it's a single directory, it's
     * appended verbatim. Issue #413. */
    char lib_dirs[AETHER_LIB_DIRS_MAX][256];
    int  lib_dir_count;
} Toolchain;

extern Toolchain tc;

/* Resolves `tc` (root, compiler path, include flags). Exported so
 * `ae --version` can report the aetherc it would actually run. */
void discover_toolchain(void);
extern char s_cache_dir[512];   /* resolved once by init_cache_dir (ae_cache.c) */

/* ae.c helpers shared across TUs. */
int  run_cmd_show_warnings(const char* cmd);
bool path_exists(const char* path);
void mkdirs(const char* path);
const char* get_cflags(void);
const char* get_home_dir(void);
bool get_exe_path(char* buf, size_t size);
bool dir_exists(const char* path);
void macos_prepare_binary(const char* path);

const char* get_temp_dir(void);
int aetherc_capture_stdout(const char* arg1, const char* in_path,
                           const char* extra_flag,
                           char* out, size_t out_sz);

int  run_cmd(const char* cmd);
int  run_cmd_quiet(const char* cmd);
void build_aetherc_cmd(char* cmd, size_t cmd_size,
                       const char* input, const char* output);
void build_gcc_cmd(char* cmd, size_t size,
                   const char* c_file, const char* out_file,
                   bool optimize, const char* extra_files);

/* ae_version.c — version manager (list/install/switch releases). */
int cmd_version(int argc, char** argv);
/* Download a URL to a path / unpack an archive into a directory. Both
 * shell out to whatever the platform has (curl or wget; tar or
 * Expand-Archive) and return 0 on success. Shared with `ae add`'s
 * release-artifact path (#1360). */
int ae_download(const char* url, const char* dest);
int ae_extract(const char* archive, const char* dest_dir);
int cmd_version_use(const char* version);
int cmd_install(int argc, char** argv);
int cmd_upgrade(void);

/* ae_repl.c — the interactive REPL. */
int cmd_repl(void);

/* ae_cache.c — build cache (content-hashed keys, publish, GC, ae cache). */
int  cache_publish(const char* tmp_path, const char* final_path);
void remove_dsym_bundle(const char* exe_path);
void gc_stale_cache_tmp(const char* dir);
void init_cache_dir(void);
void tc_lib_dir_append(const char* spec);
unsigned long long compute_cache_key(const char* ae_file, const char* extra_files,
                                     const char* opt_level, const char* extra_salt);

/* ae_cross.c — cross-compilation via the zig cc backend (#1105), plus the
 * Xcode/xcrun backend for Apple targets that zig cannot serve. */
const char* cross_target_to_zig(const char* t);
bool cross_target_is_apple(const char* triple);
const char* cross_apple_sdk(const char* triple);
bool cross_uses_unsupported_module(const char* file, char* which, size_t wsz);
/* #1648: compile one generated .c to a target-format object (zig cc -c),
 * without assembling or linking the runtime. Backs `--emit=obj --target=`. */
int  run_cross_compile_obj(const char* c_file, const char* obj_file,
                           bool optimize, const char* ztriple);
int  run_cross_build(const char* c_file, const char* out_file,
                     bool optimize, const char* extra_files,
                     const char* ztriple, bool emit_lib);

#endif /* AE_INTERNAL_H */
