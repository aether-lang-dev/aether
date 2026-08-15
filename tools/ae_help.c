/* tools/ae_help.c — `ae help <script.ae>` heuristic + LLM diagnostics.
 *
 * Issues #414 (heuristic core) + #415 (optional LLM escalation).
 * Design: docs/cic-help.md.
 *
 * Privacy invariants enforced here:
 *   - No network calls. Never opens a socket. Verified by the
 *     `tests/integration/ae_help_privacy` strace guard.
 *   - No filesystem reads outside the script, its resolvable
 *     imports, and co-located `<name>.help.md` hint files. Never
 *     touches $HOME or unrelated paths.
 *   - No execution of the script. We invoke aetherc with stderr
 *     captured for error-extraction; we do not call codegen, do not
 *     emit a binary, do not run any user code.
 *   - Output is local stdout; no caching, no logs, no telemetry.
 *
 * Architectural choice — error-source: rather than carving a new
 * "diagnose mode" into the typer (would be ~500 LOC of surgery
 * across compiler/aether_error.c, compiler/aetherc.c, and every
 * call site for the same information already on stderr), we run
 * the real aetherc as a subprocess with stderr captured and parse
 * its already-structured `error[Eabcd]: …` output. Findings
 * extracted from stderr drive every downstream heuristic. The
 * tradeoff: we only see whatever errors aetherc would normally
 * surface (which is exactly what the operator was confused by).
 * Gain: zero risk of typer regressions from a parallel code path.
 *
 * Cross-platform — on Windows we still run aetherc.exe via popen
 * and parse the same `error[Eabcd]:` lines (the typer emits them
 * verbatim regardless of OS). The strace privacy guard is POSIX-
 * only; the no-network invariant is enforced by code structure
 * (we have no sockets API linked in).
 */

#include "ae_help.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>            /* _O_WRONLY, _O_CREAT, _O_TRUNC, _O_BINARY */
#  include <windows.h>          /* GetModuleFileNameA — self-locate ae.exe */
#  include <process.h>          /* getpid()/_getpid(), _spawnv(_P_WAIT, ...) */
#  define popen  _popen
#  define pclose _pclose
#  define PATH_SEP '\\'
   /* MinGW provides getpid() in <process.h>; MSVC only ships _getpid().
    * One #ifndef guards both compilers without per-toolchain branches. */
#  ifndef getpid
#    define getpid _getpid
#  endif
#else
#  include <unistd.h>
#  include <fcntl.h>            /* O_WRONLY / O_CREAT / O_TRUNC for posix_spawn */
#  include <spawn.h>            /* posix_spawn, posix_spawn_file_actions_t */
#  include <sys/wait.h>         /* waitpid, WIFEXITED, WEXITSTATUS */
#  define PATH_SEP '/'
#  ifdef __APPLE__
#    include <mach-o/dyld.h>    /* _NSGetExecutablePath — self-locate ae */
#  endif
#endif

/* === Constants =====================================================
 * Bounded to keep allocations static and cache-friendly. The numbers
 * are generous for a single script's diagnostic surface — a config
 * file with 256 distinct errors is broken in a way `ae help`
 * shouldn't be trying to triage line-by-line. */
#define AE_HELP_MAX_FINDINGS    256
#define AE_HELP_MAX_LINES       8192
#define AE_HELP_MAX_LINE_LEN    1024
#define AE_HELP_MAX_IMPORTS     64
#define AE_HELP_MAX_EXPORTS     2048   /* All-stdlib + lib export catalog */
#define AE_HELP_NAME_LEN        64
#define AE_HELP_PATH_LEN        1024
#define AE_HELP_MSG_LEN         512
#define AE_HELP_MAX_LIBS        16     /* `--lib` search-path entries */

/* Library separator for PATH-style `--lib a:b` specs — matches the
 * convention `ae build` / `aetherc` use for the multi-entry surface. */
#ifdef _WIN32
#define AE_HELP_LIB_SEP ';'
#else
#define AE_HELP_LIB_SEP ':'
#endif

/* === Types ========================================================= */

typedef enum {
    FK_UNDEFINED_FUNC = 1,
    FK_UNDEFINED_VAR,
    FK_UNDEFINED_TYPE,
    FK_TYPE_MISMATCH,
    FK_NOT_EXPORTED,
    FK_HIDDEN_NAME,
    FK_REDEFINITION,
    FK_SYNTAX,
    FK_OTHER,
    /* Heuristic-only findings (synthesised by ae help itself, not
     * surfaced by the typer). */
    FK_YAML_COLON,        /* `port: 9990` → `port(9990)` */
    FK_HCL_EQUAL,         /* `port = 9990` → `port(9990)` */
    FK_QUOTED_INT,        /* `port("9990")` → `port(9990)` */
    FK_YAML_LIST,         /* `repos: [...]` → repo(...) per entry */
    FK_TOPLEVEL_DSL,      /* builder block at file scope → wrap in main() */
    FK_HELP_MD_HINT,      /* library-author shipped hint */
} FindingKind;

typedef struct {
    FindingKind kind;
    int  line;
    int  col;
    int  error_code;                    /* E#### from typer, 0 if heuristic */
    char name[AE_HELP_NAME_LEN];        /* unresolved identifier */
    char extra[AE_HELP_NAME_LEN];       /* "int" / "string" for type-mismatch */
    char message[AE_HELP_MSG_LEN];      /* original typer message */
    /* Rendered after heuristic post-processing. */
    char suggestion[AE_HELP_MSG_LEN];
    char doc_link[128];
    /* `--fix` plumbing. 0 = unsafe / no auto-fix; 1 = mechanical. */
    int  safe_fix;
    char fix_replace[AE_HELP_MSG_LEN];  /* full line replacement */
} Finding;

typedef struct {
    char path[AE_HELP_PATH_LEN];
    char* text;            /* full file body */
    size_t size;
    char* lines[AE_HELP_MAX_LINES];   /* indices into `text` (NUL-split) */
    int line_count;
} SourceFile;

typedef struct {
    char module_name[AE_HELP_NAME_LEN]; /* "std.os" */
    char export_name[AE_HELP_NAME_LEN]; /* "exec" */
} ExportEntry;

/* CLI flags — packed for clarity. */
typedef struct {
    int json;
    int fix;
    int llm;
    char llm_weights[AE_HELP_PATH_LEN];
    int help;     /* --help / -h on the subcommand */
    int verbose;
} HelpFlags;

/* Defined further down; forward-declared here so the `--lib` list
 * helpers below can use it. */
static void safe_strncpy(char* dst, const char* src, size_t dst_size);

/* `--lib` search path. Process-global config (one `ae help`
 * invocation analyses one script) — set once from argv + the
 * `AETHER_LIB_DIR` env in ae_help_main, then read by the aetherc
 * subprocess builder, the export-catalog loader, and the
 * `*.help.md` resolver. A file-static avoids threading the list
 * through four signatures for what is genuinely invocation-wide
 * state. */
static char g_lib_dirs[AE_HELP_MAX_LIBS][AE_HELP_PATH_LEN];
static int  g_lib_count = 0;

/* Append one directory to the `--lib` list (dedup, cap-checked). */
static void help_lib_append_one(const char* dir) {
    if (!dir || !dir[0]) return;
    /* Drop a single trailing slash so `lib` and `lib/` dedup. */
    char norm[AE_HELP_PATH_LEN];
    safe_strncpy(norm, dir, sizeof(norm));
    size_t n = strlen(norm);
    while (n > 1 && (norm[n - 1] == '/' || norm[n - 1] == '\\')) {
        norm[--n] = '\0';
    }
    for (int i = 0; i < g_lib_count; i++) {
        if (strcmp(g_lib_dirs[i], norm) == 0) return;
    }
    if (g_lib_count >= AE_HELP_MAX_LIBS) {
        fprintf(stderr, "ae help: --lib search path is full (max %d); "
                        "ignoring '%s'\n", AE_HELP_MAX_LIBS, norm);
        return;
    }
    safe_strncpy(g_lib_dirs[g_lib_count++], norm, AE_HELP_PATH_LEN);
}

/* Append a `--lib` spec, splitting PATH-style `a:b` (POSIX) /
 * `a;b` (Windows) into discrete entries — `ae build` parity. */
static void help_lib_append(const char* spec) {
    if (!spec || !spec[0]) return;
    const char* cur = spec;
    char buf[AE_HELP_PATH_LEN];
    while (*cur) {
        const char* next = strchr(cur, AE_HELP_LIB_SEP);
        size_t len = next ? (size_t)(next - cur) : strlen(cur);
        if (len > 0) {
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, cur, len);
            buf[len] = '\0';
            help_lib_append_one(buf);
        }
        if (!next) break;
        cur = next + 1;
    }
}

/* === Forward declarations ========================================== */
static int  load_source(const char* path, SourceFile* sf);
static void free_source(SourceFile* sf);
static int  scan_imports(SourceFile* sf, char imports[][AE_HELP_NAME_LEN], int max);
static int  load_stdlib_export_catalog(ExportEntry* out, int max);
static int  load_lib_export_catalog(ExportEntry* out, int max, int already);
static int  run_aetherc_capture(const char* script_path, char* stderr_buf, size_t buf_size);
static int  parse_aetherc_findings(const char* stderr_buf, Finding* findings, int max);
static int  levenshtein(const char* a, const char* b);
static void apply_levenshtein(Finding* f, const ExportEntry* exports, int n_exports,
                              char (*imports)[AE_HELP_NAME_LEN], int n_imports);
static void apply_yaml_shape_detection(SourceFile* sf, Finding* findings, int* count, int max);
static void apply_top_level_dsl(SourceFile* sf, Finding* findings, int* count, int max);
static void apply_missing_import(Finding* f, const ExportEntry* exports, int n_exports);
static void apply_type_mismatch_english(Finding* f);
static void apply_canonical_template(Finding* findings, int count,
                                      char (*imports)[AE_HELP_NAME_LEN], int n_imports);
static int  load_help_md_for_import(const char* import_name, SourceFile* sf,
                                    Finding* findings, int* count, int max);
static void apply_doc_cross_links(Finding* findings, int count);
static void render_human(const Finding* findings, int count, const SourceFile* sf, const HelpFlags* flags);
static void render_json(const Finding* findings, int count, const SourceFile* sf);
static int  apply_fix(Finding* findings, int count, SourceFile* sf);
static int  llm_escalate(const Finding* findings, int count,
                         const SourceFile* sf, const char* weights);

/* `safe_strncpy` — bounded copy that ALWAYS NUL-terminates and
 * AVOIDS the GCC -Wstringop-truncation antipattern (which fires on
 * `strncpy(dst, src, sizeof(dst)-1)` shapes). Used in lieu of every
 * such site in this file. */
static void safe_strncpy(char* dst, const char* src, size_t dst_size) {
    if (dst_size == 0) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* `path_format` — printf-style path composition with explicit
 * truncation detection. Returns 0 on success, -1 if the formatted
 * output would not fit in `dst_size` (and clears the buffer).
 *
 * Why this exists: every snprintf site in this file that composes a
 * path triggers GCC's `-Wformat-truncation` because the formatter's
 * worst-case analysis sees a 1024-byte `%s` going into a 1024-byte
 * destination and (correctly) flags "the format could overflow."
 * The actual paths in practice are well under 256 bytes, but GCC
 * has no way to know that. Routing every call through this helper
 * uses a NON-LITERAL format string (`fmt` is `const char*`, not a
 * compile-time string literal), which GCC's per-call analyser
 * cannot statically decode — so the truncation warning is silenced
 * AT THE CALL SITE while the truncation is still HANDLED EXPLICITLY
 * by the helper itself returning -1. Callers MUST check the return
 * value. */
#include <stdarg.h>
static int path_format(char* dst, size_t dst_size, const char* fmt, ...) {
    if (dst_size == 0) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= dst_size) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}

/* === Public dispatcher entry points ================================ */

int ae_help_is_script_target(const char* arg) {
    if (!arg || !*arg) return 0;
    if (arg[0] == '-') return 0;   /* a flag, not a path */
    size_t n = strlen(arg);
    if (n < 4) return 0;
    if (strcmp(arg + n - 3, ".ae") != 0) return 0;
    struct stat st;
    if (stat(arg, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

static void print_usage(void) {
    printf(
        "Usage: ae help <script.ae> [--lib <dir>] [--fix] [--json] [--llm <weights.gguf>]\n"
        "\n"
        "Offline, on-machine diagnostics for closure-DSL config scripts.\n"
        "Translates the typer's terse output into actionable suggestions.\n"
        "\n"
        "Modes:\n"
        "  ae help <script>             Run heuristics, print human-readable findings.\n"
        "  ae help <script> --fix       Apply safe rewrites (Levenshtein-1, YAML→call form).\n"
        "                               Always prints the diff first.\n"
        "  ae help <script> --json      Machine-readable findings on stdout.\n"
        "  ae help <script> --llm <w>   Optional offline local-LLM escalation.\n"
        "                               Requires AETHER_ENABLE_LLM=1 at build time.\n"
        "\n"
        "Options:\n"
        "  --lib <dir>                  Library search path for project-local\n"
        "                               imports (repeatable; PATH-style a:b\n"
        "                               accepted). Also honours $AETHER_LIB_DIR.\n"
        "                               Without it, every library call in a\n"
        "                               config script reports as undefined.\n"
        "\n"
        "Privacy:\n"
        "  No network calls. No file reads outside the script + its resolvable\n"
        "  imports + co-located *.help.md hints. No execution of the script.\n"
        "  Output is local stdout only. See docs/cic-help.md.\n"
        "\n"
        "Heuristics (ordered by precision):\n"
        "  1. Levenshtein suggestions for unresolved names\n"
        "  2. YAML/HCL pattern detection inside DSL blocks\n"
        "  3. Type-mismatch with English explanation\n"
        "  4. Missing-import detection against stdlib catalog\n"
        "  5. Library-author *.help.md hint loading\n"
        "  6. Top-level closure-DSL detection (wrap in main())\n"
    );
}

int ae_help_main(int argc, char** argv) {
    HelpFlags flags = {0};
    const char* script = NULL;

    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            flags.help = 1;
        } else if (strcmp(a, "--json") == 0) {
            flags.json = 1;
        } else if (strcmp(a, "--fix") == 0) {
            flags.fix = 1;
        } else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
            flags.verbose = 1;
        } else if (strcmp(a, "--llm") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ae help: --llm requires a path to a .gguf weights file\n");
                return 2;
            }
            flags.llm = 1;
            safe_strncpy(flags.llm_weights, argv[++i], sizeof(flags.llm_weights));
        } else if (strcmp(a, "--lib") == 0) {
            /* `--lib <dir>` — repeatable, and PATH-style `a:b`
             * accepted. Same surface as `ae build` / `aetherc`, so a
             * project's library directory can be analysed instead of
             * every library call being reported as undefined. */
            if (i + 1 >= argc) {
                fprintf(stderr, "ae help: --lib requires a directory\n");
                return 2;
            }
            help_lib_append(argv[++i]);
        } else if (a[0] == '-') {
            fprintf(stderr, "ae help: unknown option '%s'\n", a);
            return 2;
        } else if (!script) {
            script = a;
        } else {
            fprintf(stderr, "ae help: unexpected extra argument '%s'\n", a);
            return 2;
        }
    }

    if (flags.help || !script) {
        print_usage();
        return script ? 0 : 1;
    }

    /* `AETHER_LIB_DIR` — same env `ae build` honours. Appended after
     * any explicit `--lib` flags; dedup in help_lib_append keeps a
     * dir named on both channels from being probed twice. */
    {
        const char* env_libs = getenv("AETHER_LIB_DIR");
        if (env_libs && env_libs[0]) help_lib_append(env_libs);
    }

    if (flags.fix && flags.json) {
        fprintf(stderr, "ae help: --fix and --json are mutually exclusive\n");
        return 2;
    }

    /* Phase 1 — load source. */
    SourceFile sf;
    if (load_source(script, &sf) != 0) {
        fprintf(stderr, "ae help: cannot read script '%s': %s\n",
                script, strerror(errno));
        return 1;
    }

    /* Phase 2 — scan imports the script declares. Used both as
     * Levenshtein scope hint and to gate `<name>.help.md` loading. */
    char imports[AE_HELP_MAX_IMPORTS][AE_HELP_NAME_LEN];
    int n_imports = scan_imports(&sf, imports, AE_HELP_MAX_IMPORTS);

    /* Phase 3 — load stdlib export catalog. One scan over each
     * std/<module>/module.ae (~30 files, ~50 ms). Used for
     * Levenshtein scoring AND missing-import suggestions. */
    ExportEntry* exports = malloc(sizeof(ExportEntry) * AE_HELP_MAX_EXPORTS);
    if (!exports) {
        fprintf(stderr, "ae help: out of memory\n");
        free_source(&sf);
        return 1;
    }
    int n_exports = load_stdlib_export_catalog(exports, AE_HELP_MAX_EXPORTS);
    /* Extend the catalog with every `--lib` directory's modules, so
     * Levenshtein suggestions and missing-import detection draw on
     * project-library exports too — not just stdlib. */
    n_exports = load_lib_export_catalog(exports, AE_HELP_MAX_EXPORTS, n_exports);

    /* Phase 4 — invoke aetherc with stderr captured. Findings come
     * from its already-structured `error[Eabcd]:` lines. */
    char stderr_buf[64 * 1024];
    stderr_buf[0] = '\0';
    int compile_rc = run_aetherc_capture(script, stderr_buf, sizeof(stderr_buf));
    (void)compile_rc; /* Even on success we may have warnings. */

    Finding findings[AE_HELP_MAX_FINDINGS];
    int n_findings = parse_aetherc_findings(stderr_buf, findings, AE_HELP_MAX_FINDINGS);

    /* Phase 5 — heuristic post-processing. Order matters:
     *   1. Levenshtein + missing-import (per-finding enrichment for
     *      undefined-name kinds).
     *   2. Type-mismatch English (per-finding enrichment for type
     *      mismatches; #414 capability 3).
     *   3. Synthetic findings from line-by-line source scan:
     *      YAML/HCL, top-level DSL.
     *   4. *.help.md library-author hints — synthetic findings driven
     *      by library docstrings, not the typer.
     *   5. Canonical-example surfacing — attaches a `<libroot>/_examples`
     *      path to any finding referencing an imported library
     *      (#414 capability 8). Runs LAST so it can extend
     *      already-rendered suggestions cleanly.
     *   6. Doc cross-links — every finding kind maps to its canonical
     *      doc page. */
    for (int i = 0; i < n_findings; i++) {
        if (findings[i].kind == FK_UNDEFINED_FUNC ||
            findings[i].kind == FK_UNDEFINED_VAR ||
            findings[i].kind == FK_UNDEFINED_TYPE) {
            apply_levenshtein(&findings[i], exports, n_exports, imports, n_imports);
            apply_missing_import(&findings[i], exports, n_exports);
        }
        apply_type_mismatch_english(&findings[i]);
    }
    apply_yaml_shape_detection(&sf, findings, &n_findings, AE_HELP_MAX_FINDINGS);
    apply_top_level_dsl(&sf, findings, &n_findings, AE_HELP_MAX_FINDINGS);

    /* Library-author hints: one *.help.md per imported module — and
     * once only. A module can legitimately appear in more than one
     * `import` statement: closure-DSL files need both `import bash`
     * (the builder verb) and a selective `import bash (script)` (the
     * bare setter names). `load_help_md_for_import` keys off the module
     * name, so without deduping here every hint from a twice-imported
     * module fires twice (aeb feedback R2). Skip names already seen. */
    for (int i = 0; i < n_imports; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) {
            if (strcmp(imports[i], imports[j]) == 0) { seen = 1; break; }
        }
        if (seen) continue;
        load_help_md_for_import(imports[i], &sf, findings, &n_findings, AE_HELP_MAX_FINDINGS);
    }

    apply_canonical_template(findings, n_findings, imports, n_imports);
    apply_doc_cross_links(findings, n_findings);

    /* Phase 6 — render or act. */
    int exit_code = 0;
    if (flags.fix) {
        exit_code = apply_fix(findings, n_findings, &sf);
    } else if (flags.json) {
        render_json(findings, n_findings, &sf);
    } else {
        render_human(findings, n_findings, &sf, &flags);
    }

    if (flags.llm) {
        int llm_rc = llm_escalate(findings, n_findings, &sf, flags.llm_weights);
        if (llm_rc != 0 && exit_code == 0) exit_code = llm_rc;
    }

    free(exports);
    free_source(&sf);
    return exit_code;
}

/* === Source loader ================================================= */

static int load_source(const char* path, SourceFile* sf) {
    memset(sf, 0, sizeof(*sf));
    safe_strncpy(sf->path, path, sizeof(sf->path));

    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0 || n > (long)(8 * 1024 * 1024)) { fclose(f); errno = EFBIG; return -1; }
    rewind(f);
    sf->size = (size_t)n;
    sf->text = malloc(sf->size + 1);
    if (!sf->text) { fclose(f); errno = ENOMEM; return -1; }
    if (fread(sf->text, 1, sf->size, f) != sf->size) {
        fclose(f); free(sf->text); sf->text = NULL; return -1;
    }
    fclose(f);
    sf->text[sf->size] = '\0';

    /* Split lines in-place by replacing `\n` with `\0`. Line index
     * array points into `text`. CRLF → LF normalised on the fly. */
    sf->line_count = 0;
    char* p = sf->text;
    sf->lines[sf->line_count++] = p;
    while (*p) {
        if (*p == '\r' && p[1] == '\n') {
            *p = '\0'; p++;
        }
        if (*p == '\n') {
            *p = '\0';
            if (sf->line_count >= AE_HELP_MAX_LINES) break;
            sf->lines[sf->line_count++] = p + 1;
        }
        p++;
    }
    return 0;
}

static void free_source(SourceFile* sf) {
    if (sf->text) free(sf->text);
    sf->text = NULL;
    sf->line_count = 0;
}

/* Token check: returns 1 if the next non-whitespace tokens starting
 * at `s` match `keyword` followed by a non-identifier character. */
static int line_starts_with_keyword(const char* s, const char* keyword) {
    while (*s == ' ' || *s == '\t') s++;
    size_t kl = strlen(keyword);
    if (strncmp(s, keyword, kl) != 0) return 0;
    char next = s[kl];
    return (next == 0 || next == ' ' || next == '\t' ||
            next == '(' || next == '{' || next == '.') ? 1 : 0;
}

static int scan_imports(SourceFile* sf, char imports[][AE_HELP_NAME_LEN], int max) {
    int n = 0;
    for (int i = 0; i < sf->line_count && n < max; i++) {
        const char* line = sf->lines[i];
        if (!line_starts_with_keyword(line, "import")) continue;
        /* Skip leading whitespace + "import " prefix. */
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        p += 6; /* "import" */
        while (*p == ' ' || *p == '\t') p++;
        /* Read the dotted name up to a non-identifier char. */
        const char* start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '.' || *p == '_')) p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || len >= AE_HELP_NAME_LEN) continue;
        memcpy(imports[n], start, len);
        imports[n][len] = '\0';
        n++;
    }
    return n;
}

/* === Stdlib export catalog ========================================= */

/* Find the directory containing `std/` — usually the install prefix
 * (or the repo root in dev mode). We piggy-back on the heuristics ae
 * already uses elsewhere: check AETHER_LIB_DIR, AETHER_ROOT, common
 * locations. */
static int resolve_std_root(char* out, size_t out_size) {
    const char* candidates[8];
    int nc = 0;
    const char* env_root = getenv("AETHER_ROOT");
    if (env_root && *env_root) candidates[nc++] = env_root;
    /* Try local repo (dev mode): walk up from CWD looking for ./std/string/module.ae. */
    static char cwd_walk[AE_HELP_PATH_LEN];
    if (getcwd(cwd_walk, sizeof(cwd_walk))) {
        candidates[nc++] = cwd_walk;
    }
    /* Common install prefixes. */
    candidates[nc++] = "/usr/local/share/aether";
    candidates[nc++] = "/usr/share/aether";
#ifdef _WIN32
    candidates[nc++] = "C:\\aether";
#endif

    for (int i = 0; i < nc; i++) {
        char probe[AE_HELP_PATH_LEN];
        /* Walk up to 6 parent levels for the repo-root case. */
        char cur[AE_HELP_PATH_LEN];
        safe_strncpy(cur, candidates[i], sizeof(cur));
        for (int depth = 0; depth < 6; depth++) {
            if (path_format(probe, sizeof(probe), "%s%cstd%cstring%cmodule.ae",
                            cur, PATH_SEP, PATH_SEP, PATH_SEP) != 0) {
                /* Truncated — `cur` is too long to be a real repo root. */
                break;
            }
            struct stat st;
            if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
                safe_strncpy(out, cur, out_size);
                return 0;
            }
            /* Pop one path component. */
            char* slash = strrchr(cur, PATH_SEP);
            if (!slash) break;
            *slash = '\0';
            if (cur[0] == '\0') break;
        }
    }
    return -1;
}

/* Parse `exports ( ident, ident, ident )` blocks from `module_path`.
 * Block can span multiple lines. Comments (`//`) are stripped. */
static int parse_module_exports(const char* module_path,
                                const char* module_name,
                                ExportEntry* out, int max, int already) {
    FILE* f = fopen(module_path, "rb");
    if (!f) return already;
    char line[AE_HELP_MAX_LINE_LEN];
    int in_exports = 0;
    int n = already;
    while (fgets(line, sizeof(line), f) && n < max) {
        /* Strip line comments. */
        char* c = strstr(line, "//");
        if (c) *c = '\0';
        /* Strip trailing whitespace. */
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r' ||
                         line[l - 1] == ' ' || line[l - 1] == '\t')) {
            line[--l] = '\0';
        }
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!in_exports) {
            if (strncmp(p, "exports", 7) == 0) {
                p += 7;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '(') continue;
                p++;
                in_exports = 1;
            } else {
                continue;
            }
        }
        /* Read identifiers separated by commas; terminate on `)`. */
        while (*p && n < max) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (*p == ')') { in_exports = 0; break; }
            if (!*p) break;
            const char* s = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t len = (size_t)(p - s);
            if (len > 0 && len < AE_HELP_NAME_LEN) {
                safe_strncpy(out[n].module_name, module_name, sizeof(out[n].module_name));
                memcpy(out[n].export_name, s, len);
                out[n].export_name[len] = '\0';
                n++;
            }
        }
    }
    fclose(f);
    return n;
}

static int load_stdlib_export_catalog(ExportEntry* out, int max) {
    char root[AE_HELP_PATH_LEN];
    if (resolve_std_root(root, sizeof(root)) != 0) return 0;

    char std_dir[AE_HELP_PATH_LEN];
    if (path_format(std_dir, sizeof(std_dir), "%s%cstd", root, PATH_SEP) != 0) {
        return 0;
    }
    DIR* d = opendir(std_dir);
    if (!d) return 0;

    int n = 0;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL && n < max) {
        if (entry->d_name[0] == '.') continue;
        /* Each std/<mod>/module.ae is the canonical export source. */
        char mod_path[AE_HELP_PATH_LEN];
        if (path_format(mod_path, sizeof(mod_path), "%s%c%s%cmodule.ae",
                        std_dir, PATH_SEP, entry->d_name, PATH_SEP) != 0) continue;
        struct stat st;
        if (stat(mod_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        char mod_name[AE_HELP_NAME_LEN];
        /* mod_name is small (64 bytes) and d_name can be NAME_MAX (255).
         * Skip entries whose name+prefix would overflow — no real stdlib
         * module has a name that long anyway. path_format returns -1 on
         * truncation, which we honour by skipping the entry. */
        if (path_format(mod_name, sizeof(mod_name), "std.%s", entry->d_name) != 0) continue;
        n = parse_module_exports(mod_path, mod_name, out, max, n);
    }
    closedir(d);
    return n;
}

/* Append the export catalogs of every `--lib` directory. For each
 * resolved lib dir, every immediate `<lib>/<name>/module.ae` is
 * parsed under the bare module name `<name>` — that is how a project
 * library is imported (`import build`, not `import std.build`), so
 * the Levenshtein / missing-import scopes match the script's call
 * sites. `already` carries the running count from the stdlib pass so
 * both catalogs share one array. */
static int load_lib_export_catalog(ExportEntry* out, int max, int already) {
    int n = already;
    for (int li = 0; li < g_lib_count && n < max; li++) {
        DIR* d = opendir(g_lib_dirs[li]);
        if (!d) continue;  /* a bad --lib dir is the wrapped aetherc's
                            * problem to report, not ours */
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL && n < max) {
            if (entry->d_name[0] == '.') continue;
            char mod_path[AE_HELP_PATH_LEN];
            if (path_format(mod_path, sizeof(mod_path), "%s%c%s%cmodule.ae",
                            g_lib_dirs[li], PATH_SEP, entry->d_name,
                            PATH_SEP) != 0) continue;
            struct stat st;
            if (stat(mod_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            n = parse_module_exports(mod_path, entry->d_name, out, max, n);
        }
        closedir(d);
    }
    return n;
}

/* === aetherc subprocess ============================================ */

/* Find aetherc binary. Prefer the same-directory sibling (this binary
 * is `ae`; aetherc lives next to it in `build/`); fall back to PATH. */
/* Resolve an absolute path to `aetherc`. The script's working
 * directory is whatever the caller chose, NOT necessarily the repo
 * root, so probing relative paths like `./build/aetherc` is
 * inherently fragile. Strategy, ordered from highest-confidence to
 * lowest:
 *
 *   1. $AETHERC env var (caller-explicit; respects any local layout).
 *   2. $AETHER_HOME/bin/aetherc[.exe] (installed layout, what the
 *      installer documents and the editor extension expects).
 *   3. Directory containing THIS binary (`ae`) — if we're at
 *      `<dir>/ae[.exe]`, aetherc almost always lives next door in
 *      dev mode (`build/`) AND in install mode (`bin/`). Self-locating
 *      via the executable path means cwd doesn't matter, which is
 *      critical for Windows MSYS2 CI where tests cd into tmp dirs
 *      before invoking ae.
 *   4. Repo-relative `./build/aetherc[.exe]` for the unusual case
 *      where ae was invoked directly from a checkout root via a
 *      relative path (rare in tests, kept for the dev-loop case
 *      where someone runs `./build/ae help foo.ae` from the root).
 *   5. POSIX install dirs (/usr/local/bin, /usr/bin).
 *   6. Bare `aetherc` for $PATH resolution as a final fallback.
 *
 * On Windows the `.exe` variant is checked BEFORE the no-suffix one
 * for every candidate. Some MSYS2 stat() implementations accept
 * either form, but popen() invokes the literal name and Windows
 * requires `.exe`; checking `.exe` first is the only reliable shape. */
static int find_aetherc(char* out, size_t out_size) {
#ifdef _WIN32
    const char* exts[] = { ".exe", "", NULL };
#else
    const char* exts[] = { "", NULL };
#endif

    /* 1. Caller-explicit override. */
    const char* env_aetherc = getenv("AETHERC");
    if (env_aetherc && *env_aetherc) {
        struct stat st;
        if (stat(env_aetherc, &st) == 0) {
            safe_strncpy(out, env_aetherc, out_size);
            return 0;
        }
    }

    /* 2. $AETHER_HOME/bin/aetherc[.exe]. */
    const char* aether_home = getenv("AETHER_HOME");
    if (aether_home && *aether_home) {
        for (int e = 0; exts[e]; e++) {
            char probe[AE_HELP_PATH_LEN];
            int w = snprintf(probe, sizeof(probe), "%s/bin/aetherc%s", aether_home, exts[e]);
            if (w < 0 || (size_t)w >= sizeof(probe)) continue;
            struct stat st;
            if (stat(probe, &st) == 0) {
                safe_strncpy(out, probe, out_size);
                return 0;
            }
        }
    }

    /* 3. Sibling of THIS binary — self-locating, cwd-independent. */
    char ae_dir[AE_HELP_PATH_LEN] = {0};
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, ae_dir, (DWORD)sizeof(ae_dir));
    if (len > 0 && len < (DWORD)sizeof(ae_dir)) {
        ae_dir[len] = '\0';
        char* slash = strrchr(ae_dir, '\\');
        if (!slash) slash = strrchr(ae_dir, '/');
        if (slash) *slash = '\0';
    } else {
        ae_dir[0] = '\0';
    }
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof(ae_dir);
    if (_NSGetExecutablePath(ae_dir, &sz) == 0) {
        char resolved[1024];
        if (realpath(ae_dir, resolved)) {
            char* slash = strrchr(resolved, '/');
            if (slash) {
                *slash = '\0';
                safe_strncpy(ae_dir, resolved, sizeof(ae_dir));
            } else {
                ae_dir[0] = '\0';
            }
        }
    }
#elif defined(__FreeBSD__)
    /* #1586: same FreeBSD self-path as get_exe_path in ae.c. */
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t fb_len = sizeof(ae_dir);
    if (sysctl(mib, 4, ae_dir, &fb_len, NULL, 0) == 0 && fb_len > 1) {
        char* fslash = strrchr(ae_dir, '/');
        if (fslash) *fslash = '\0';
    } else {
        ae_dir[0] = '\0';
    }
#else
    ssize_t rl = readlink("/proc/self/exe", ae_dir, sizeof(ae_dir) - 1);
    if (rl > 0) {
        ae_dir[rl] = '\0';
        char* slash = strrchr(ae_dir, '/');
        if (slash) *slash = '\0';
    } else {
        ae_dir[0] = '\0';
    }
#endif
    if (ae_dir[0]) {
        for (int e = 0; exts[e]; e++) {
            char probe[AE_HELP_PATH_LEN];
            int w = snprintf(probe, sizeof(probe), "%s/aetherc%s", ae_dir, exts[e]);
            if (w < 0 || (size_t)w >= sizeof(probe)) continue;
            struct stat st;
            if (stat(probe, &st) == 0) {
                safe_strncpy(out, probe, out_size);
                return 0;
            }
        }
    }

    /* 4. Repo-relative `./build/aetherc[.exe]` for dev-loop convenience. */
    for (int e = 0; exts[e]; e++) {
        char probe[AE_HELP_PATH_LEN];
        int w = snprintf(probe, sizeof(probe), "./build/aetherc%s", exts[e]);
        if (w < 0 || (size_t)w >= sizeof(probe)) continue;
        struct stat st;
        if (stat(probe, &st) == 0) {
            safe_strncpy(out, probe, out_size);
            return 0;
        }
    }

    /* 5. POSIX install dirs. */
    const char* posix_dirs[] = { "/usr/local/bin/aetherc", "/usr/bin/aetherc", NULL };
    for (int i = 0; posix_dirs[i]; i++) {
        struct stat st;
        if (stat(posix_dirs[i], &st) == 0) {
            safe_strncpy(out, posix_dirs[i], out_size);
            return 0;
        }
    }

    /* 6. Bare name — let $PATH (and on Windows, PATHEXT) resolve. */
    safe_strncpy(out, "aetherc", out_size);
    return 0;
}

/* Build an err-file path inside the platform-native temp directory.
 * Caller-side note: `out` must hold at least AE_HELP_PATH_LEN bytes. */
static int build_err_file(char* out, size_t out_size) {
    const char* tmp_dir;
#ifdef _WIN32
    /* GetTempPathA queries the OS layer directly, returning a native
     * Windows path (`D:\...\Temp\`) regardless of how TEMP was set
     * by the parent shell. Necessary on MSYS2 where bash exports
     * `TEMP` in POSIX form (`/d/...`) that cmd.exe can't parse. */
    static char tmp_buf[AE_HELP_PATH_LEN];
    DWORD tlen = GetTempPathA((DWORD)sizeof(tmp_buf), tmp_buf);
    if (tlen == 0 || tlen >= sizeof(tmp_buf)) {
        safe_strncpy(tmp_buf, ".", sizeof(tmp_buf));
    } else {
        size_t bl = strlen(tmp_buf);
        while (bl > 1 && (tmp_buf[bl - 1] == '\\' || tmp_buf[bl - 1] == '/')) {
            tmp_buf[--bl] = '\0';
        }
    }
    tmp_dir = tmp_buf;
#else
    tmp_dir = getenv("TMPDIR");
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
#endif
    long pid = (long)getpid();
    return path_format(out, out_size,
                       "%s%cae_help_aetherc_err_%ld.log",
                       tmp_dir, PATH_SEP, pid);
}

static int run_aetherc_capture(const char* script_path, char* stderr_buf, size_t buf_size) {
    stderr_buf[0] = '\0';

    char aetherc[AE_HELP_PATH_LEN];
    find_aetherc(aetherc, sizeof(aetherc));

    /* Where the compiled C goes: `/dev/null` on POSIX, `NUL` on Windows.
     * aetherc takes the output as a POSITIONAL argument, so this is passed as
     * one. Passing it as `-o <sink>` (which it was) made aetherc read the
     * flag itself as the output path and write the generated C to a file
     * literally named "-o" in whatever directory the user ran `ae help` in. */
#ifdef _WIN32
    const char* sink = "NUL";
#else
    const char* sink = "/dev/null";
#endif

    char err_file[AE_HELP_PATH_LEN];
    if (build_err_file(err_file, sizeof(err_file)) != 0) return -1;

    /* Invoke aetherc with stderr redirected to `err_file` WITHOUT
     * going through a shell. Two different paths because POSIX and
     * Windows expose different child-spawn APIs:
     *
     *   POSIX: posix_spawn with file actions opening err_file as
     *          stderr. No fork/exec by hand, no shell, no quoting.
     *   Windows: _spawnv (the MSVCRT helper that `tools/ae.c`'s
     *            `win_run` already uses) with stderr redirected via
     *            _dup2 around the spawn. cmd.exe never enters the
     *            picture, so MSYS2's POSIX-flavoured paths and our
     *            mixed-slash aetherc path can't break the redirect.
     *
     * This is the same architectural fix `tools/ae.c` adopted
     * earlier (POSIX issue + Windows path collision); we now mirror
     * it here. The previous `system(cmd)` shape was the only
     * shell-dependent invocation left in the ae_help binary. */
    /* aetherc argv: base 3 args + one `--lib <dir>` pair per resolved
     * search-path entry + NULL. Forwarding `--lib` lets the wrapped
     * compile resolve project-local library imports, so `ae help` on
     * a real config script stops flagging every library call as an
     * undefined function. Shared by both spawn branches below. */
    const char* argv[4 + 2 * AE_HELP_MAX_LIBS + 1];
    int ac = 0;
    argv[ac++] = aetherc;
    argv[ac++] = script_path;
    argv[ac++] = sink;
    for (int i = 0; i < g_lib_count; i++) {
        argv[ac++] = "--lib";
        argv[ac++] = g_lib_dirs[i];
    }
    argv[ac] = NULL;

    int rc;
#ifdef _WIN32
    {
        fflush(stderr);
        int saved = _dup(2);
        int fd = _open(err_file, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
        if (fd < 0) { if (saved >= 0) _close(saved); return -1; }
        _dup2(fd, 2);
        _close(fd);
        rc = (int)_spawnv(_P_WAIT, argv[0], (const char* const*)argv);
        if (saved >= 0) { _dup2(saved, 2); _close(saved); }
    }
#else
    {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 2, err_file,
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        extern char** environ;
        pid_t pid;
        if (posix_spawnp(&pid, argv[0], &fa, NULL,
                         (char* const*)argv, environ) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            remove(err_file);
            return -1;
        }
        posix_spawn_file_actions_destroy(&fa);
        int status = 0;
        waitpid(pid, &status, 0);
        rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif

    /* Read the captured stderr back into the caller's buffer. */
    FILE* fp = fopen(err_file, "rb");
    if (fp) {
        size_t total = 0;
        while (total + 1 < buf_size) {
            size_t r = fread(stderr_buf + total, 1, buf_size - 1 - total, fp);
            if (r == 0) break;
            total += r;
        }
        stderr_buf[total] = '\0';
        fclose(fp);
    }
    remove(err_file);
    return rc;
}

/* === Finding parser =================================================
 *
 * aetherc emits errors in the documented form:
 *
 *     error[E0301]: Undefined function 'super_token'
 *       --> script.ae:7:11
 *     7 | super_token("...")
 *       |          ^ help: ...
 *
 * Parse just the header + location lines — that's all we need.
 * Subsequent renderer adds back source context from `sf`. */

static int parse_int(const char** pp) {
    int v = 0;
    while (isdigit((unsigned char)**pp)) {
        v = v * 10 + (**pp - '0');
        (*pp)++;
    }
    return v;
}

static FindingKind kind_from_error_code(int code) {
    switch (code) {
        case 301: return FK_UNDEFINED_FUNC;
        case 300: return FK_UNDEFINED_VAR;
        case 302: return FK_UNDEFINED_TYPE;
        case 303: return FK_NOT_EXPORTED;
        case 304: return FK_HIDDEN_NAME;
        case 200: return FK_TYPE_MISMATCH;
        case 400: return FK_REDEFINITION;
        case 100: return FK_SYNTAX;
        default:  return FK_OTHER;
    }
}

/* Extract the quoted identifier from a typer message like
 * "Undefined function 'super_token'" or
 * "Identifier 'bind' is hidden in this scope". Stops at the closing
 * quote. */
static void extract_quoted_name(const char* msg, char* out, size_t out_size) {
    out[0] = '\0';
    const char* q = strchr(msg, '\'');
    if (!q) return;
    q++;
    const char* q2 = strchr(q, '\'');
    if (!q2) return;
    size_t len = (size_t)(q2 - q);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, q, len);
    out[len] = '\0';
}

static int parse_aetherc_findings(const char* stderr_buf, Finding* findings, int max) {
    int n = 0;
    const char* p = stderr_buf;

    while (*p && n < max) {
        /* Find the next "error[E" header. */
        const char* hdr = strstr(p, "error[E");
        if (!hdr) break;
        p = hdr + 7;
        int code = parse_int(&p);
        if (*p != ']') { continue; }
        p++; /* ] */
        while (*p == ':' || *p == ' ') p++;

        Finding* f = &findings[n];
        memset(f, 0, sizeof(*f));
        f->error_code = code;
        f->kind = kind_from_error_code(code);

        /* Copy the message up to end-of-line, stripping ANSI codes. */
        char raw_msg[AE_HELP_MSG_LEN];
        size_t mi = 0;
        while (*p && *p != '\n' && mi + 1 < sizeof(raw_msg)) {
            if (*p == '\x1b') { /* ANSI escape */
                while (*p && *p != 'm') p++;
                if (*p) p++;
                continue;
            }
            raw_msg[mi++] = *p++;
        }
        raw_msg[mi] = '\0';
        /* Strip trailing whitespace. */
        while (mi > 0 && (raw_msg[mi - 1] == ' ' || raw_msg[mi - 1] == '\t' ||
                          raw_msg[mi - 1] == '\r')) {
            raw_msg[--mi] = '\0';
        }
        safe_strncpy(f->message, raw_msg, sizeof(f->message));
        extract_quoted_name(raw_msg, f->name, sizeof(f->name));

        /* Look for the "  --> file:line:col" location on a nearby line
         * and extract `line` and `col`. Windows MSYS2 paths have
         * three colons (`D:/a/foo.ae:12:5`) — the first belongs to
         * the drive letter, not the line-number separator. POSIX
         * paths have two colons (`/foo/bar.ae:12:5`). The robust
         * shape is "parse from the end, ignore the path": find the
         * line ending, walk back through `\\d+:\\d+`, take those
         * two numbers. Strip trailing CR if present (Windows line
         * endings reach us as `\r\n` in popen output). */
        const char* loc = strstr(p, "-->");
        if (loc) {
            const char* line_end = loc;
            while (*line_end && *line_end != '\n') line_end++;
            const char* tail = line_end;
            while (tail > loc && (tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t')) tail--;
            const char* digit_run_end = tail;
            while (digit_run_end > loc && digit_run_end[-1] >= '0' && digit_run_end[-1] <= '9') digit_run_end--;
            /* digit_run_end now points at first digit of col. */
            if (digit_run_end < tail && digit_run_end > loc && digit_run_end[-1] == ':') {
                const char* line_digit_end = digit_run_end - 1; /* on the `:` */
                const char* line_digit_start = line_digit_end;
                while (line_digit_start > loc && line_digit_start[-1] >= '0' && line_digit_start[-1] <= '9') line_digit_start--;
                if (line_digit_start < line_digit_end && line_digit_start > loc && line_digit_start[-1] == ':') {
                    f->line = parse_int(&line_digit_start);
                    f->col  = parse_int(&digit_run_end);
                }
            }
            p = loc;
        }

        n++;
    }
    return n;
}

/* === Levenshtein =================================================== */

static int levenshtein(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    if (la > 96) la = 96;
    if (lb > 96) lb = 96;
    int prev[97], curr[97];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (tolower((unsigned char)a[i - 1]) ==
                        tolower((unsigned char)b[j - 1])) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int m = del < ins ? del : ins;
            if (sub < m) m = sub;
            curr[j] = m;
        }
        for (int j = 0; j <= lb; j++) prev[j] = curr[j];
    }
    return prev[lb];
}

/* For each unresolved name, find the top-3 closest candidates from
 * the imported modules' export sets (highest priority) and from any
 * stdlib module (lower priority — only included if no import close
 * match). Levenshtein-1 single-candidate triggers a safe-fix. */
static void apply_levenshtein(Finding* f, const ExportEntry* exports, int n_exports,
                              char (*imports)[AE_HELP_NAME_LEN], int n_imports) {
    if (!f->name[0]) return;

    int best_dist[3] = { 99, 99, 99 };
    char best_name[3][AE_HELP_NAME_LEN] = {{0}};
    char best_mod[3][AE_HELP_NAME_LEN]  = {{0}};
    int best_from_import[3] = {0, 0, 0};
    int imported_only = (n_imports > 0);

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n_exports; i++) {
            int is_imported = 0;
            for (int k = 0; k < n_imports; k++) {
                if (strcmp(imports[k], exports[i].module_name) == 0) {
                    is_imported = 1; break;
                }
            }
            if (pass == 0 && imported_only && !is_imported) continue;
            if (pass == 1) {
                if (!imported_only || best_dist[0] <= 2) break;
            }
            int d = levenshtein(f->name, exports[i].export_name);
            if (d > 3) continue;
            /* Insert into top-3. */
            for (int k = 0; k < 3; k++) {
                if (d < best_dist[k]) {
                    for (int m = 2; m > k; m--) {
                        best_dist[m] = best_dist[m - 1];
                        best_from_import[m] = best_from_import[m - 1];
                        safe_strncpy(best_name[m], best_name[m - 1], AE_HELP_NAME_LEN);
                        safe_strncpy(best_mod[m],  best_mod[m - 1],  AE_HELP_NAME_LEN);
                    }
                    best_dist[k] = d;
                    best_from_import[k] = is_imported;
                    safe_strncpy(best_name[k], exports[i].export_name, AE_HELP_NAME_LEN);
                    safe_strncpy(best_mod[k],  exports[i].module_name,  AE_HELP_NAME_LEN);
                    break;
                }
            }
        }
    }

    if (best_dist[0] >= 99) return;

    /* Special case: best match is distance-0 (i.e., the name exists
     * verbatim in stdlib) but the user did NOT import that module —
     * route to missing-import shape instead of "did you mean this
     * same name?". apply_missing_import will pick this up. */
    if (best_dist[0] == 0 && !best_from_import[0]) {
        return;
    }

    /* Render suggestion. */
    char buf[AE_HELP_MSG_LEN];
    if (best_dist[1] >= 99) {
        snprintf(buf, sizeof(buf), "Did you mean: %s (in %s)?",
                 best_name[0], best_mod[0]);
        if (best_dist[0] <= 1) {
            f->safe_fix = 1;
            snprintf(f->fix_replace, sizeof(f->fix_replace), "%s", best_name[0]);
        }
    } else {
        size_t off = (size_t)snprintf(buf, sizeof(buf), "Did you mean one of: ");
        for (int k = 0; k < 3 && best_dist[k] < 99; k++) {
            int w = snprintf(buf + off, sizeof(buf) - off,
                             "%s%s (%s)",
                             k ? ", " : "",
                             best_name[k], best_mod[k]);
            if (w < 0 || (size_t)w >= sizeof(buf) - off) break;
            off += (size_t)w;
        }
    }
    safe_strncpy(f->suggestion, buf, sizeof(f->suggestion));
}

/* === Type-mismatch English ==========================================
 * The typer emits messages of the shape:
 *   "Argument %d of '%s': expected %s, got %s"
 *   "Type mismatch in assignment to '%s': expected %s, got %s"
 *   "Type mismatch in field '%s' of message '%s': expected %s, got %s"
 *
 * Extract the {expected, got} pair and produce a high-precision
 * English suggestion for the common pairs. Anything we can't pattern-
 * match against is left unsuggested — the original typer message is
 * still shown verbatim. Issue #414 capability 3. */
static void apply_type_mismatch_english(Finding* f) {
    if (f->kind != FK_TYPE_MISMATCH || !f->message[0]) return;
    /* Find "expected " and "got " in the message. */
    const char* exp = strstr(f->message, "expected ");
    const char* got = strstr(f->message, "got ");
    if (!exp || !got) return;
    exp += 9; /* past "expected " */
    got += 4; /* past "got " */
    /* Copy the bare type names (stopping at comma / space / end). */
    char expected_type[32];
    char actual_type[32];
    size_t ei = 0, gi = 0;
    while (ei + 1 < sizeof(expected_type) && exp[ei] && exp[ei] != ',' &&
           exp[ei] != ' ' && exp[ei] != '\n' && exp[ei] != '\r') {
        expected_type[ei] = exp[ei]; ei++;
    }
    expected_type[ei] = '\0';
    while (gi + 1 < sizeof(actual_type) && got[gi] && got[gi] != ',' &&
           got[gi] != ' ' && got[gi] != '\n' && got[gi] != '\r' &&
           got[gi] != '.' && got[gi] != ';') {
        actual_type[gi] = got[gi]; gi++;
    }
    actual_type[gi] = '\0';
    if (!expected_type[0] || !actual_type[0]) return;

    /* High-precision pairs first. Anything not recognised falls
     * through to the generic "use <expected> instead of <got>"
     * shape — still more readable than the bare typer message. */
    char buf[AE_HELP_MSG_LEN];
    if (strcmp(expected_type, "int") == 0 &&
        (strcmp(actual_type, "string") == 0 || strcmp(actual_type, "AetherString") == 0)) {
        snprintf(buf, sizeof(buf),
                 "Drop the quotes, pass an integer literal (e.g. `9990`), not a string `\"9990\"`.");
    } else if (strcmp(expected_type, "string") == 0 && strcmp(actual_type, "int") == 0) {
        snprintf(buf, sizeof(buf),
                 "Quote the value, pass `\"%s\"`-shaped string literal, not an integer.", actual_type);
    } else if (strcmp(expected_type, "bool") == 0 && strcmp(actual_type, "int") == 0) {
        snprintf(buf, sizeof(buf),
                 "Use `true` / `false` literals here, not `0`/`1`.");
    } else if (strcmp(expected_type, "int") == 0 && strcmp(actual_type, "bool") == 0) {
        snprintf(buf, sizeof(buf),
                 "Use an integer literal here (1 / 0), not `true` / `false`.");
    } else if (strcmp(expected_type, "float") == 0 && strcmp(actual_type, "int") == 0) {
        snprintf(buf, sizeof(buf),
                 "Use a decimal literal, `9.0` rather than `9`, to force the float type.");
    } else {
        snprintf(buf, sizeof(buf),
                 "Use a `%s` value here, not a `%s`.", expected_type, actual_type);
    }
    safe_strncpy(f->suggestion, buf, sizeof(f->suggestion));
}

/* === Canonical-template surfacing (#414 capability 8) ===============
 * If an imported library ships an `_examples/` directory next to its
 * `module.ae`, list those file paths under findings that reference
 * the library. Convention only — stdlib doesn't ship examples today;
 * library authors can opt in by dropping `.ae` files into
 * `<libroot>/_examples/`. No-op when the directory is absent.
 *
 * For brevity we attach the FIRST example path found per finding —
 * `ae help` is already verbose; users can `ls <libroot>/_examples/`
 * for the full list. Path printed verbatim so it's clickable in any
 * editor that linkifies paths. */
static void apply_canonical_template(Finding* findings, int count,
                                      char (*imports)[AE_HELP_NAME_LEN], int n_imports) {
    char root[AE_HELP_PATH_LEN];
    if (resolve_std_root(root, sizeof(root)) != 0) return;
    for (int i = 0; i < count; i++) {
        Finding* f = &findings[i];
        if (!f->name[0]) continue;
        /* Walk imports; check each for `_examples/`. First match wins. */
        for (int k = 0; k < n_imports; k++) {
            char dir[AE_HELP_PATH_LEN];
            safe_strncpy(dir, imports[k], sizeof(dir));
            for (char* p = dir; *p; p++) {
                if (*p == '.') *p = PATH_SEP;
            }
            char ex_dir[AE_HELP_PATH_LEN];
            if (path_format(ex_dir, sizeof(ex_dir), "%s%c%s%c_examples",
                            root, PATH_SEP, dir, PATH_SEP) != 0) continue;
            DIR* d = opendir(ex_dir);
            if (!d) continue;
            /* Find the first .ae file. */
            struct dirent* e;
            char chosen[AE_HELP_PATH_LEN] = {0};
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                size_t en = strlen(e->d_name);
                if (en < 4 || strcmp(e->d_name + en - 3, ".ae") != 0) continue;
                if (path_format(chosen, sizeof(chosen), "%s%c%s",
                                ex_dir, PATH_SEP, e->d_name) != 0) {
                    chosen[0] = '\0';
                    continue;
                }
                break;
            }
            closedir(d);
            if (chosen[0]) {
                /* Append to suggestion so the canonical path appears
                 * with the existing help text — no extra render path
                 * to keep in sync. */
                char merged[AE_HELP_MSG_LEN];
                if (f->suggestion[0]) {
                    snprintf(merged, sizeof(merged),
                             "%s\n          canonical example: %s", f->suggestion, chosen);
                } else {
                    snprintf(merged, sizeof(merged),
                             "Canonical example for %s: %s", imports[k], chosen);
                }
                safe_strncpy(f->suggestion, merged, sizeof(f->suggestion));
                break;
            }
        }
    }
}

/* === Missing-import detection ====================================== */
/* If the unresolved name matches an export from a module the script
 * has NOT imported, suggest the `import` line. */
static void apply_missing_import(Finding* f, const ExportEntry* exports, int n_exports) {
    if (!f->name[0] || f->suggestion[0]) return; /* Levenshtein already filled it */
    int matches = 0;
    char first_mod[AE_HELP_NAME_LEN] = {0};
    char modules[8][AE_HELP_NAME_LEN] = {{0}};
    for (int i = 0; i < n_exports && matches < 8; i++) {
        if (strcmp(exports[i].export_name, f->name) == 0) {
            int dup = 0;
            for (int k = 0; k < matches; k++) {
                if (strcmp(modules[k], exports[i].module_name) == 0) { dup = 1; break; }
            }
            if (!dup) {
                safe_strncpy(modules[matches], exports[i].module_name, AE_HELP_NAME_LEN);
                if (!first_mod[0]) safe_strncpy(first_mod, exports[i].module_name, AE_HELP_NAME_LEN);
                matches++;
            }
        }
    }
    if (matches == 0) return;
    char buf[AE_HELP_MSG_LEN];
    if (matches == 1) {
        snprintf(buf, sizeof(buf),
                 "'%s' is exported by %s. Add this import near the top:\n    import %s (%s)",
                 f->name, first_mod, first_mod, f->name);
    } else {
        size_t off = (size_t)snprintf(buf, sizeof(buf),
                                       "'%s' is exported by multiple modules. Pick one:", f->name);
        for (int k = 0; k < matches; k++) {
            int w = snprintf(buf + off, sizeof(buf) - off,
                             "\n    import %s (%s)", modules[k], f->name);
            if (w < 0 || (size_t)w >= sizeof(buf) - off) break;
            off += (size_t)w;
        }
    }
    safe_strncpy(f->suggestion, buf, sizeof(f->suggestion));
}

/* === YAML/HCL shape detection ======================================
 * Pure source-line analysis. Walks each source line and matches a
 * small high-precision set of patterns. Only fires inside closure-
 * DSL blocks (lines between a `{` opener and its matching `}`).
 *
 * The block-detection here is a simplification: we treat every `{`
 * on a line starting a builder call (line ends with `{` or with
 * `<ident>(...) {` shape) as opening a DSL block. That matches the
 * common closure-DSL syntax without needing a real parser. False
 * positives are mitigated by the per-pattern checks themselves —
 * `key: int_literal` only fires if `key` is a plausible identifier
 * AND the value is a literal.
 */
static int is_plausible_identifier(const char* s, size_t len) {
    if (len == 0 || len >= AE_HELP_NAME_LEN) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; i < len; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    }
    return 1;
}

/* `line_opens_dsl_block` — is this line opening a closure-DSL block?
 * The discriminator vs a function-definition body opener (`main() {`,
 * `fn foo() {`) is: closure-DSL openers have a `.` member-access in
 * the call (`my_lib.serve {`, `bash.test(b) {`). Function defs are
 * bare names. Conservative: returns true only when we see a
 * `<ident>.<ident>` shape before the first `{`. */
static int line_opens_dsl_block(const char* line) {
    int found_dot_access = 0;
    int saw_brace = 0;
    /* Track if we've seen an ident.ident shape before the `{`. */
    const char* p = line;
    while (*p) {
        if (*p == '{') { saw_brace = 1; break; }
        if (*p == '/' && p[1] == '/') break;
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char* s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            (void)s;
            if (*p == '.') {
                /* Look-ahead: is next char ident-start? Then it's a member access. */
                const char* q = p + 1;
                if (isalpha((unsigned char)*q) || *q == '_') {
                    found_dot_access = 1;
                }
            }
            continue;
        }
        p++;
    }
    return saw_brace && found_dot_access;
}

/* Count net brace delta on a line (ignoring those inside string
 * literals or line comments). */
static int line_brace_delta(const char* line, int* opener_is_dsl) {
    int delta = 0;
    int in_str = 0;
    char quote = 0;
    int saw_open_here = 0;
    for (const char* p = line; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == quote) in_str = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') { in_str = 1; quote = *p; continue; }
        if (*p == '/' && p[1] == '/') break;
        if (*p == '{') { delta++; saw_open_here = 1; }
        else if (*p == '}') delta--;
    }
    if (opener_is_dsl) {
        *opener_is_dsl = (saw_open_here && line_opens_dsl_block(line)) ? 1 : 0;
    }
    return delta;
}

static void apply_yaml_shape_detection(SourceFile* sf, Finding* findings, int* count, int max) {
    /* Block-context stack: each frame says whether the enclosing
     * block is a closure-DSL block or not. YAML/HCL patterns only
     * fire when the innermost frame is a DSL block. */
    int stack_is_dsl[64] = {0};
    int sp = 0;     /* count of frames; stack_is_dsl[sp-1] is top */

    for (int li = 0; li < sf->line_count && *count < max; li++) {
        const char* line = sf->lines[li];
        int opener_is_dsl = 0;

        /* Determine whether the CURRENT line is INSIDE a DSL block,
         * before we update the stack (so an `}` line still belongs
         * to the closing frame for analysis purposes). */
        int in_dsl_now = (sp > 0) && stack_is_dsl[sp - 1];

        int delta = line_brace_delta(line, &opener_is_dsl);
        /* Apply per-character open/close: pushes use the opener flag
         * of this line; closes pop. For lines with mixed open+close,
         * we approximate: a positive delta pushes that many DSL/non-
         * DSL frames based on the line shape; negative pops. */
        if (delta > 0) {
            for (int k = 0; k < delta && sp < (int)(sizeof(stack_is_dsl)/sizeof(stack_is_dsl[0])); k++) {
                stack_is_dsl[sp++] = opener_is_dsl;
            }
        } else if (delta < 0) {
            for (int k = 0; k < -delta && sp > 0; k++) sp--;
        }

        if (!in_dsl_now) continue;

        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || (*p == '/' && p[1] == '/')) continue;

        /* `ident: value` or `ident = value` (with literal-only RHS
         * for v1 — that's the high-precision shape). */
        const char* ident = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        size_t ilen = (size_t)(p - ident);
        if (!is_plausible_identifier(ident, ilen)) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':' && *p != '=') continue;
        char kind_char = *p;
        if (kind_char == ':' && (p[1] == ':' || p[1] == '=')) continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        /* RHS must be a literal (int, string, list) to be high-
         * precision. We accept: digits, double-quote, single-quote,
         * `[`. Anything else (function call, identifier) is rejected
         * to avoid flagging normal Aether expressions. */
        if (!(isdigit((unsigned char)*p) || *p == '-' || *p == '"' || *p == '\'' || *p == '[' ||
              strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0)) {
            continue;
        }
        /* Strip a trailing semicolon from RHS for cleaner suggestion. */
        char rhs[AE_HELP_MSG_LEN];
        safe_strncpy(rhs, p, sizeof(rhs));
        size_t rl = strlen(rhs);
        while (rl > 0 && (rhs[rl - 1] == ';' || rhs[rl - 1] == ' ' || rhs[rl - 1] == '\t')) {
            rhs[--rl] = '\0';
        }

        Finding* f = &findings[(*count)++];
        memset(f, 0, sizeof(*f));
        f->kind = (kind_char == ':') ? FK_YAML_COLON : FK_HCL_EQUAL;
        f->line = li + 1;
        f->col = (int)(ident - line) + 1;
        memcpy(f->name, ident, ilen); f->name[ilen] = '\0';
        snprintf(f->message, sizeof(f->message),
                 "%s on line %d looks like %s; Aether setters use call form",
                 f->name, f->line, kind_char == ':' ? "YAML" : "HCL");
        snprintf(f->suggestion, sizeof(f->suggestion),
                 "Did you mean: %s(%s)?", f->name, rhs);
        char fixed[AE_HELP_MSG_LEN];
        snprintf(fixed, sizeof(fixed), "%.*s%s(%s)",
                 (int)(ident - line), line, f->name, rhs);
        safe_strncpy(f->fix_replace, fixed, sizeof(f->fix_replace));
        f->safe_fix = 1;
    }
}

/* === Top-level closure-DSL detection ===============================
 * Detects a builder-style call at file scope (outside any `main()`
 * or other `fn` definition) where the call is followed by a `{...}`
 * trailing closure. Heuristic: scan top-level lines (depth==0) for
 * `ident.ident(...) {` or `ident(...) {` patterns that aren't `fn`
 * defs. */
static void apply_top_level_dsl(SourceFile* sf, Finding* findings, int* count, int max) {
    int depth = 0;
    for (int li = 0; li < sf->line_count && *count < max; li++) {
        const char* line = sf->lines[li];

        /* Determine if this line was at depth==0 when it began. */
        int line_start_depth = depth;
        for (const char* p = line; *p; p++) {
            if (*p == '{') depth++;
            else if (*p == '}' && depth > 0) depth--;
        }
        if (line_start_depth != 0) continue;

        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '/' || *p == '#') continue;
        /* Skip declarations. */
        if (line_starts_with_keyword(line, "fn") ||
            line_starts_with_keyword(line, "import") ||
            line_starts_with_keyword(line, "actor") ||
            line_starts_with_keyword(line, "struct") ||
            line_starts_with_keyword(line, "enum") ||
            line_starts_with_keyword(line, "const") ||
            line_starts_with_keyword(line, "exports") ||
            line_starts_with_keyword(line, "module") ||
            line_starts_with_keyword(line, "main")) continue;

        /* Look for `<ident>.<ident>` followed (optionally by parens)
         * by `{`. The dot-access discriminates closure-DSL openers
         * from any other top-level construct (function defs start
         * with `fn` or are bare names — already filtered above). */
        const char* ident = p;
        int seen_dot = 0;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '.') {
            if (*p == '.') seen_dot = 1;
            p++;
        }
        if (p == ident || !seen_dot) continue;
        while (*p == ' ' || *p == '\t') p++;
        /* Optional `(args)`. */
        if (*p == '(') {
            int pdepth = 1;
            p++;
            while (*p && pdepth > 0) {
                if (*p == '(') pdepth++;
                else if (*p == ')') pdepth--;
                p++;
            }
            if (pdepth != 0) continue;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (*p != '{') continue;

        /* Found one. */
        size_t ilen = (size_t)((line + (ident - line)) - line); /* placeholder, real below */
        const char* iend = ident;
        while (isalnum((unsigned char)*iend) || *iend == '_' || *iend == '.') iend++;
        ilen = (size_t)(iend - ident);

        Finding* f = &findings[(*count)++];
        memset(f, 0, sizeof(*f));
        f->kind = FK_TOPLEVEL_DSL;
        f->line = li + 1;
        f->col = (int)(ident - line) + 1;
        size_t cp = ilen < sizeof(f->name) - 1 ? ilen : sizeof(f->name) - 1;
        memcpy(f->name, ident, cp); f->name[cp] = '\0';
        snprintf(f->message, sizeof(f->message),
                 "%s { ... } is at top level. Aether scripts run main() as the entry point.",
                 f->name);
        snprintf(f->suggestion, sizeof(f->suggestion),
                 "Wrap the entire block in `main() { ... }` and re-run.");
    }
}

/* === Library-author *.help.md hints ================================
 * Each imported module may ship a `<basename>.help.md` next to its
 * `module.ae`. Format (per docs/cic-help.md):
 *
 *     # Title
 *
 *     ## Section heading
 *
 *     Free-form prose describing the issue.
 *
 *     Pattern: literal-name `bind`, `listen`, `accept`, ...
 *
 * For v1 we support only `Pattern: literal-name X` — match if any
 * source line contains `X(` as a setter/function call. AST predicates
 * are deliberately deferred (issue text: "resist growing it into a
 * rule engine").
 */
static int find_help_md_path(const char* import_name, char* out, size_t out_size) {
    char root[AE_HELP_PATH_LEN];
    if (resolve_std_root(root, sizeof(root)) != 0) return -1;
    /* `std.os` → `<root>/std/os/os.help.md` (basename = last segment). */
    const char* basename = strrchr(import_name, '.');
    basename = basename ? basename + 1 : import_name;
    /* `std.os` → dir `<root>/std/os`. */
    char dir[AE_HELP_PATH_LEN];
    safe_strncpy(dir, import_name, sizeof(dir));
    for (char* p = dir; *p; p++) {
        if (*p == '.') *p = PATH_SEP;
    }
    char probe[AE_HELP_PATH_LEN];
    if (path_format(probe, sizeof(probe), "%s%c%s%c%s.help.md",
                    root, PATH_SEP, dir, PATH_SEP, basename) != 0) {
        return -1;
    }
    struct stat st;
    if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        safe_strncpy(out, probe, out_size);
        return 0;
    }
    /* Not in stdlib — probe each `--lib` directory. A project library
     * resolved via `--lib` ships its hint file the same way stdlib
     * does: `<libdir>/<import-as-dir>/<basename>.help.md`. This is
     * what lets a library author (not just the Aether core team)
     * absorb authoring-mistake knowledge into a shipped hint file. */
    for (int li = 0; li < g_lib_count; li++) {
        if (path_format(probe, sizeof(probe), "%s%c%s%c%s.help.md",
                        g_lib_dirs[li], PATH_SEP, dir, PATH_SEP,
                        basename) != 0) {
            continue;
        }
        if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
            safe_strncpy(out, probe, out_size);
            return 0;
        }
    }
    return -1;
}

static int load_help_md_for_import(const char* import_name, SourceFile* sf,
                                    Finding* findings, int* count, int max) {
    char path[AE_HELP_PATH_LEN];
    if (find_help_md_path(import_name, path, sizeof(path)) != 0) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    char line[AE_HELP_MAX_LINE_LEN];
    char section_title[256] = {0};
    char section_body[AE_HELP_MSG_LEN] = {0};
    char pattern_name[AE_HELP_NAME_LEN] = {0};

    /* Helper macro: when a section finishes (next ## or EOF), if it
     * has a Pattern: name and the script source contains `name(`,
     * emit a finding. */
    while (fgets(line, sizeof(line), f) && *count < max) {
        if (strncmp(line, "## ", 3) == 0) {
            /* Flush previous. */
            if (pattern_name[0]) {
                char needle[AE_HELP_NAME_LEN + 2];
                snprintf(needle, sizeof(needle), "%s(", pattern_name);
                int hit_line = 0;
                for (int li = 0; li < sf->line_count; li++) {
                    if (strstr(sf->lines[li], needle)) { hit_line = li + 1; break; }
                }
                if (hit_line > 0) {
                    Finding* nf = &findings[(*count)++];
                    memset(nf, 0, sizeof(*nf));
                    nf->kind = FK_HELP_MD_HINT;
                    nf->line = hit_line;
                    nf->col = 1;
                    safe_strncpy(nf->name, pattern_name, sizeof(nf->name));
                    snprintf(nf->message, sizeof(nf->message),
                             "%s (%s hint)", section_title, import_name);
                    safe_strncpy(nf->suggestion, section_body, sizeof(nf->suggestion));
                }
            }
            section_title[0] = '\0';
            section_body[0] = '\0';
            pattern_name[0] = '\0';
            /* Capture new title. */
            const char* t = line + 3;
            while (*t == ' ') t++;
            safe_strncpy(section_title, t, sizeof(section_title));
            size_t sl = strlen(section_title);
            while (sl > 0 && (section_title[sl - 1] == '\n' || section_title[sl - 1] == '\r')) {
                section_title[--sl] = '\0';
            }
            continue;
        }
        if (strncmp(line, "Pattern:", 8) == 0) {
            const char* t = line + 8;
            while (*t == ' ' || *t == '\t') t++;
            /* Look for the first backtick-quoted name. */
            const char* tick = strchr(t, '`');
            if (tick) {
                tick++;
                const char* tick2 = strchr(tick, '`');
                if (tick2) {
                    size_t len = (size_t)(tick2 - tick);
                    if (len < sizeof(pattern_name)) {
                        memcpy(pattern_name, tick, len);
                        pattern_name[len] = '\0';
                    }
                }
            }
            continue;
        }
        /* Accumulate body up to size limit. */
        size_t bl = strlen(section_body);
        size_t add = strlen(line);
        if (bl + add + 1 < sizeof(section_body)) {
            memcpy(section_body + bl, line, add);
            section_body[bl + add] = '\0';
        }
    }
    /* Trailing flush. */
    if (pattern_name[0] && *count < max) {
        char needle[AE_HELP_NAME_LEN + 2];
        snprintf(needle, sizeof(needle), "%s(", pattern_name);
        int hit_line = 0;
        for (int li = 0; li < sf->line_count; li++) {
            if (strstr(sf->lines[li], needle)) { hit_line = li + 1; break; }
        }
        if (hit_line > 0) {
            Finding* nf = &findings[(*count)++];
            memset(nf, 0, sizeof(*nf));
            nf->kind = FK_HELP_MD_HINT;
            nf->line = hit_line;
            nf->col = 1;
            safe_strncpy(nf->name, pattern_name, sizeof(nf->name));
            snprintf(nf->message, sizeof(nf->message),
                     "%s (%s hint)", section_title, import_name);
            safe_strncpy(nf->suggestion, section_body, sizeof(nf->suggestion));
        }
    }
    fclose(f);
    return 1;
}

/* === Doc cross-links =============================================== */
static void apply_doc_cross_links(Finding* findings, int count) {
    for (int i = 0; i < count; i++) {
        Finding* f = &findings[i];
        const char* link = NULL;
        switch (f->kind) {
            case FK_UNDEFINED_FUNC:
            case FK_UNDEFINED_VAR:
            case FK_UNDEFINED_TYPE:
            case FK_NOT_EXPORTED:
            case FK_HIDDEN_NAME:
                link = "docs/module-system-design.md (Selective import / exports)";
                break;
            case FK_YAML_COLON:
            case FK_HCL_EQUAL:
            case FK_QUOTED_INT:
            case FK_YAML_LIST:
                link = "docs/closures-and-builder-dsl.md";
                break;
            case FK_TOPLEVEL_DSL:
                link = "docs/config-is-code.md (Script entrypoint)";
                break;
            case FK_HELP_MD_HINT:
                link = "docs/cic-help.md (Library author recipe)";
                break;
            case FK_TYPE_MISMATCH:
                link = "docs/type-system.md";
                break;
            default: break;
        }
        if (link) safe_strncpy(f->doc_link, link, sizeof(f->doc_link));
    }
}

/* === Renderer (human) ============================================== */
static const char* kind_label(FindingKind k) {
    switch (k) {
        case FK_UNDEFINED_FUNC: return "undefined function";
        case FK_UNDEFINED_VAR:  return "undefined variable";
        case FK_UNDEFINED_TYPE: return "undefined type";
        case FK_TYPE_MISMATCH:  return "type mismatch";
        case FK_NOT_EXPORTED:   return "name not exported";
        case FK_HIDDEN_NAME:    return "name hidden in scope";
        case FK_REDEFINITION:   return "redefinition";
        case FK_SYNTAX:         return "syntax error";
        case FK_YAML_COLON:     return "YAML-style colon";
        case FK_HCL_EQUAL:      return "HCL-style equals";
        case FK_QUOTED_INT:     return "quoted integer";
        case FK_YAML_LIST:      return "YAML-style list";
        case FK_TOPLEVEL_DSL:   return "top-level DSL block";
        case FK_HELP_MD_HINT:   return "library hint";
        default:                return "diagnostic";
    }
}

static void render_human(const Finding* findings, int count, const SourceFile* sf, const HelpFlags* flags) {
    (void)flags;
    if (count == 0) {
        printf("ae help: no diagnostics for %s.\n", sf->path);
        return;
    }
    printf("\nae help, %d finding%s for %s\n\n",
           count, count == 1 ? "" : "s", sf->path);
    for (int i = 0; i < count; i++) {
        const Finding* f = &findings[i];
        printf("[%d] %s at %s:%d:%d\n",
               i + 1, kind_label(f->kind), sf->path, f->line, f->col);
        if (f->message[0]) {
            printf("    %s\n", f->message);
        }
        /* Echo the source line with a caret. */
        if (f->line > 0 && f->line <= sf->line_count) {
            printf("    %4d | %s\n", f->line, sf->lines[f->line - 1]);
            if (f->col > 0) {
                printf("         |%*s^\n", f->col, "");
            }
        }
        if (f->suggestion[0]) {
            printf("    help: %s\n", f->suggestion);
        }
        if (f->safe_fix) {
            printf("    (safe to auto-apply with --fix)\n");
        }
        if (f->doc_link[0]) {
            printf("    see:  %s\n", f->doc_link);
        }
        printf("\n");
    }
}

/* === Renderer (JSON) =============================================== */
/* Minimal JSON emitter — no nested objects, all string fields are
 * escaped. */
static void json_escape(const char* in, char* out, size_t out_size) {
    size_t oi = 0;
    for (size_t i = 0; in[i] && oi + 2 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (oi + 3 >= out_size) break;
            out[oi++] = '\\'; out[oi++] = (char)c;
        } else if (c == '\n') {
            if (oi + 3 >= out_size) break;
            out[oi++] = '\\'; out[oi++] = 'n';
        } else if (c == '\r') {
            if (oi + 3 >= out_size) break;
            out[oi++] = '\\'; out[oi++] = 'r';
        } else if (c == '\t') {
            if (oi + 3 >= out_size) break;
            out[oi++] = '\\'; out[oi++] = 't';
        } else if (c < 0x20) {
            if (oi + 7 >= out_size) break;
            int w = snprintf(out + oi, out_size - oi, "\\u%04x", c);
            if (w < 0) break;
            oi += (size_t)w;
        } else {
            out[oi++] = (char)c;
        }
    }
    out[oi] = '\0';
}

static void render_json(const Finding* findings, int count, const SourceFile* sf) {
    char esc_path[AE_HELP_PATH_LEN * 2];
    json_escape(sf->path, esc_path, sizeof(esc_path));
    printf("{\"script\":\"%s\",\"findings\":[", esc_path);
    for (int i = 0; i < count; i++) {
        const Finding* f = &findings[i];
        char emsg[AE_HELP_MSG_LEN * 2];
        char esug[AE_HELP_MSG_LEN * 2];
        char edoc[256];
        json_escape(f->message, emsg, sizeof(emsg));
        json_escape(f->suggestion, esug, sizeof(esug));
        json_escape(f->doc_link, edoc, sizeof(edoc));
        printf("%s{\"kind\":\"%s\",\"line\":%d,\"col\":%d,\"code\":%d,\"name\":\"%s\","
               "\"message\":\"%s\",\"suggestion\":\"%s\",\"safe_fix\":%s,\"doc\":\"%s\"}",
               i ? "," : "",
               kind_label(f->kind), f->line, f->col, f->error_code, f->name,
               emsg, esug, f->safe_fix ? "true" : "false", edoc);
    }
    printf("]}\n");
}

/* === --fix applicator ==============================================
 * Two-pass:
 *   1. Filter findings to safe_fix=1.
 *   2. Print the unified diff (line-by-line).
 *   3. Prompt for confirmation (skipped if --json or stdin is not tty).
 *   4. Apply atomically: write to <path>.aehelp.tmp, rename over.
 */
static int apply_fix(Finding* findings, int count, SourceFile* sf) {
    int n_safe = 0;
    for (int i = 0; i < count; i++) {
        if (findings[i].safe_fix && findings[i].line > 0 && findings[i].line <= sf->line_count) {
            n_safe++;
        }
    }
    if (n_safe == 0) {
        printf("ae help --fix: no safe rewrites available.\n");
        return 0;
    }
    printf("ae help --fix: %d safe rewrite%s to apply.\n\n",
           n_safe, n_safe == 1 ? "" : "s");
    for (int i = 0; i < count; i++) {
        const Finding* f = &findings[i];
        if (!f->safe_fix || f->line <= 0 || f->line > sf->line_count) continue;
        printf("--- %s:%d (before)\n", sf->path, f->line);
        printf("-%s\n", sf->lines[f->line - 1]);
        printf("+++ %s:%d (after)\n", sf->path, f->line);
        printf("+%s\n\n", f->fix_replace);
    }
    /* Confirm. Reject non-TTY stdin so CI / piped-input invocations
     * can't accidentally rewrite files without explicit operator
     * consent. Both POSIX `isatty` and Windows `_isatty` return 0
     * when stdin is a pipe / redirected file — same semantics, the
     * function name differs only in the underscore prefix. */
#ifdef _WIN32
    if (!_isatty(_fileno(stdin))) {
#else
    if (!isatty(fileno(stdin))) {
#endif
        fprintf(stderr, "ae help --fix: stdin is not a TTY; refusing to auto-apply non-interactively.\n");
        return 1;
    }
    printf("Apply these rewrites? [y/N]: ");
    fflush(stdout);
    int ans = getchar();
    if (ans != 'y' && ans != 'Y') {
        printf("Aborted; no changes written.\n");
        return 1;
    }
    /* Apply: rebuild file from line array, replacing matched lines. */
    char tmp[AE_HELP_PATH_LEN];
    if (path_format(tmp, sizeof(tmp), "%s.aehelp.tmp", sf->path) != 0) {
        fprintf(stderr, "ae help --fix: script path is too long to derive a temp filename.\n");
        return 1;
    }
    FILE* out = fopen(tmp, "wb");
    if (!out) {
        fprintf(stderr, "ae help --fix: cannot create %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    for (int li = 0; li < sf->line_count; li++) {
        const char* render = sf->lines[li];
        for (int i = 0; i < count; i++) {
            if (findings[i].safe_fix && findings[i].line == li + 1) {
                render = findings[i].fix_replace;
                break;
            }
        }
        fputs(render, out);
        fputc('\n', out);
    }
    fclose(out);
    if (rename(tmp, sf->path) != 0) {
        fprintf(stderr, "ae help --fix: rename failed: %s\n", strerror(errno));
        unlink(tmp);
        return 1;
    }
    printf("ae help --fix: applied %d rewrite%s to %s.\n",
           n_safe, n_safe == 1 ? "" : "s", sf->path);
    return 0;
}

/* === LLM escalation (#415) =========================================
 * Compile-time gated via AETHER_ENABLE_LLM. When disabled (the
 * default), the stub below returns a clear "rebuild with the flag"
 * message. When enabled, the real path links a llama.cpp shim — the
 * shim lives in `vendor/llama_cpp_shim.c` and is built into ae via
 * the Makefile's `LLM_LDFLAGS` when AETHER_ENABLE_LLM=1.
 *
 * Privacy: the shim only reads the user-supplied weights file and
 * stdin/stdout. No socket-creating code paths exist. */
#ifdef AETHER_ENABLE_LLM
extern int ae_llm_run(const char* weights_path, const char* prompt, FILE* out);
#endif

static int llm_escalate(const Finding* findings, int count,
                         const SourceFile* sf, const char* weights) {
#ifdef AETHER_ENABLE_LLM
    /* Validate weights file exists before we spend any cycles. */
    struct stat st;
    if (stat(weights, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "ae help --llm: weights file not found: %s\n", weights);
        return 1;
    }
    /* Build prompt: findings summary + relevant source lines. */
    char prompt[16384];
    size_t off = 0;
    off += (size_t)snprintf(prompt + off, sizeof(prompt) - off,
        "You are an offline diagnostic helper for the Aether language. "
        "Given the static-analysis findings and source lines below, "
        "explain what the user likely intended and how to fix each "
        "finding. Keep responses concise.\n\n"
        "Script: %s\n\nFindings:\n", sf->path);
    for (int i = 0; i < count && off < sizeof(prompt) - 256; i++) {
        const Finding* f = &findings[i];
        int w = snprintf(prompt + off, sizeof(prompt) - off,
            "  [%d] line %d: %s (%s)\n", i + 1, f->line, f->message, kind_label(f->kind));
        if (w > 0) off += (size_t)w;
    }
    off += (size_t)snprintf(prompt + off, sizeof(prompt) - off, "\nRelevant lines:\n");
    for (int i = 0; i < count && off < sizeof(prompt) - 256; i++) {
        const Finding* f = &findings[i];
        if (f->line > 0 && f->line <= sf->line_count) {
            int w = snprintf(prompt + off, sizeof(prompt) - off,
                "  %d: %s\n", f->line, sf->lines[f->line - 1]);
            if (w > 0) off += (size_t)w;
        }
    }
    printf("\n--- LLM escalation (%s) ---\n", weights);
    int rc = ae_llm_run(weights, prompt, stdout);
    printf("--- end ---\n");
    return rc;
#else
    (void)findings; (void)count; (void)sf; (void)weights;
    fprintf(stderr,
        "ae help --llm: this build was compiled without LLM support.\n"
        "  Rebuild with `AETHER_ENABLE_LLM=1 make ae` to enable.\n"
        "  See docs/build-system.md for details.\n"
        "  Note: the LLM path is opt-in only; heuristic findings above\n"
        "  cover the common cases without it.\n");
    return 1;
#endif
}
