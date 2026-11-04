#include "aether_module.h"
#include "codegen/optimizer.h"   // resolve_when_statements (#2275)
#include "aether_error.h"
#include "aether_strmap.h"
#include "aether_aea.h"
#include "aether_defines.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "analysis/typechecker.h"
#include "analysis/sandbox_trust.h"   // module_sweep_unreachable (#2613)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
    #include <direct.h>    // _getcwd, for the unresolved-import message (#3181)
    #define access _access
    #define F_OK 0
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif
#include <dirent.h>
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#ifdef __APPLE__
    #include <mach-o/dyld.h>
#endif

ModuleRegistry* global_module_registry = NULL;

/* #3545: `fmt` formatted into a string as long as it needs (malloc'd; the
 * caller frees it). A name or a path the merge builds is never cut to a
 * buffer's size, where a cut name is another name and a cut path another
 * file. */
static char* mod_strf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static char* mod_strf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char* s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (!s) {
        fprintf(stderr, "Fatal: out of memory\n");
        exit(1);
    }
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static void names_add(ModuleNames* l, const char* name);
static int names_has(const ModuleNames* l, const char* name);
static void names_free(ModuleNames* l);

/* #3545: the path `fmt` names, malloc'd, when a module file is there, else
 * NULL. Built as long as it is: cut to a buffer's size it named another
 * file, which a probe could then find. */
static char* probe_pathf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static char* probe_pathf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char* s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (!s) {
        fprintf(stderr, "Fatal: out of memory\n");
        exit(1);
    }
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (module_probe(s)) return s;
    free(s);
    return NULL;
}

/* A module path with its dots made slashes (`std.http.client` ->
 * `std/http/client`), malloc'd, as long as it is. */
static char* dots_to_slashes(const char* module_path) {
    char* s = mod_strf("%s", module_path);
    for (char* p = s; *p; p++)
        if (*p == '.') *p = '/';
    return s;
}

/* --- Dependency recording (#1882) -------------------------------------------
 * A build launched with --emit-deps records, in first-seen order, every path
 * the resolver probed (found or absent) and every file it parsed. Deduped by
 * exact path. Kept deliberately simple — a build resolves a few dozen paths,
 * not thousands, so a linear-scan dedupe over a growable array is ample. */
typedef struct { char* path; int found; int is_read; } DepEntry;
static DepEntry* g_deps = NULL;
static int g_dep_count = 0, g_dep_cap = 0;
static int g_dep_recording = 0;
/* Set when a path could not be recorded (out of memory). The manifest is
 * then incomplete, and a key built from it would not change when the
 * missing file does, so module_dep_write writes none (#2537). */
static int g_dep_incomplete = 0;

void module_dep_recording_enable(void) { g_dep_recording = 1; }

static DepEntry* dep_find(const char* path) {
    for (int i = 0; i < g_dep_count; i++)
        if (strcmp(g_deps[i].path, path) == 0) return &g_deps[i];
    return NULL;
}

/* Record one path. `found` = the probe saw it on disk; `is_read` = its CONTENTS
 * were read (a parsed module file), which the manifest content-hashes. A path
 * can be upgraded absent->found and probe->read as more is learned about it. */
static void dep_record(const char* path, int found, int is_read) {
    if (!g_dep_recording || !path || !path[0]) return;
    DepEntry* e = dep_find(path);
    if (e) {
        if (found) e->found = 1;
        if (is_read) e->is_read = 1;
        return;
    }
    /* A dropped entry under-invalidates: an edit to a file the manifest does
     * not name leaves the key as it was. So a failure here poisons the whole
     * manifest rather than losing one line of it. */
    if (g_dep_count == g_dep_cap) {
        int ncap = g_dep_cap ? g_dep_cap * 2 : 32;
        DepEntry* nd = realloc(g_deps, (size_t)ncap * sizeof(DepEntry));
        if (!nd) { g_dep_incomplete = 1; return; }
        g_deps = nd; g_dep_cap = ncap;
    }
    char* copy = strdup(path);
    if (!copy) { g_dep_incomplete = 1; return; }
    g_deps[g_dep_count].path = copy;
    g_deps[g_dep_count].found = found;
    g_deps[g_dep_count].is_read = is_read;
    g_dep_count++;
}

int module_probe(const char* path) {
    int found = (access(path, F_OK) == 0);
    dep_record(path, found, 0);
    return found;
}

void module_dep_record_read(const char* path) { dep_record(path, 1, 1); }

/* On any failure (an incomplete recording, a file that cannot be opened or
 * written in full) there is no manifest at `out_path` afterwards, not even
 * the previous build's: ae then keys on the source tree. A partial or stale
 * manifest would name fewer files than the build read, and an edit to one
 * it leaves out would be served from the cache (#2537). */
int module_dep_write(const char* out_path) {
    if (g_dep_incomplete) {
        remove(out_path);
        return 1;
    }
    FILE* f = fopen(out_path, "w");
    if (!f) {
        remove(out_path);
        return 1;
    }
    /* v1 header lets the reader reject a format it doesn't understand and fall
     * back to the tree hash rather than trust a stale/foreign layout. */
    fprintf(f, "# aether-deps v1\n");
    for (int i = 0; i < g_dep_count; i++) {
        if (g_deps[i].is_read)      fprintf(f, "read %s\n", g_deps[i].path);
        else if (!g_deps[i].found)  fprintf(f, "absent %s\n", g_deps[i].path);
        /* a probed-and-found path that was never parsed (e.g. a directory
         * existence check) is neither read nor absent — its presence is
         * captured by the sibling `absent` lines it would flip, so it needs
         * no line of its own. */
    }
    int failed = ferror(f) != 0;
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        remove(out_path);
        return 1;
    }
    return 0;
}

char* module_resolve_source_directive(ASTNode* c) {
    const char* rel = c->value;
    int absolute = rel[0] == '/' || rel[0] == '\\' ||
                   (((rel[0] >= 'A' && rel[0] <= 'Z') || (rel[0] >= 'a' && rel[0] <= 'z')) &&
                    rel[1] == ':');
    const char* file = c->source_file;
    if (absolute || !file) return strdup(rel);
    const char* cut = NULL;
    for (const char* p = file; *p; p++)
        if (*p == '/' || *p == '\\') cut = p;
    if (!cut) return strdup(rel);
    size_t dlen = (size_t)(cut - file);
    char* out = (char*)malloc(dlen + 1 + strlen(rel) + 1);
    if (!out) return NULL;
    memcpy(out, file, dlen);
    out[dlen] = '/';
    strcpy(out + dlen + 1, rel);
    return out;
}

int module_check_source_directives(ASTNode* ast) {
    int ok = 1;
    for (int i = 0; ast && i < ast->child_count; i++) {
        ASTNode* c = ast->children[i];
        if (c && c->type == AST_C_INCLUDE_DIRECTIVE && c->value) {
            /* #2560: a header the generated C includes is read by the C
             * compiler, not by aetherc, so the dependency manifest has to
             * name it or an edit to it leaves ae's build key unchanged and
             * the cache serves the old object. The copy beside the module,
             * the directory first on the include path (emit_c_include_dirs),
             * is recorded as read; its absence is recorded too, so a header
             * put there later changes the key. One found elsewhere on the
             * include path (a system or SDK header) is not followed. */
            char* path = module_resolve_source_directive(c);
            if (path && module_probe(path)) module_dep_record_read(path);
            free(path);
            continue;
        }
        if (!c || c->type != AST_SOURCE_DIRECTIVE || !c->value) continue;
        char* path = module_resolve_source_directive(c);
        if (!path) continue;
        if (access(path, F_OK) != 0) {
            char msg[1200];
            snprintf(msg, sizeof(msg),
                     "@source(\"%s\"): no such file (looked for %s, relative to the "
                     "directory of the file that names it)", c->value, path);
            AetherError e = { c->source_file, NULL, c->line, c->column, msg,
                              "the path is relative to the module's own directory; "
                              "put the C file beside module.ae or fix the name",
                              NULL, AETHER_ERR_SYNTAX };
            aether_error_report(&e);
            ok = 0;
        } else {
            module_dep_record_read(path);
        }
        free(path);
    }
    return ok;
}

void module_set_source_dir(const char* source_path) {
    module_registry_init();
    /* The directory part of the path, whole (#2543): a 2048-byte copy cut
     * a longer one, and the modules beside the file were not found. Strip
     * the filename, keeping the trailing slash; "" when there is no
     * directory component. */
    size_t len = 0;
    if (source_path) {
        for (size_t i = 0; source_path[i]; i++) {
            if (source_path[i] == '/' || source_path[i] == '\\') len = i + 1;
        }
    }
    char* dir = (char*)malloc(len + 1);
    if (!dir) {
        fprintf(stderr, "aetherc: out of memory recording the source directory\n");
        exit(1);
    }
    if (len) memcpy(dir, source_path, len);
    dir[len] = '\0';
    free(global_module_registry->source_dir);
    global_module_registry->source_dir = dir;
}

void module_add_lib_dir(const char* dir) {
    module_registry_init();
    if (!dir || !dir[0]) return;
    /* Normalise: strip trailing slash so `--lib ./lib/` and
     * `--lib ./lib` resolve to the same entry (dedup catches them)
     * AND the joined lookup path stays clean (`<entry>/<mod>.ae`
     * rather than `./lib//<mod>.ae`). Root paths ("/" on POSIX,
     * "C:\" on Windows) are preserved — stripping their slash
     * would change semantics. ALSO translate MSYS2 POSIX-form
     * (`/d/foo`) to native Windows (`D:/foo`) so a `;`-joined
     * path-list reaches us in the same shape as a sequence of
     * separate `--lib` flags. Both are aether_lib_path_normalize's,
     * which keeps the whole path (#2539). */
    char* norm = aether_lib_path_normalize(dir);
    if (!norm) {
        fprintf(stderr, "aetherc: out of memory adding the --lib directory '%s'\n", dir);
        exit(1);
    }
    /* Skip duplicates so repeated `--lib /same/dir` doesn't waste
     * search slots. O(N) check over a fixed cap-of-8 list — trivial. */
    for (int i = 0; i < global_module_registry->lib_dir_count; i++) {
        if (strcmp(global_module_registry->lib_dirs[i], norm) == 0) { free(norm); return; }
    }
    if (global_module_registry->lib_dir_count >= AETHER_LIB_DIRS_MAX + 1) {
        fprintf(stderr,
            "warning: --lib search path is full (max %d entries); "
            "ignoring '%s'\n", AETHER_LIB_DIRS_MAX, norm);
        free(norm);
        return;
    }
    global_module_registry->lib_dirs[global_module_registry->lib_dir_count++] = norm;
}

void module_add_lib_dirs(const char* spec) {
    if (!spec || !spec[0]) return;
    module_registry_init();
    /* Split on the platform path separator. Each segment is
     * appended via module_add_lib_dir, which normalises, dedupes,
     * and enforces the cap. Empty segments (e.g. trailing `:`,
     * double `::`) are silently skipped — matches Java -cp and
     * PATH semantics. Single source of truth for separator
     * parsing — both `module_set_lib_dir` and aetherc's repeated
     * `--lib` handler route through here. */
    const char* cur = spec;
    while (*cur) {
        const char* next = strchr(cur, AETHER_LIB_PATH_SEP_CHAR);
        size_t len = next ? (size_t)(next - cur) : strlen(cur);
        if (len > 0) {
            char* seg = (char*)malloc(len + 1);
            if (!seg) {
                fprintf(stderr, "aetherc: out of memory reading the --lib path\n");
                exit(1);
            }
            memcpy(seg, cur, len);
            seg[len] = '\0';
            module_add_lib_dir(seg);
            free(seg);
        }
        if (!next) break;
        cur = next + 1;
    }
}

static void module_clear_lib_dirs(void) {
    for (int i = 0; i < global_module_registry->lib_dir_count; i++)
        free(global_module_registry->lib_dirs[i]);
    global_module_registry->lib_dir_count = 0;
}

void module_set_lib_dir(const char* lib_dir) {
    module_registry_init();
    if (!lib_dir || !lib_dir[0]) return;
    /* A fresh `--lib <path>` (or `AETHER_LIB_DIR=<path>`) replaces the
     * directories added before it, not the project's own `lib/`, which
     * stays first (#3504): a program that imports a module of its lib/
     * and one from elsewhere names only the other. The append-form is
     * `module_add_lib_dirs`. */
    module_clear_lib_dirs();
    module_add_lib_dir(AETHER_DEFAULT_LIB_DIR);
    module_add_lib_dirs(lib_dir);
}

// Module management
void module_registry_init(void) {
    if (!global_module_registry) {
        global_module_registry = (ModuleRegistry*)malloc(sizeof(ModuleRegistry));
        global_module_registry->modules = NULL;
        global_module_registry->module_count = 0;
        global_module_registry->module_capacity = 0;
        global_module_registry->source_dir = NULL;
        /* #3181: the entry program's imports are resolved first, so this
         * starts at 0. It was left as malloc gave it: where that was not
         * zero, the entry program's imports looked beside it before the
         * project's lib/ (the order #3103 keeps for a module's own), and a
         * missing one was reported as "beside this module". */
        global_module_registry->source_is_module = 0;
        global_module_registry->lib_dir_count = 0;
        const char* env_lib = getenv("AETHER_LIB_DIR");
        if (env_lib && env_lib[0]) {
            /* Reuse module_set_lib_dir's parser so the env var
             * accepts the same separator-string form as --lib.
             * Issue #413. */
            module_set_lib_dir(env_lib);
        } else {
            module_add_lib_dir(AETHER_DEFAULT_LIB_DIR);
        }
    }
}

static void module_sees_reset(void);
static void module_unreachable_reset(void);

void module_registry_shutdown(void) {
    module_sees_reset();
    module_unreachable_reset();
    if (global_module_registry) {
        for (int i = 0; i < global_module_registry->module_count; i++) {
            module_free(global_module_registry->modules[i]);
        }
        free(global_module_registry->modules);
        module_clear_lib_dirs();
        free(global_module_registry->source_dir);
        free(global_module_registry);
        global_module_registry = NULL;
    }
}

AetherModule* module_create(const char* name, const char* file_path) {
    AetherModule* module = (AetherModule*)malloc(sizeof(AetherModule));
    module->name = strdup(name);
    module->file_path = strdup(file_path);
    module->ast = NULL;
    module->exports = NULL;
    module->export_count = 0;
    module->imports = NULL;
    module->import_count = 0;
    module->ns = NULL;
    module->lane_target = NULL;
    memset(&module->func_names, 0, sizeof(module->func_names));
    memset(&module->const_names, 0, sizeof(module->const_names));
    module->names_ast = NULL;
    module->names_children = 0;
    return module;
}

static void decl_index_drop(const ASTNode* ast);   /* defined below (#3546) */

void module_free(AetherModule* module) {
    if (!module) return;

    free(module->name);
    free(module->file_path);
    free(module->ns);
    free(module->lane_target);
    free(module->func_names.names);
    strmap_free(&module->func_names.index);
    free(module->const_names.names);
    strmap_free(&module->const_names.index);

    if (module->ast) {
        decl_index_drop(module->ast);
        free_ast_node(module->ast);
    }
    
    for (int i = 0; i < module->export_count; i++) {
        free(module->exports[i]);
    }
    free(module->exports);
    
    for (int i = 0; i < module->import_count; i++) {
        free(module->imports[i]);
    }
    free(module->imports);
    
    free(module);
}

// Module registration
void module_register(AetherModule* module) {
    if (!global_module_registry) {
        module_registry_init();
    }
    
    // Check if module already exists
    for (int i = 0; i < global_module_registry->module_count; i++) {
        if (strcmp(global_module_registry->modules[i]->name, module->name) == 0) {
            fprintf(stderr, "Warning: Module '%s' already registered, replacing\n", module->name);
            module_free(global_module_registry->modules[i]);
            global_module_registry->modules[i] = module;
            return;
        }
    }
    
    // Grow array if needed
    if (global_module_registry->module_count >= global_module_registry->module_capacity) {
        int new_capacity = global_module_registry->module_capacity == 0 ? 8 : global_module_registry->module_capacity * 2;
        AetherModule** new_modules = (AetherModule**)aether_xrealloc(
            global_module_registry->modules,
            new_capacity * sizeof(AetherModule*)
        );
        if (!new_modules) return;
        global_module_registry->modules = new_modules;
        global_module_registry->module_capacity = new_capacity;
    }
    
    global_module_registry->modules[global_module_registry->module_count++] = module;
}

AetherModule* module_find(const char* name) {
    if (!global_module_registry) return NULL;
    
    for (int i = 0; i < global_module_registry->module_count; i++) {
        if (strcmp(global_module_registry->modules[i]->name, name) == 0) {
            return global_module_registry->modules[i];
        }
    }
    
    return NULL;
}

static const char* module_last_segment(const char* path);  /* defined below */
static ASTNode* unwrap_export(ASTNode* node);              /* defined below */

static int module_is_shipped(const char* name) {
    return name && (strncmp(name, "std.", 4) == 0 || strncmp(name, "contrib.", 8) == 0);
}

/* #2190 / #2255: does this module need its last segment as its namespace?
 * A module whose externs carry that segment as a prefix does: std.math
 * declares `extern math_floor`, and `math.floor` reaches it as
 * `<ns>_floor`, so under a namespace of `std_math` the call would look
 * for `std_math_floor` and find nothing. A module without such externs
 * (all its functions are Aether, renamed along with the namespace) can
 * take its full path like any other. Without an AST to inspect, a shipped
 * module is assumed to need its leaf and a local one not to. */
static int module_pins_leaf(AetherModule* m) {
    if (!m || !m->name) return 0;
    if (!m->ast) return module_is_shipped(m->name);
    const char* leaf = module_last_segment(m->name);
    size_t n = strlen(leaf);
    for (int i = 0; i < m->ast->child_count; i++) {
        ASTNode* decl = unwrap_export(m->ast->children[i]);
        if (decl && decl->type == AST_EXTERN_FUNCTION && decl->value &&
            strncmp(decl->value, leaf, n) == 0 && decl->value[n] == '_') {
            return 1;
        }
    }
    return 0;
}

/* #2209: two modules whose paths end in the same segment (`mine.vk` and
 * `contrib.vulkan.vk`) used to share the namespace `vk`, so their merged
 * symbols were both `vk_<name>`, `module_find_by_name_or_leaf("vk")` returned
 * whichever loaded first, and the other's exports were reported as missing.
 * A module keeps the short last-segment namespace unless another loaded
 * module ends the same way; then it gets its full path, dots as underscores
 * (`mine_vk`). Written `vk.` / alias prefixes are rewritten to these
 * namespaces by the merger, in the scope that wrote them, so nothing
 * downstream sees the collision.
 *
 * #2190: a module whose externs need its leaf (module_pins_leaf) keeps it
 * whatever collides with it, so when a program's own `pkg.math` collides
 * with std.math, pkg.math is the one that moves. #2255: two pure-Aether
 * modules that happen to share a leaf, such as std.jsonpath.parser and
 * contrib.jq.parser, both move, shipped or not. */
void module_assign_namespaces(void) {
    if (!global_module_registry) return;
    ModuleRegistry* reg = global_module_registry;
    for (int i = 0; i < reg->module_count; i++) {
        AetherModule* m = reg->modules[i];
        if (!m || !m->name) continue;
        const char* leaf = module_last_segment(m->name);
        int shared = 0;
        for (int j = 0; j < reg->module_count && !shared; j++) {
            AetherModule* o = reg->modules[j];
            if (j == i || !o || !o->name) continue;
            shared = strcmp(module_last_segment(o->name), leaf) == 0;
        }
        int renamed = shared && !module_pins_leaf(m);
        free(m->ns);
        m->ns = strdup(renamed ? m->name : leaf);
        if (renamed) {
            for (char* p = m->ns; *p; p++) if (*p == '.') *p = '_';
        }
    }
}

const char* module_namespace_of(const char* module_path) {
    if (!module_path) return "";
    AetherModule* m = module_find(module_path);
    if (m && m->ns) return m->ns;
    return module_last_segment(module_path);
}

AetherModule* module_find_by_namespace(const char* ns) {
    if (!global_module_registry || !ns) return NULL;
    AetherModule* m = module_find(ns);
    if (m) return m;
    ModuleRegistry* reg = global_module_registry;
    for (int i = 0; i < reg->module_count; i++) {
        AetherModule* cand = reg->modules[i];
        if (cand && cand->ns && strcmp(cand->ns, ns) == 0) return cand;
    }
    for (int i = 0; i < reg->module_count; i++) {
        AetherModule* cand = reg->modules[i];
        if (cand && cand->name && strcmp(module_last_segment(cand->name), ns) == 0)
            return cand;
    }
    return NULL;
}

// Import/export handling
void module_add_export(AetherModule* module, const char* symbol) {
    if (!module) return;
    
    // Check if already exported
    for (int i = 0; i < module->export_count; i++) {
        if (strcmp(module->exports[i], symbol) == 0) {
            return;
        }
    }
    
    char** new_exports = (char**)realloc(module->exports, (module->export_count + 1) * sizeof(char*));
    if (!new_exports) return;
    module->exports = new_exports;
    module->exports[module->export_count++] = strdup(symbol);
}

void module_add_import(AetherModule* module, const char* module_name) {
    if (!module) return;
    
    // Check if already imported
    for (int i = 0; i < module->import_count; i++) {
        if (strcmp(module->imports[i], module_name) == 0) {
            return;
        }
    }
    
    char** new_imports = (char**)realloc(module->imports, (module->import_count + 1) * sizeof(char*));
    if (!new_imports) return;
    module->imports = new_imports;
    module->imports[module->import_count++] = strdup(module_name);
}

/* #2614: the namespaces code written in one module may qualify a name with.
 * Asked once per qualified name the checker resolves inside a merged body,
 * and consecutive asks come from the same function, so the last module's
 * list is kept; module_registry_shutdown drops it with the modules. */
static AetherModule* g_sees_module = NULL;
static const char** g_sees_ns = NULL;
static int g_sees_count = 0;

static void module_sees_reset(void) {
    free(g_sees_ns);
    g_sees_ns = NULL;
    g_sees_count = 0;
    g_sees_module = NULL;
}

int module_sees_namespace(const char* module_name, const char* ns) {
    if (!module_name || !ns) return 0;
    if (!g_sees_module || strcmp(g_sees_module->name, module_name) != 0) {
        AetherModule* m = module_find(module_name);
        if (!m) return 0;
        const char** list = malloc(sizeof(const char*) * (size_t)(m->import_count + 1));
        if (!list) return 0;
        int n = 0;
        /* A module names itself (#1780: `audio.x` inside module `audio`). */
        list[n++] = module_namespace_of(m->name);
        for (int i = 0; i < m->import_count; i++) {
            if (m->imports[i]) list[n++] = module_namespace_of(m->imports[i]);
        }
        free(g_sees_ns);
        g_sees_ns = list;
        g_sees_count = n;
        g_sees_module = m;
    }
    for (int i = 0; i < g_sees_count; i++) {
        if (strcmp(g_sees_ns[i], ns) == 0) return 1;
    }
    return 0;
}

/* #2631: the last segment a module file is imported under, which is the
 * namespace a build gives it: `<dir>/module.ae` is `<dir>`, any other
 * `<name>.ae` (a package's own file, `contrib/jq/parser.ae`) is `<name>`. */
const char* module_leaf_of_file(const char* path, char* buf, size_t cap) {
    if (!buf || cap == 0) return "";
    buf[0] = '\0';
    if (!path) return buf;
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t blen = strlen(base);
    if (blen < 4 || strcmp(base + blen - 3, ".ae") != 0) return buf;
    if (strcmp(base, "module.ae") == 0) {
        if (base == path) {
            /* `ae check module.ae` inside the module's directory: the
             * directory is named by the full path. */
            char full[4096];
#ifdef _WIN32
            if (!_fullpath(full, path, sizeof(full))) return buf;
#else
            if (!realpath(path, full)) return buf;
#endif
            if (strcmp(full, path) == 0) return buf;
            return module_leaf_of_file(full, buf, cap);
        }
        const char* end = base - 1;     /* the separator before module.ae */
        const char* start = end;
        while (start > path && start[-1] != '/' && start[-1] != '\\') start--;
        size_t n = (size_t)(end - start);
        if (n == 0 || n >= cap || (n == 1 && start[0] == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.')) return buf;
        memcpy(buf, start, n);
        buf[n] = '\0';
        return buf;
    }
    size_t n = blen - 3;
    if (n >= cap) return buf;
    memcpy(buf, base, n);
    buf[n] = '\0';
    return buf;
}

int module_is_exported(AetherModule* module, const char* symbol) {
    if (!module) return 0;

    for (int i = 0; i < module->export_count; i++) {
        if (strcmp(module->exports[i], symbol) == 0) {
            return 1;
        }
    }

    return 0;
}

/* #2172: does `module` make `name` reachable as `<leaf>.name`? Either it
 * exports `name` itself, or it exports the prefixed C-style spelling
 * `<leaf>_name` that a qualified call resolves to: std.math lists
 * `math_sqrt`, and `math.sqrt` / `import std.math (sqrt)` reach it through
 * that convention. Export enforcement asks this, not module_is_exported. */
int module_exports_symbol(AetherModule* module, const char* name) {
    if (!module || !name) return 0;
    if (module_is_exported(module, name)) return 1;
    if (!module->name) return 0;
    char* prefixed = mod_strf("%s_%s", module_last_segment(module->name), name);
    int exported = module_is_exported(module, prefixed);
    free(prefixed);
    return exported;
}

/* #924 re-export resolution. A module may list, in its `exports`, a symbol
 * it brought in via `import` — `module_resolve_reexport` finds the module
 * that actually DEFINES such a symbol so qualified resolution can redirect
 * `hub.X` to `<origin>_X`.
 *
 * `module` exports `symbol`; if one of its imported modules also exports
 * `symbol`, that import is the origin (re-export). Resolution is transitive
 * (the origin may itself re-export) and cycle-guarded via `depth`. Returns
 * the origin module, or NULL when `module` is itself the definer (no
 * importer exports the name) — the caller then resolves locally as before.
 *
 * Precision: a module that DEFINES `symbol` itself is never treated as
 * re-exporting it (a local definition wins over a same-named import), so a
 * name collision resolves to the local def — the redirect simply doesn't
 * fire. Re-export is also opt-in: `module` must list `symbol` in `exports`. */
static ASTNode* unwrap_export(ASTNode* node);  /* defined below */
static int module_defines_symbol(AetherModule* module, const char* symbol) {
    if (!module || !module->ast) return 0;
    for (int j = 0; j < module->ast->child_count; j++) {
        ASTNode* d = unwrap_export(module->ast->children[j]);
        if (!d || !d->value) continue;
        if ((d->type == AST_FUNCTION_DEFINITION ||
             d->type == AST_BUILDER_FUNCTION ||
             d->type == AST_CONST_DECLARATION) &&
            strcmp(d->value, symbol) == 0) {
            return 1;
        }
    }
    return 0;
}

static AetherModule* resolve_reexport_rec(AetherModule* module,
                                          const char* symbol, int depth) {
    if (!module || depth > 64) return NULL;
    if (!module_is_exported(module, symbol)) return NULL;
    /* A locally-defined export is not a re-export — resolve it in place. */
    if (module_defines_symbol(module, symbol)) return NULL;
    for (int i = 0; i < module->import_count; i++) {
        AetherModule* imp = module_find(module->imports[i]);
        if (!imp || imp == module) continue;
        if (module_is_exported(imp, symbol)) {
            /* Follow transitively: imp might itself be re-exporting. */
            AetherModule* deeper = resolve_reexport_rec(imp, symbol, depth + 1);
            return deeper ? deeper : imp;
        }
    }
    return NULL;
}

AetherModule* module_resolve_reexport(AetherModule* module, const char* symbol) {
    return resolve_reexport_rec(module, symbol, 0);
}

// Package manifest (aether.toml)
PackageManifest* package_manifest_load(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Warning: Could not open package manifest: %s\n", path);
        return NULL;
    }
    
    PackageManifest* manifest = (PackageManifest*)malloc(sizeof(PackageManifest));
    manifest->package_name = NULL;
    manifest->version = NULL;
    manifest->author = NULL;
    manifest->dependencies = NULL;
    manifest->dependency_count = 0;
    
    // Simple TOML parser (basic implementation)
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;
        
        // Parse key = "value"
        char* equals = strchr(line, '=');
        if (equals) {
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            
            // Trim whitespace
            while (*key == ' ') key++;
            while (*value == ' ') value++;
            
            // Remove quotes
            if (*value == '"') {
                value++;
                char* end = strchr(value, '"');
                if (end) *end = '\0';
            }
            
            if (strcmp(key, "name") == 0) {
                manifest->package_name = strdup(value);
            } else if (strcmp(key, "version") == 0) {
                manifest->version = strdup(value);
            } else if (strcmp(key, "author") == 0) {
                manifest->author = strdup(value);
            }
        }
    }
    
    fclose(file);
    return manifest;
}

void package_manifest_free(PackageManifest* manifest) {
    if (!manifest) return;
    
    free(manifest->package_name);
    free(manifest->version);
    free(manifest->author);
    
    for (int i = 0; i < manifest->dependency_count; i++) {
        free(manifest->dependencies[i]);
    }
    free(manifest->dependencies);
    
    free(manifest);
}

// Dependency Graph Implementation

DependencyGraph* dependency_graph_create(void) {
    DependencyGraph* graph = malloc(sizeof(DependencyGraph));
    graph->nodes = NULL;
    graph->node_count = 0;
    return graph;
}

void dependency_graph_free(DependencyGraph* graph) {
    if (!graph) return;
    
    for (int i = 0; i < graph->node_count; i++) {
        DependencyNode* node = graph->nodes[i];
        free(node->module_name);
        free(node->dependencies);
        free(node);
    }
    free(graph->nodes);
    free(graph);
}

DependencyNode* dependency_graph_find_node(DependencyGraph* graph, const char* module_name) {
    if (!graph) return NULL;
    
    for (int i = 0; i < graph->node_count; i++) {
        if (strcmp(graph->nodes[i]->module_name, module_name) == 0) {
            return graph->nodes[i];
        }
    }
    return NULL;
}

DependencyNode* dependency_graph_add_node(DependencyGraph* graph, const char* module_name) {
    if (!graph) return NULL;
    
    // Check if node already exists
    DependencyNode* existing = dependency_graph_find_node(graph, module_name);
    if (existing) return existing;
    
    // Create new node
    DependencyNode* node = malloc(sizeof(DependencyNode));
    node->module_name = strdup(module_name);
    node->dependencies = NULL;
    node->dependency_count = 0;
    node->visited = 0;
    node->in_stack = 0;
    
    // Add to graph
    DependencyNode** new_nodes = realloc(graph->nodes, (graph->node_count + 1) * sizeof(DependencyNode*));
    if (!new_nodes) { free(node); return NULL; }
    graph->nodes = new_nodes;
    graph->nodes[graph->node_count++] = node;
    
    return node;
}

void dependency_graph_add_edge(DependencyGraph* graph, const char* from, const char* to) {
    if (!graph) return;
    
    DependencyNode* from_node = dependency_graph_add_node(graph, from);
    DependencyNode* to_node = dependency_graph_add_node(graph, to);
    
    // Check if edge already exists
    for (int i = 0; i < from_node->dependency_count; i++) {
        if (from_node->dependencies[i] == to_node) {
            return;
        }
    }
    
    // Add edge
    DependencyNode** new_deps = realloc(from_node->dependencies,
                                     (from_node->dependency_count + 1) * sizeof(DependencyNode*));
    if (!new_deps) return;
    from_node->dependencies = new_deps;
    from_node->dependencies[from_node->dependency_count++] = to_node;
}

// DFS helper for cycle detection.
//
// #925: on finding a back edge we capture the ACTUAL cycle — the chain of
// modules from the back-edge target down to the current node — rather than
// reporting the DFS root (which is `__main__`, not a cycle member). `path`
// is the in-stack ancestor list; `path_len` its depth. On a hit we record
// the slice of `path` starting at the back-edge target into `out_cycle`
// (caller-owned, capacity `out_cap`) and return its length via `out_len`.
static int dfs_find_cycle(DependencyNode* node,
                          DependencyNode** path, int path_len,
                          DependencyNode** out_cycle, int out_cap,
                          int* out_len) {
    if (node->in_stack) {
        // Back edge: the cycle is path[start..path_len) + node, where
        // path[start] == node (the module the back edge re-enters).
        int start = 0;
        for (int i = 0; i < path_len; i++) {
            if (path[i] == node) { start = i; break; }
        }
        int n = 0;
        for (int i = start; i < path_len && n < out_cap; i++) {
            out_cycle[n++] = path[i];
        }
        // Close the loop by repeating the entry node (a → b → a).
        if (n < out_cap) out_cycle[n++] = node;
        *out_len = n;
        return 1;
    }

    if (node->visited) {
        // Already checked this node
        return 0;
    }

    node->visited = 1;
    node->in_stack = 1;
    if (path_len < out_cap) path[path_len] = node;

    // Visit all dependencies
    for (int i = 0; i < node->dependency_count; i++) {
        if (dfs_find_cycle(node->dependencies[i], path, path_len + 1,
                           out_cycle, out_cap, out_len)) {
            return 1;
        }
    }

    node->in_stack = 0;
    return 0;
}

int dependency_graph_has_cycle(DependencyGraph* graph) {
    if (!graph || graph->node_count == 0) return 0;

    // Reset visited flags
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i]->visited = 0;
        graph->nodes[i]->in_stack = 0;
    }

    int cap = graph->node_count + 1;
    DependencyNode** path = malloc(sizeof(DependencyNode*) * cap);
    DependencyNode** cycle = malloc(sizeof(DependencyNode*) * cap);
    if (!path || !cycle) { free(path); free(cycle); return 0; }

    // Run DFS from each unvisited node
    int found = 0;
    for (int i = 0; i < graph->node_count && !found; i++) {
        if (!graph->nodes[i]->visited) {
            int cycle_len = 0;
            if (dfs_find_cycle(graph->nodes[i], path, 0, cycle, cap, &cycle_len)) {
                found = 1;
                /* #925: name the modules in the cycle, in order, instead of
                 * the bogus `involving module '__main__'`. Skip a leading
                 * `__main__` if present (it's the synthetic entry root, not
                 * a user module — it can't be part of a real import cycle,
                 * but a defensive slice keeps the message clean). */
                char msg[512];
                int off = snprintf(msg, sizeof(msg),
                                   "circular import dependency: ");
                for (int c = 0; c < cycle_len && off < (int)sizeof(msg) - 8; c++) {
                    const char* nm = cycle[c]->module_name
                                     ? cycle[c]->module_name : "?";
                    off += snprintf(msg + off, sizeof(msg) - off,
                                    "%s%s", (c ? " -> " : ""), nm);
                }
                aether_error_simple(msg, 0, 0);
            }
        }
    }

    free(path);
    free(cycle);
    return found;
}

// --- Module Orchestration ---

static const char* get_user_home_dir(void) {
    const char* h = getenv("HOME");
    if (h && h[0]) return h;
#ifdef _WIN32
    h = getenv("USERPROFILE");
    if (h && h[0]) return h;
    h = getenv("LOCALAPPDATA");
    if (h && h[0]) return h;
#endif
    return NULL;
}

static int get_exe_directory(char* buf, size_t bufsz) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
    if (n == 0 || n >= bufsz) return 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)bufsz;
    if (_NSGetExecutablePath(buf, &sz) != 0) return 0;
#elif defined(__FreeBSD__)
    /* #1586: /proc/self/exe is Linux-only; KERN_PROC_PATHNAME is the
     * mount-nothing FreeBSD primitive (pid -1 = current process). */
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t fb_len = bufsz;
    if (sysctl(mib, 4, buf, &fb_len, NULL, 0) != 0 || fb_len <= 1) {
        ssize_t pn = readlink("/proc/curproc/file", buf, bufsz - 1);
        if (pn <= 0 || (size_t)pn >= bufsz - 1) return 0;
        buf[pn] = '\0';
    }
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, bufsz - 1);
    if (n <= 0 || (size_t)n >= bufsz - 1) return 0;   /* #3545: filled, it may be cut */
    buf[n] = '\0';
#else
    return 0;
#endif
    char* last_sep = strrchr(buf, '/');
#ifdef _WIN32
    char* last_bsep = strrchr(buf, '\\');
    if (!last_sep || (last_bsep && last_bsep > last_sep)) last_sep = last_bsep;
#endif
    if (last_sep) *last_sep = '\0';
    else return 0;
    return 1;
}

// Walk the standard install-prefix search order looking for
// `<root>/<converted>/module.ae`, where root is "std" or "contrib".
// All toolchain-bundled module trees (std/, contrib/) live under
// the same `share/aether/<root>/` prefix once installed, and use the
// same five-tier discovery: CWD-local → AETHER_HOME → exe-relative
// (handles ~/.local, /usr/local, custom prefixes uniformly) →
// ~/.aether → /usr/local. Factored out of module_resolve_stdlib_path
// so contrib resolves the same way.
static char* resolve_pkg_path(const char* root, const char* converted) {
    char* hit_path = NULL;

    // Try 1: Local development path (relative to CWD)
    if ((hit_path = probe_pathf("%s/%s/module.ae", root, converted))) return hit_path;

    // Try 2: Installed path via AETHER_HOME
    const char* aether_home = getenv("AETHER_HOME");
    if (aether_home && aether_home[0]) {
        if ((hit_path = probe_pathf("%s/share/aether/%s/%s/module.ae", aether_home, root, converted))) return hit_path;
        if ((hit_path = probe_pathf("%s/%s/%s/module.ae", aether_home, root, converted))) return hit_path;
    }

    // Try 3: Relative to the running aetherc binary. This is the
    // canonical lookup for installed downstream consumers — `ae
    // build` invokes aetherc from $prefix/bin/, so $exe_dir/../
    // points at the install prefix and ../share/aether/<root>/
    // finds the module.ae regardless of which prefix was used at
    // install time.
    char exe_dir[4096];   /* the OS bounds it; a path past it is refused (get_exe_directory) */
    if (get_exe_directory(exe_dir, sizeof(exe_dir))) {
        if ((hit_path = probe_pathf("%s/../share/aether/%s/%s/module.ae", exe_dir, root, converted))) return hit_path;
        if ((hit_path = probe_pathf("%s/../%s/%s/module.ae", exe_dir, root, converted))) return hit_path;
        if ((hit_path = probe_pathf("%s/../lib/%s/%s/module.ae", exe_dir, root, converted))) return hit_path;
    }

    // Try 4: User home directory (~/.aether)
    const char* home = get_user_home_dir();
    if (home && home[0]) {
        if ((hit_path = probe_pathf("%s/.aether/share/aether/%s/%s/module.ae", home, root, converted))) return hit_path;
    }

#ifndef _WIN32
    // Try 5: System install locations (POSIX only)
    if ((hit_path = probe_pathf("/usr/local/share/aether/%s/%s/module.ae", root, converted))) return hit_path;
#endif

    return NULL;
}

char* module_resolve_stdlib_path(const char* module_name) {
    // Convert dots to slashes so `std.http.client` resolves to
    // `std/http/client/module.ae` rather than the literal `std/http.client/`
    // (which would never exist on disk). Mirrors what the local-path
    // resolver below already does for non-stdlib imports. Without
    // this, only single-component stdlib modules (`std.fs`, `std.io`)
    // resolved; nested ones like `std.http.client` failed even when
    // the file was present at the expected path.
    char* converted = dots_to_slashes(module_name);
    char* found = resolve_pkg_path("std", converted);
    free(converted);
    return found;
}

char* module_resolve_contrib_path(const char* module_name) {
    // Same dot-to-slash conversion as the stdlib resolver — required
    // for nested contrib modules like `contrib.host.python` →
    // `contrib/host/python/module.ae`.
    char* converted = dots_to_slashes(module_name);
    char* found = resolve_pkg_path("contrib", converted);
    free(converted);
    return found;
}

/* Order matches by full path. Every match shares the same
 * ~/.aether/packages prefix, so this orders by "<host>/<owner>"
 * without copying either component into a fixed-size buffer
 * (d_name is 256 bytes on glibc, 260 on MinGW, 1024 on macOS, so
 * any hardcoded join buffer is wrong on some platform). */
static int pkg_match_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Probe one installed-package root for the module entry point.
 * `sub_path` is the import path after the package name (leading '/'),
 * or NULL when the import names the package itself. Returns a
 * malloc'd path on a hit, NULL otherwise. */
static char* pkg_probe_root(const char* root, const char* sub_path) {
    char* hit_path = NULL;
    if (sub_path) {
        if ((hit_path = probe_pathf("%s/src%s/module.ae", root, sub_path))) return hit_path;
        if ((hit_path = probe_pathf("%s/src%s.ae", root, sub_path))) return hit_path;
        if ((hit_path = probe_pathf("%s/lib%s/module.ae", root, sub_path))) return hit_path;
        if ((hit_path = probe_pathf("%s%s/module.ae", root, sub_path))) return hit_path;
        if ((hit_path = probe_pathf("%s%s.ae", root, sub_path))) return hit_path;
    } else {
        if ((hit_path = probe_pathf("%s/src/module.ae", root))) return hit_path;
        if ((hit_path = probe_pathf("%s/module.ae", root))) return hit_path;
    }
    return NULL;
}

// Resolve a local module path (e.g., "mypackage.utils") to a file path.
/* The module `converted` (a path with slashes) under `base`, the importing
 * file's directory with its trailing slash, in the shapes and the lib path
 * order the CWD-relative probes use (Issue #413). */
static char* resolve_under_source_dir(const char* base, const char* converted) {
    char* hit_path = NULL;
    for (int li = 0; li < global_module_registry->lib_dir_count; li++) {
        const char* lib = global_module_registry->lib_dirs[li];
        if ((hit_path = probe_pathf("%s%s/%s/module.ae", base, lib, converted))) return hit_path;
        if ((hit_path = probe_pathf("%s%s/%s.ae", base, lib, converted))) return hit_path;
    }
    if ((hit_path = probe_pathf("%ssrc/%s/module.ae", base, converted))) return hit_path;
    if ((hit_path = probe_pathf("%ssrc/%s.ae", base, converted))) return hit_path;
    if ((hit_path = probe_pathf("%s%s/module.ae", base, converted))) return hit_path;
    if ((hit_path = probe_pathf("%s%s.ae", base, converted))) return hit_path;
    return NULL;
}

static char* resolve_local_converted(const char* converted);

char* module_resolve_local_path(const char* module_path) {
    char* converted = dots_to_slashes(module_path);
    char* found = resolve_local_converted(converted);
    free(converted);
    return found;
}

/* The local module whose path, its dots made slashes, is `converted`. */
static char* resolve_local_converted(const char* converted) {
    char* hit_path = NULL;

    /* #3103: a module's own import names a file beside it first. Tried after
     * the CWD, std.jsonpath's `import parser` found contrib/jq/parser.ae
     * whenever the compiler ran in contrib/jq, and a program importing
     * both failed there ("'free_query' is not exported from module
     * 'parser'"). The entry program keeps the CWD (the project root) first. */
    const char* src_dir = global_module_registry->source_dir;
    if (global_module_registry->source_is_module && src_dir && src_dir[0]) {
        char* found = resolve_under_source_dir(src_dir, converted);
        if (found) return found;
    }

    /* Try 1/2: search every CWD-relative lib_dirs[] entry in order,
     * first hit wins. Each entry probes both shapes (`<entry>/
     * <module>/module.ae` then `<entry>/<module>.ae`) so the
     * historical resolution semantics for any single entry are
     * preserved — what's new is the loop over multiple roots.
     * Issue #413. */
    for (int li = 0; li < global_module_registry->lib_dir_count; li++) {
        const char* lib = global_module_registry->lib_dirs[li];
        if ((hit_path = probe_pathf("%s/%s/module.ae", lib, converted))) return hit_path;
        if ((hit_path = probe_pathf("%s/%s.ae", lib, converted))) return hit_path;
    }

    // Try 3: src/module_path/module.ae
    if ((hit_path = probe_pathf("src/%s/module.ae", converted))) return hit_path;

    // Try 4: src/module_path.ae
    if ((hit_path = probe_pathf("src/%s.ae", converted))) return hit_path;

    // Try 5: module_path/module.ae (project root)
    if ((hit_path = probe_pathf("%s/module.ae", converted))) return hit_path;

    // Try 6: module_path.ae (single file in root)
    if ((hit_path = probe_pathf("%s.ae", converted))) return hit_path;

    // Try 6b: Search relative to source file directory
    if (src_dir && src_dir[0] && !global_module_registry->source_is_module) {
        char* found = resolve_under_source_dir(src_dir, converted);
        if (found) return found;
    }

    // Try 7-9: Search installed packages at ~/.aether/packages/
    // import mylib.utils → search ~/.aether/packages/*/mylib/src/utils/module.ae
    // The first path component (mylib) is the package name
    {
        const char* home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");  // Windows
        if (home) {
            /* The package name (the first component of the module path) and
             * every path under the package directory, as long as they are
             * (#3545: they were cut to fixed buffers, so a long name probed
             * another package's path). */
            const char* slash0 = strchr(converted, '/');
            char* pkg_name = slash0 ? mod_strf("%.*s", (int)(slash0 - converted), converted)
                                    : mod_strf("%s", converted);

            // The sub-path within the package (everything after package name)
            const char* sub_path = strchr(converted, '/');

            char* pkg_base = mod_strf("%s/.aether/packages", home);

            // Flat layout: ~/.aether/packages/<pkg_name>/
            char* root = mod_strf("%s/%s", pkg_base, pkg_name);
            char* hit = pkg_probe_root(root, sub_path);
            free(root);
            if (hit) {
                free(pkg_name);
                free(pkg_base);
                return hit;
            }

            /* Host-nested layout: ~/.aether/packages/<host>/<owner>/<pkg_name>/,
             * which is what `ae add <host>/<owner>/<repo>` and `apkg install`
             * create. Without this scan an installed package is unreachable:
             * `import repo` only ever probed the flat path above.
             *
             * CRITICAL: collect every match rather than returning the first.
             * readdir order is unspecified (ext4 hashes entries, APFS makes
             * no guarantee), so returning the first hit would let the same
             * source resolve to a different package on another machine. We
             * sort by "<host>/<owner>" for a reproducible result and refuse
             * to guess when two owners ship the same package name. Every
             * match is kept, however many (#3545: 16 were). */
            char** matches = NULL;
            int match_count = 0, match_cap = 0;
            size_t base_len = strlen(pkg_base);

            DIR* d_host = opendir(pkg_base);
            if (d_host) {
                struct dirent* e_host;
                while ((e_host = readdir(d_host)) != NULL) {
                    if (e_host->d_name[0] == '.') continue;
                    char* host_dir = mod_strf("%s/%s", pkg_base, e_host->d_name);
                    DIR* d_owner = opendir(host_dir);
                    if (!d_owner) {
                        free(host_dir);
                        continue;
                    }
                    struct dirent* e_owner;
                    while ((e_owner = readdir(d_owner)) != NULL) {
                        if (e_owner->d_name[0] == '.') continue;
                        char* owner_root = mod_strf("%s/%s/%s", host_dir, e_owner->d_name, pkg_name);
                        char* found = pkg_probe_root(owner_root, sub_path);
                        free(owner_root);
                        if (!found) continue;
                        if (match_count == match_cap) {
                            match_cap = match_cap ? match_cap * 2 : 4;
                            matches = aether_xrealloc(matches, (size_t)match_cap * sizeof(*matches));
                        }
                        matches[match_count++] = found;
                    }
                    closedir(d_owner);
                    free(host_dir);
                }
                closedir(d_host);
            }

            if (match_count > 0) {
                qsort(matches, (size_t)match_count, sizeof(matches[0]),
                      pkg_match_cmp);

                if (match_count > 1) {
                    /* Two owners ship a package with this name. Picking one
                     * silently would link against an arbitrary dependency, so
                     * report it and let the user disambiguate. Each owner is
                     * printed straight out of its match path. */
                    char* msg = mod_strf("ambiguous package import '%s': installed under ",
                                         pkg_name);
                    for (int m = 0; m < match_count; m++) {
                        const char* rel = matches[m] + base_len + 1;
                        const char* host_end = strchr(rel, '/');
                        const char* owner_end = host_end ? strchr(host_end + 1, '/') : NULL;
                        int owner_len = owner_end ? (int)(owner_end - rel)
                                                  : (int)strlen(rel);
                        char* longer = mod_strf("%s%s%.*s", msg, (m ? ", " : ""), owner_len, rel);
                        free(msg);
                        msg = longer;
                    }
                    char* whole = mod_strf("%s. Remove the duplicate from ~/.aether/packages.", msg);
                    aether_error_simple(whole, 0, 0);
                    free(whole);
                    free(msg);
                    for (int m = 0; m < match_count; m++) free(matches[m]);
                    free(matches);
                    free(pkg_name);
                    free(pkg_base);
                    return NULL;
                }

                char* only = matches[0];
                free(matches);
                free(pkg_name);
                free(pkg_base);
                return only;
            }
            free(matches);
            free(pkg_name);
            free(pkg_base);
        }
    }

    return NULL;
}

/* Reads a whole file. Returns a malloc'd, NUL-terminated buffer and sets
 * *len, or NULL when the file cannot be read. `mode` is "r" for source text
 * (Windows folds CRLF, as the lexer has always seen it) and "rb" for an
 * artifact, whose payload length and hash count raw bytes. */
static char* module_read_file(const char* path, const char* mode, size_t* len) {
    FILE* f = fopen(path, mode);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* CRITICAL: ftell reports -1 on an unseekable stream (a FIFO, a
     * character device). Falling through would malloc(0) and then hand
     * fread a size_t-widened SIZE_MAX, writing past a zero-byte buffer. */
    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char* data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t bytes_read = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[bytes_read] = '\0';
    *len = bytes_read;
    return data;
}

#ifndef AETHER_VERSION
#define AETHER_VERSION "0.0.0-dev"
#endif

static int env_flag(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* #1746: an installed module may carry a compiled artifact beside its
 * source. Returns the artifact's AST when there is one and it was built from
 * exactly this source by this front end with the same `defined(...)`
 * answers; NULL sends the caller to the ordinary parse. Nothing here is an
 * error: a missing, stale or damaged artifact only costs the parse it would
 * have saved. AETHER_NO_AEA=1 turns artifacts off; AETHER_AEA_TRACE=1 says
 * on stderr which modules came from an artifact and why others did not. */
static ASTNode* module_load_artifact(const char* file_path, const char* source,
                                     size_t source_len) {
    if (env_flag("AETHER_NO_AEA")) return NULL;
    char* source_rel = NULL;
    char* artifact = aea_artifact_path_for(file_path, &source_rel);
    if (!artifact) return NULL;

    ASTNode* ast = NULL;
    /* Probed, so a build cache that saw no artifact is invalidated when one
     * is installed later. */
    if (module_probe(artifact)) {
        size_t len = 0;
        char* data = module_read_file(artifact, "rb", &len);
        if (data) {
            module_dep_record_read(artifact);
            AeaConsumer consumer = {
                AETHER_VERSION, aea_frontend_id(), source_rel,
                source, source_len, aether_define_is_set
            };
            const char* why = NULL;
            ast = aea_decode(data, len, &consumer, &why);
            if (env_flag("AETHER_AEA_TRACE")) {
                if (ast) fprintf(stderr, "aea: using %s\n", artifact);
                else     fprintf(stderr, "aea: not using %s: %s\n", artifact, why ? why : "?");
            }
            free(data);
        }
    }
    free(artifact);
    free(source_rel);
    return ast;
}

/* Lexes and parses `source` as the module at `file_path`. Saves/restores
 * the global lexer state. */
static ASTNode* module_parse_source(const char* file_path, char* source) {
    // Save lexer state (lexer is global)
    LexerState saved;
    lexer_save(&saved);

    // Re-point the diagnostic source context at THIS module for the
    // duration of its parse, so any parse error names the module's own
    // file and renders from the module's own buffer — not the importing
    // file's, which would mislabel the location and print an unrelated
    // source snippet (#646). Restored before `source` is freed by the
    // caller so the context never dangles; nested imports save/restore
    // correctly because each call frame keeps its caller's context on the
    // stack.
    const char* saved_err_filename = NULL;
    const char* saved_err_source = NULL;
    aether_error_get_source(&saved_err_filename, &saved_err_source);
    aether_error_set_source(file_path, source);

    int token_count = 0;
    Token** tokens = lexer_tokenize(source, &token_count);
    if (!tokens) {
        aether_error_set_source(saved_err_filename, saved_err_source);
        lexer_restore(&saved);
        return NULL;
    }
    /* A character the lexer stops at is reported as the entry file's is
     * (compile_source): its message names it. Parsed on, the module's
     * tokens ended at it and the parser reported "Expected statement in
     * block" there instead. */
    Token* last = tokens[token_count - 1];
    if (last->type == TOKEN_ERROR) {
        aether_error_full(last->value, last->line, last->column,
                          lexer_error_help(last->value), NULL, AETHER_ERR_SYNTAX);
        aether_error_set_source(saved_err_filename, saved_err_source);
        free_tokens(tokens, token_count);
        lexer_restore(&saved);
        return NULL;
    }

    Parser* parser = create_parser(tokens, token_count);
    ASTNode* ast = parse_program(parser);

    aether_error_set_source(saved_err_filename, saved_err_source);
    free_tokens(tokens, token_count);
    free_parser(parser);
    lexer_restore(&saved);
    return ast;
}

char* module_build_artifact(const char* source_path, const char* source_rel,
                            size_t* out_len, const char** err) {
    static const char* unused;
    if (!err) err = &unused;
    *err = NULL;
    size_t size = 0;
    char* source = module_read_file(source_path, "r", &size);
    if (!source) { *err = "cannot read the source file"; return NULL; }

    int errors_before = aether_error_count();
    int warnings_before = aether_warning_count();
    aether_define_record_queries(1);
    ASTNode* ast = module_parse_source(source_path, source);
    aether_define_record_queries(0);

    char* out = NULL;
    if (!ast || aether_error_count() != errors_before) {
        *err = "the module does not parse";
    } else if (aether_warning_count() != warnings_before) {
        /* An importer of the artifact would not see these warnings, so the
         * artifact would not compile the same as the source. */
        *err = "the module parses with warnings";
    } else if (aether_define_query_overflowed()) {
        *err = "the module tests more build symbols than an artifact records";
    } else {
        int n = aether_define_query_count();
        AeaDefine* defines = n ? calloc((size_t)n, sizeof(AeaDefine)) : NULL;
        if (n && !defines) {
            *err = "out of memory";
        } else {
            for (int i = 0; i < n; i++) {
                defines[i].name = aether_define_query_name(i);
                defines[i].value = aether_define_query_value(i);
            }
            ast_stamp_source_file(ast, source_path);
            AeaProducer producer = {
                AETHER_VERSION, aea_frontend_id(), source_rel,
                source, size, defines, n
            };
            out = aea_encode(ast, &producer, out_len, err);
            free(defines);
        }
    }
    free_ast_node(ast);
    free(source);
    return out;
}

// Parse a module file into an AST, from its compiled artifact when a
// compatible one is installed beside it (#1746), else from the text.
ASTNode* module_parse_file(const char* file_path) {
    size_t size = 0;
    char* source = module_read_file(file_path, "r", &size);
    if (!source) return NULL;
    /* This file's CONTENTS drive codegen, so a `read` entry (content-hashed on
     * a warm run) is the correct dependency, upgrading whatever `probe` the
     * resolver already recorded for it. */
    module_dep_record_read(file_path);

    ASTNode* ast = module_load_artifact(file_path, source, size);
    if (!ast) ast = module_parse_source(file_path, source);

    // Stamp every node with the source path before this AST gets
    // cloned into the merged program. The clone preserves
    // source_file, so codegen can emit `#line N "path"` directives
    // pointing at the right .ae file even after module merging. An
    // artifact's AST carries no path, so it gets the same one a parse of
    // this file would have.
    if (ast) ast_stamp_source_file(ast, file_path);

    // #2275: resolve the module's compile-time `when`s now, as the entry
    // file's are resolved. A top-level `when` is not a declaration the merge
    // copies, so its surviving arm (the externs, functions and consts it
    // declares for this platform) never reached the program, and the
    // module's own functions reported them undefined. Resolved here, the
    // arm's declarations are the module's top level for every later step:
    // its imports, its exports, the merge. A condition that is not a
    // compile-time constant is reported against this file and fails the
    // build with the module's other errors (see the check after
    // module_orchestrate).
    //
    // Tokenizing and parser cleanup now live in module_parse_source (#1746),
    // so this function frees only `source`; the error-source is saved and
    // restored here rather than by an outer scope.
    if (ast) {
        const char* saved_err_filename = NULL;
        const char* saved_err_source = NULL;
        aether_error_get_source(&saved_err_filename, &saved_err_source);
        aether_error_set_source(file_path, source);
        resolve_when_statements(ast);
        aether_error_set_source(saved_err_filename, saved_err_source);
    }

    free(source);
    return ast;
}

// Try to resolve an import path. If the full path fails to resolve as a
// module, split at the last dot and treat the tail as a selective-import
// symbol (Java-style `import java.util.List` — class name tacked on the
// end). On success with split, mutates the import_node in place: shortens
// `value` to the module path and inserts the symbol name as an
// AST_IDENTIFIER first child (matching the shape of the parenthesised
// `import mod (a, b)` selective form). Caller owns the returned file
// path on success; returns NULL on failure.
/* #1858: an import that resolves to nothing used to be accepted silently, so a
 * typo stayed invisible until a call through the namespace failed somewhere
 * else entirely. Worse for a module exporting only constants or types, whose
 * uses can resolve through other paths, letting a misspelt import go unnoticed
 * for good. Report it where it is written, and say where we looked. */
/* A few std namespaces are provided by the compiler itself and have no module
 * directory to resolve to, so "did not resolve to a file" is normal for them
 * and must not be reported. The list is small and was derived by scanning
 * every `import std.X` in the tree against std/X: std.heap is the only one
 * without a directory (heap.new / heap.free are lowered directly by the parser
 * and codegen). Re-run that scan if another builtin namespace appears. */
static int import_is_compiler_builtin(const char* name) {
    static const char* const builtins[] = { "std.heap" };
    if (!name) return 0;
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(name, builtins[i]) == 0) return 1;
    }
    return 0;
}

/* Append `text` to a message, cut at its end rather than overrunning it. */
static void msg_append(char* msg, size_t size, const char* text) {
    size_t used = strlen(msg);
    if (!text || used + 1 >= size) return;
    snprintf(msg + used, size - used, "%s", text);
}

/* Append a searched directory to the message, as an absolute path: `dir`
 * itself when it is one, else `dir` under `here`, the directory the compile
 * ran in. Then `note` (" (beside this module)", or ""), and a comma. */
static void msg_append_dir(char* msg, size_t size, const char* here, const char* dir,
                           const char* note) {
    /* Without the trailing slash source_dir keeps. */
    char bare[1024];
    snprintf(bare, sizeof(bare), "%s", dir);
    size_t n = strlen(bare);
    while (n > 1 && (bare[n - 1] == '/' || bare[n - 1] == '\\')) bare[--n] = '\0';
    int absolute = bare[0] == '/' || bare[0] == '\\' || (bare[0] && bare[1] == ':');
    if (!absolute) {
        msg_append(msg, size, here);
        if (strcmp(bare, ".") != 0) {
            msg_append(msg, size, "/");
            msg_append(msg, size, bare);
        }
    } else {
        msg_append(msg, size, bare);
    }
    msg_append(msg, size, note);
    msg_append(msg, size, ", ");
}

static void report_unresolved_import(ASTNode* import_node) {
    const char* name = import_node && import_node->value ? import_node->value : "?";
    if (import_is_compiler_builtin(name)) return;
    char msg[2048];
    if (strncmp(name, "std.", 4) == 0 || strncmp(name, "contrib.", 8) == 0) {
        snprintf(msg, sizeof(msg),
                 "unresolved import '%s': no module of that name was found in %s", name,
                 name[0] == 's' ? "the standard library" : "contrib");
    } else {
        /* #3181: the directories module_resolve_local_path tried, in its
         * order, each spelt in full. "src/, the project root" said nothing
         * of where that root was, so a search made from the wrong directory
         * (`ae run app/` from outside app/ compiled from the caller's) read
         * as a module that does not exist. */
        char here[1024];
#ifdef _WIN32
        if (!_getcwd(here, (int)sizeof(here))) snprintf(here, sizeof(here), ".");
#else
        if (!getcwd(here, sizeof(here))) snprintf(here, sizeof(here), ".");
#endif
        for (char* q = here; *q; q++) if (*q == '\\') *q = '/';
        snprintf(msg, sizeof(msg),
                 "unresolved import '%s': no module of that name was found in ", name);
        const char* src_dir = global_module_registry ? global_module_registry->source_dir : NULL;
        int has_src_dir = src_dir && src_dir[0];
        int beside_first = global_module_registry && global_module_registry->source_is_module;
        if (beside_first && has_src_dir)
            msg_append_dir(msg, sizeof(msg), here, src_dir, " (beside this module)");
        for (int i = 0; global_module_registry && i < global_module_registry->lib_dir_count; i++)
            msg_append_dir(msg, sizeof(msg), here, global_module_registry->lib_dirs[i], "");
        msg_append_dir(msg, sizeof(msg), here, "src", "");
        msg_append_dir(msg, sizeof(msg), here, ".", " (where the compile ran)");
        if (!beside_first && has_src_dir)
            msg_append_dir(msg, sizeof(msg), here, src_dir, " (beside the importing file)");
        msg_append(msg, sizeof(msg), "and the installed packages");
    }
    aether_error_simple(msg, import_node ? import_node->line : 0,
                        import_node ? import_node->column : 0);
}

static char* resolve_import_path(ASTNode* import_node) {
    const char* module_path = import_node->value;
    if (!module_path) return NULL;

    char* file_path;
    int errors_before = aether_error_count();
    if (strncmp(module_path, "std.", 4) == 0) {
        file_path = module_resolve_stdlib_path(module_path + 4);
    } else if (strncmp(module_path, "contrib.", 8) == 0) {
        file_path = module_resolve_contrib_path(module_path + 8);
    } else {
        file_path = module_resolve_local_path(module_path);
    }
    if (file_path) return file_path;

    /* The resolver failed hard (e.g. an ambiguous installed package) rather
     * than simply not finding the path. Retrying as `module.symbol` would
     * re-walk the same roots and report the identical error a second time,
     * so surface the original diagnostic alone. */
    if (aether_error_count() > errors_before) return NULL;

    const char* last_dot = strrchr(module_path, '.');
    if (!last_dot) return NULL;

    int prefix_len = (int)(last_dot - module_path);
    if (prefix_len <= 0) return NULL;
    const char* symbol = last_dot + 1;
    if (!*symbol) return NULL;
    /* The path before the symbol, however long (#3545: past 511 bytes the
     * import was refused as unresolved). */
    char* prefix = mod_strf("%.*s", prefix_len, module_path);

    if (strncmp(prefix, "std.", 4) == 0) {
        file_path = module_resolve_stdlib_path(prefix + 4);
    } else if (strncmp(prefix, "contrib.", 8) == 0) {
        file_path = module_resolve_contrib_path(prefix + 8);
    } else {
        file_path = module_resolve_local_path(prefix);
    }
    if (!file_path) {
        free(prefix);
        return NULL;
    }

    ASTNode* sym_node = create_ast_node(AST_IDENTIFIER, symbol,
                                        import_node->line, import_node->column);

    ASTNode** new_children = malloc(sizeof(ASTNode*) * (import_node->child_count + 1));
    new_children[0] = sym_node;
    for (int i = 0; i < import_node->child_count; i++) {
        new_children[i + 1] = import_node->children[i];
    }
    free(import_node->children);
    import_node->children = new_children;
    import_node->child_capacity = 0;
    import_node->child_count++;

    free(import_node->value);
    import_node->value = prefix;

    return file_path;
}

/* Walk `ast`'s top-level for the (selective-import, local-def-with-
 * same-name) shadow pattern. Emit a diagnostic on the first hit and
 * return 0; return 1 if no collision. Issue #436 facet A. Used by
 * orchestrate_module (per-module check) and module_orchestrate
 * (main program check) — same shape, same diagnostic.
 *
 * `ctx_label` is what to call the source in the error message —
 * the module's name for orchestrate_module, "main program" for
 * the entry-point AST. `ctx_path` is the .ae path (or NULL).
 */
static int check_selective_import_shadow(ASTNode* ast,
                                          const char* ctx_label,
                                          const char* ctx_path) {
    if (!ast) return 1;
    /* Every name the imports select, with the import it comes from, as many
     * as there are (#3545: they were read into 256 slots, and a local
     * function shadowing a name past them was not refused), the first
     * import of each name kept. */
    ModuleNames selected = {0};
    StrMap selected_from = {0};
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
        for (int sk = 0; sk < child->child_count; sk++) {
            ASTNode* sel = child->children[sk];
            if (!sel || sel->type != AST_IDENTIFIER || !sel->value) continue;
            if (sel->annotation &&
                strcmp(sel->annotation, "module_alias") == 0) continue;
            /* Per-symbol alias: the LOCAL name a def could shadow is
             * the alias, not the exported name (`import vg (path as
             * vgpath)` frees up `path` for local use; a local `vgpath`
             * is now the collision). */
            const char* local = sel->annotation &&
                                        strncmp(sel->annotation, "select_alias:", 13) == 0
                                    ? sel->annotation + 13
                                    : sel->value;
            if (names_has(&selected, local)) continue;
            names_add(&selected, local);
            strmap_put(&selected_from, local, (void*)child->value);
        }
    }
    int ok = 1;
    for (int i = 0; ok && selected.count > 0 && i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (!child) continue;
        const char* fn_name = NULL;
        int fn_line = 0, fn_col = 0;
        if ((child->type == AST_FUNCTION_DEFINITION ||
             child->type == AST_BUILDER_FUNCTION) && child->value) {
            fn_name = child->value;
            fn_line = child->line; fn_col = child->column;
        } else if (child->type == AST_EXPORT_STATEMENT &&
                   child->child_count > 0 && child->children[0]) {
            ASTNode* inner = child->children[0];
            if ((inner->type == AST_FUNCTION_DEFINITION ||
                 inner->type == AST_BUILDER_FUNCTION) && inner->value) {
                fn_name = inner->value;
                fn_line = inner->line; fn_col = inner->column;
            }
        }
        if (!fn_name || !names_has(&selected, fn_name)) continue;
        const char* from = (const char*)strmap_get(&selected_from, fn_name);
        fprintf(stderr,
            "error[E1000]: %s%s%s%s defines local function '%s' "
            "(%d:%d) but also selectively imports '%s' from '%s'\n",
            ctx_label,
            ctx_path ? " (" : "",
            ctx_path ? ctx_path : "",
            ctx_path ? ")" : "",
            fn_name, fn_line, fn_col,
            fn_name, from);
        fprintf(stderr,
            "  the local def silently shadows the import, so a "
            "bare call to '%s(...)' inside the local body would\n"
            "  recurse into the local rather than forward to the "
            "imported symbol, at runtime this is a stack\n"
            "  overflow with no compile-time signal.\n", fn_name);
        fprintf(stderr,
            "  fix one of:\n"
            "    - rename the local function\n"
            "    - drop the selective import: `import %s` (then "
            "call via the qualified form)\n"
            "    - keep both but call the imported version "
            "qualified inside the local body\n",
            from);
        ok = 0;
    }
    names_free(&selected);
    strmap_free(&selected_from);
    return ok;
}

/* #2172: every name a selective import picks must be one the module exports.
 * `import um (helper)` used to bind an unexported `helper` as readily as an
 * exported one, so a module's export list kept nothing private from a
 * selective import (the qualified form is checked by the typechecker). Run
 * once the imported module is registered, which is when its export list is
 * known. A module with no export list exports everything, as elsewhere.
 * Prints the diagnostic and returns 0 on the first unexported name. */
static int check_selective_import_exports(ASTNode* import_child,
                                          const char* importer_path) {
    if (!import_child || !import_child->value) return 1;
    AetherModule* mod = module_find(import_child->value);
    if (!mod || mod->export_count == 0) return 1;
    for (int k = 0; k < import_child->child_count; k++) {
        ASTNode* sel = import_child->children[k];
        if (!sel || sel->type != AST_IDENTIFIER || !sel->value) continue;
        if (sel->annotation && strcmp(sel->annotation, "module_alias") == 0) continue;
        /* A per-symbol alias (`import m (name as local)`) keeps the
         * exported name in value; the alias is only the local spelling. */
        if (module_exports_symbol(mod, sel->value)) continue;
        char msg[512];
        snprintf(msg, sizeof(msg), "'%s' is not exported from module '%s'",
                 sel->value, import_child->value);
        /* `import std.io (println)`: the name is a builtin, which no
         * module exports and nothing needs to import. Say that, rather
         * than send the reader to std.io's export list. */
        const char* hint = is_builtin_function_name(sel->value)
            ? "it is a builtin: call it without importing it"
            : "a selective import can only name what the module lists in "
              "its exports(...)";
        int line = sel->line ? sel->line : import_child->line;
        int col = sel->line ? sel->column : import_child->column;
        /* importer_path names the importing module's file; NULL is the entry
         * program, which the reporter already knows. */
        AetherError e = { importer_path, NULL, line, col, msg, hint, NULL,
                          AETHER_ERR_NOT_EXPORTED };
        aether_error_report(&e);
        return 0;
    }
    return 1;
}

/* Last `.`-separated segment of a module path: "std.audio" -> "audio",
 * "audio" -> "audio". Returns a pointer into `path`. */
static const char* module_last_segment(const char* path) {
    if (!path) return "";
    const char* dot = strrchr(path, '.');
    return dot ? dot + 1 : path;
}

/* #2976: the short name an export of the module whose last segment is
 * `leaf` answers to, `<leaf>.<short>`: the name without its C prefix, a
 * leading `aether_<leaf>_` or `<leaf>_`, when what is left starts a name
 * (`sha3_256_hex` keeps its own). Points into `name`. */
static const char* export_short_name(const char* leaf, const char* name) {
    size_t ln = strlen(leaf);
    const char* rest = name;
    if (ln > 0 && strncmp(rest, "aether_", 7) == 0 && strncmp(rest + 7, leaf, ln) == 0 &&
        rest[7 + ln] == '_')
        rest += 7 + ln + 1;
    else if (ln > 0 && strncmp(rest, leaf, ln) == 0 && rest[ln] == '_')
        rest += ln + 1;
    return isalpha((unsigned char)*rest) ? rest : name;
}

/* #2976: one name per operation. A module may not export two names that
 * answer to one short name, `add` beside `aether_set_add` or `json_free`
 * beside `free`: a caller writing `set.add` reaches one of them, the other
 * is the same operation under a second name (or, worse, a different one
 * that reads as the same). Every such pair is reported, at the later name
 * of the module's export list; returns 0 when there is one. `leaf` is the
 * module's last segment, `path` its file. With `implicit`, a module that
 * lists no exports exports every top-level function, builder, extern and
 * constant, and those are its names. */
static void export_names_add(ASTNode*** names, int* n, int* cap, ASTNode* nm) {
    if (!nm || !nm->value) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *names = (ASTNode**)aether_xrealloc(*names, (size_t)*cap * sizeof(ASTNode*));
    }
    (*names)[(*n)++] = nm;
}

static int check_export_short_names(ASTNode* ast, const char* leaf, const char* module_name,
                                    const char* path, int implicit) {
    if (!ast || !leaf || !leaf[0]) return 1;
    ASTNode** names = NULL;
    int n = 0, cap = 0;
    int listed = 0;
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (!child) continue;
        if (child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            listed = 1;
            export_names_add(&names, &n, &cap, child->children[0]);
        } else if (child->type == AST_EXPORTS_LIST) {
            listed = 1;
            for (int k = 0; k < child->child_count; k++)
                export_names_add(&names, &n, &cap, child->children[k]);
        }
    }
    for (int i = 0; !listed && implicit && i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child && (child->type == AST_FUNCTION_DEFINITION ||
                      child->type == AST_BUILDER_FUNCTION ||
                      child->type == AST_EXTERN_FUNCTION ||
                      child->type == AST_CONST_DECLARATION))
            export_names_add(&names, &n, &cap, child);
    }
    int ok = 1;
    /* #3546: the names seen and the short names they answer to, each the
     * first name that does, so each later name is two probes: compared with
     * every earlier one, a module of n exports cost n squared. */
    StrMap seen = {0};
    StrMap shorts = {0};
    for (int i = 0; i < n; i++) {
        /* A name listed twice is one name. */
        if (strmap_has(&seen, names[i]->value)) continue;
        strmap_put(&seen, names[i]->value, (void*)1);
        const char* si = export_short_name(leaf, names[i]->value);
        ASTNode* first = strmap_get(&shorts, si);
        if (!first) {
            strmap_put(&shorts, si, names[i]);
            continue;
        }
        char msg[1024], hint[1024];
        snprintf(msg, sizeof(msg),
                 "module '%s' exports '%s' and '%s', two names that answer to "
                 "one short name, '%s.%s'",
                 module_name, first->value, names[i]->value, leaf, si);
        snprintf(hint, sizeof(hint),
                 "export one name per operation: drop the duplicate from exports(...), "
                 "or give a raw extern a name of its own, `@extern(\"<c name>\") %s_raw(...)`",
                 si);
        AetherError e = { path, NULL, names[i]->line, names[i]->column, msg, hint, NULL,
                          AETHER_ERR_EXPORT_SHORT_NAME };
        aether_error_report(&e);
        ok = 0;
    }
    strmap_free(&seen);
    strmap_free(&shorts);
    free(names);
    return ok;
}

/* #1780: a module named `X` that does `import std.X` (no alias) self-shadows —
 * a qualified `X.name(...)` call resolves to the CURRENT module first, so a
 * wrapper forwarding to the stdlib function of the same name calls itself, and
 * the program segfaults on infinite recursion with no diagnostic. `import X as
 * Y` aliasing already exists, so warn at the import site and point at it. This
 * is the qualified-`mod.fn` sibling of check_selective_import_shadow's
 * bare-name (#436) check. Warning, not error: the resolution rule is unchanged,
 * we only make the trap visible. `self_name` is the importing module's name. */
static void warn_self_shadowing_import(const char* self_name,
                                       ASTNode* import_child) {
    if (!self_name || !import_child || !import_child->value) return;
    /* An `as`-alias frees the wrapper to name the stdlib module — no trap. */
    for (int k = 0; k < import_child->child_count; k++) {
        ASTNode* c = import_child->children[k];
        if (c && c->annotation && strcmp(c->annotation, "module_alias") == 0)
            return;
    }
    const char* self_seg = module_last_segment(self_name);
    const char* imp_seg = module_last_segment(import_child->value);
    /* Only when the imported path is DIFFERENT from the module's own name but
     * shares the last segment (e.g. module `audio` importing `std.audio`).
     * A module importing literally itself is a separate cycle error. */
    if (strcmp(import_child->value, self_name) == 0) return;
    if (strcmp(self_seg, imp_seg) != 0) return;
    fprintf(stderr,
        "warning: module '%s' imports '%s'; a qualified call to '%s.<name>' "
        "resolves to THIS module first, not '%s'\n",
        self_name, import_child->value, self_seg, import_child->value);
    fprintf(stderr,
        "  so a wrapper forwarding to '%s.<name>' of the same name calls "
        "itself — infinite recursion at runtime, with no other signal.\n",
        import_child->value);
    fprintf(stderr,
        "  fix: alias the import so the wrapper can name what it wraps, e.g. "
        "`import %s as %s_std` and call `%s_std.<name>`.\n",
        import_child->value, self_seg, self_seg);
}

/* #2255: do two paths name the same file? The same module is reached as
 * `contrib/jq/value.ae` from one import and as an absolute path from
 * another, so compare identity (device and inode) where the platform has
 * it, and the canonical full path on Windows, where names are
 * case-insensitive. */
static int module_same_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;
#ifdef _WIN32
    char fa[MAX_PATH], fb[MAX_PATH];
    if (!_fullpath(fa, a, sizeof(fa)) || !_fullpath(fb, b, sizeof(fb))) return 0;
    for (char* p = fa; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = fb; *p; p++) if (*p == '/') *p = '\\';
    return _stricmp(fa, fb) == 0;
#else
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

AetherModule* module_find_by_file(const char* file_path) {
    if (!global_module_registry || !file_path) return NULL;
    for (int i = 0; i < global_module_registry->module_count; i++) {
        AetherModule* m = global_module_registry->modules[i];
        if (m && module_same_file(m->file_path, file_path)) return m;
    }
    return NULL;
}

/* Is `resolved` the file `import <import_path>` names inside the directory
 * of `importer_file`, i.e. a package importing one of its own files? */
static int import_is_sibling(const char* importer_file, const char* import_path,
                             const char* resolved) {
    if (!importer_file || !import_path || !resolved) return 0;
    const char* slash = strrchr(importer_file, '/');
#ifdef _WIN32
    const char* bslash = strrchr(importer_file, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    size_t dir_len = slash ? (size_t)(slash - importer_file) + 1 : 0;
    char* converted = dots_to_slashes(import_path);
    char* cand = mod_strf("%.*s%s.ae", (int)dir_len, importer_file, converted);
    int same = module_same_file(resolved, cand);
    free(cand);
    if (!same) {
        cand = mod_strf("%.*s%s/module.ae", (int)dir_len, importer_file, converted);
        same = module_same_file(resolved, cand);
        free(cand);
    }
    free(converted);
    return same;
}

/* The dotted path a package's own files are named under: the module's
 * name when it is the package's facade (`<dir>/module.ae`), otherwise its
 * name without the last segment (`contrib.jq.eval` is a file beside the
 * facade). NULL when there is no such prefix. Caller frees. */
static char* module_package_prefix(const char* name, const char* file_path) {
    if (!name) return NULL;
    size_t fl = file_path ? strlen(file_path) : 0;
    if (fl >= 9 && strcmp(file_path + fl - 9, "module.ae") == 0 &&
        (fl == 9 || file_path[fl - 10] == '/' || file_path[fl - 10] == '\\')) {
        return strdup(name);
    }
    const char* dot = strrchr(name, '.');
    if (!dot) return NULL;
    char* prefix = malloc((size_t)(dot - name) + 1);
    memcpy(prefix, name, (size_t)(dot - name));
    prefix[dot - name] = '\0';
    return prefix;
}

/* #2255: settle which registered module an import names, BEFORE it is
 * looked up or loaded. The registry is keyed by name, and a package
 * importing one of its own files writes a bare name: std.jsonpath and
 * contrib.jq both `import parser`. Keyed as `parser`, whichever loaded
 * first answered for both, so the other package ran against the wrong
 * parser (a compile error one way round, a silently failing
 * jsonpath.query the other).
 *
 *  - A bare import from inside a shipped package that resolves to one of
 *    its own files is named by its place in the package
 *    (`contrib.jq.parser`), so a package's internals never take a bare
 *    name another package, or the program, could also want. The
 *    namespace is still the last segment unless something else ends the
 *    same way, so the generated C only changes when there is a clash.
 *  - An import of a file already loaded under another name binds to that
 *    module instead of loading the file a second time (jq's tests import
 *    `value` directly while contrib.jq loads it as contrib.jq.value). Only
 *    when both names end the same way: the scope's written prefix is that
 *    last segment, and must not change under it. */
/* #2607: the instruction set an `@target("avx2") import` names (the parser's
 * `lane_target:` marker), or NULL for an ordinary import. */
static const char* import_lane_target(const ASTNode* imp) {
    if (!imp || !imp->annotation || strncmp(imp->annotation, "lane_target:", 12) != 0) return NULL;
    return imp->annotation + 12;
}

/* #2607: `@target("avx2") import m as alias` is a module of its own: the same
 * file loaded a second time and registered as `m__avx2`, so it gets its own
 * namespace and its own functions beside a plain `import m`, and codegen can
 * emit its functions compiled for the target (lane_region_open). The import
 * is renamed to that module before it is looked up or loaded; the alias is
 * how the importer names it. Returns 0 after reporting an import that cannot
 * be one: without an alias the copy has no name of its own to be called by,
 * and a selection would leave the copy's own functions half merged. */
static int module_bind_target_import(ASTNode* imp, const char* importer_file) {
    const char* target = import_lane_target(imp);
    int alias = 0, selection = 0;
    for (int k = 0; k < imp->child_count; k++) {
        ASTNode* c = imp->children[k];
        if (!c || c->type != AST_IDENTIFIER) continue;
        if (c->annotation && strcmp(c->annotation, "module_alias") == 0) alias = 1;
        else selection = 1;
    }
    if (!alias || selection) {
        char msg[512];
        snprintf(msg, sizeof(msg), "@target(\"%s\") import %s %s", target, imp->value,
                 !alias ? "has no `as` name" : "selects names");
        AetherError e = { importer_file, NULL, imp->line, imp->column, msg,
                          !alias ? "a module imported at a target is a second copy of it, called through its own name: `@target(\"avx2\") import m as m_avx2`"
                                 : "a module imported at a target is copied whole: drop the (names) list and call it through its `as` name",
                          NULL, AETHER_ERR_SYNTAX };
        aether_error_report(&e);
        return 0;
    }
    size_t n = strlen(imp->value) + 2 + strlen(target) + 1;
    char* instance = malloc(n);
    snprintf(instance, n, "%s__%s", imp->value, target);
    free(imp->value);
    imp->value = instance;
    return 1;
}

/* #2607: once the copy is registered, it carries its target. */
static void module_note_lane_target(const ASTNode* imp) {
    const char* target = import_lane_target(imp);
    AetherModule* m = target ? module_find(imp->value) : NULL;
    if (m && !m->lane_target) m->lane_target = strdup(target);
}

static void module_bind_import_identity(ASTNode* imp, const char* resolved,
                                        const char* importer,
                                        const char* importer_file) {
    if (!imp || !imp->value || !resolved) return;
    if (importer && module_is_shipped(importer) && !module_is_shipped(imp->value) &&
        import_is_sibling(importer_file, imp->value, resolved)) {
        char* prefix = module_package_prefix(importer, importer_file);
        if (prefix) {
            size_t n = strlen(prefix) + 1 + strlen(imp->value) + 1;
            char* qualified = malloc(n);
            snprintf(qualified, n, "%s.%s", prefix, imp->value);
            free(prefix);
            free(imp->value);
            imp->value = qualified;
        }
    }
    AetherModule* by_name = module_find(imp->value);
    if (by_name) return;
    AetherModule* by_file = module_find_by_file(resolved);
    if (by_file && by_file->name &&
        strcmp(module_last_segment(by_file->name), module_last_segment(imp->value)) == 0) {
        free(imp->value);
        imp->value = strdup(by_file->name);
    }
}

/* Put back the source directory saved around a module's imports. */
static void module_restore_source_dir(char* saved, int saved_is_module) {
    free(global_module_registry->source_dir);
    global_module_registry->source_dir = saved;
    global_module_registry->source_is_module = saved_is_module;
}

// Recursive helper: load a single module and its transitive imports
static int orchestrate_module(const char* module_name, const char* file_path,
                              DependencyGraph* graph) {
    // Already loaded? Skip.
    if (module_find(module_name)) return 1;

    // Parse the file
    ASTNode* ast = module_parse_file(file_path);
    if (!ast) return 1;  // Graceful: file may exist but be empty/invalid

    // Create and register module
    AetherModule* mod = module_create(module_name, file_path);
    mod->ast = ast;
    module_register(mod);

    // Collect exports from BOTH the legacy per-function `export <fn>`
    // form (AST_EXPORT_STATEMENT) AND the new top-of-file `exports (…)`
    // list form (AST_EXPORTS_LIST). The two forms are mutually exclusive
    // within a single module — mixing them is a hard error since it
    // signals confusion about which is the source of truth. The legacy
    // form gets a per-module deprecation warning that prints the exact
    // migration line so users know what to paste at the top of their
    // file and which `export` keywords to remove.
    int has_legacy_export = 0;
    int has_exports_list  = 0;
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type == AST_EXPORT_STATEMENT) has_legacy_export = 1;
        if (child->type == AST_EXPORTS_LIST)     has_exports_list = 1;
    }
    if (has_legacy_export && has_exports_list) {
        fprintf(stderr,
                "error: module '%s' mixes the legacy `export <fn>` form with "
                "the new top-of-file `exports (…)` list, pick one.\n",
                module_name);
    }
    if (has_legacy_export && !has_exports_list) {
        // Build a precise, copy-pasteable migration message. Show the
        // exact `exports (…)` line the user should add, list each
        // `export`-tagged declaration that needs the keyword stripped,
        // and point at the docs section that explains the rationale.
        fprintf(stderr,
                "warning: module '%s' uses the deprecated per-function "
                "`export` keyword.\n", module_name);
        fprintf(stderr, "  Migrate in two steps:\n");
        fprintf(stderr, "    1. Add this line at the top of the file:\n");
        fprintf(stderr, "         exports (");
        int first = 1;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* child = ast->children[i];
            if (child->type != AST_EXPORT_STATEMENT) continue;
            if (child->child_count == 0) continue;
            ASTNode* exported = child->children[0];
            if (!exported || !exported->value) continue;
            if (!first) fprintf(stderr, ", ");
            fprintf(stderr, "%s", exported->value);
            first = 0;
        }
        fprintf(stderr, ")\n");
        fprintf(stderr,
                "    2. Remove the `export` keyword from each declaration:\n"
                "         export double_value(x) { … }\n"
                "                ^^^^^ delete\n");
        fprintf(stderr,
                "  See docs/module-system-design.md "
                "§ \"Legacy `export <fn>` form (deprecated)\".\n");
    }
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        // Legacy: `export <fn>` wraps the declaration as the first child.
        if (child->type == AST_EXPORT_STATEMENT && child->child_count > 0) {
            ASTNode* exported = child->children[0];
            if (exported->value) {
                module_add_export(mod, exported->value);
            }
        }
        // New: `exports (a, b, c)` — children are AST_IDENTIFIER name nodes.
        if (child->type == AST_EXPORTS_LIST) {
            for (int k = 0; k < child->child_count; k++) {
                ASTNode* name_node = child->children[k];
                if (name_node && name_node->value) {
                    module_add_export(mod, name_node->value);
                }
            }
        }
    }

    /* Issue #436 (facet A): silent infinite-recursion guard.
     *
     * Detect the pattern
     *   import std.X (length)        // selective import
     *   length(s: string) -> int {   // local def with same name
     *       return length(s)          // intent: std.X.length; reality: self
     *   }
     *
     * The merger renames the intra-module call to `mod_length`, the
     * body becomes self-recursive, runtime stack overflows. Caught
     * here BEFORE the merger runs. Helper extracted so the same
     * check applies to the main program AST in module_orchestrate. */
    char* ctx = mod_strf("module '%s'", module_name);
    int shadow_ok = check_selective_import_shadow(ast, ctx, file_path);
    free(ctx);
    if (!shadow_ok) return 0;
    if (!check_export_short_names(ast, module_last_segment(module_name), module_name, file_path, 1))
        return 0;
    /* #2125: the C files this module ships must exist, and they are
     * dependencies of the build. */
    if (!module_check_source_directives(ast)) return 0;

    // Add node to dependency graph
    dependency_graph_add_node(graph, module_name);

    /* Resolve THIS module's imports relative to THIS module's directory,
     * not the entry program's. Without this, `source_dir` stays pinned to
     * the top-level file for the whole compile, so a by-name module whose
     * facade imports its own submodules (e.g. std.jsonpath's module.ae
     * doing `import parser` -> std/jsonpath/src/parser.ae) resolves those
     * submodules against the CONSUMER's directory and fails. The Try-6b
     * "relative to source file directory" branch in module_resolve_local_path
     * only works if source_dir names the importing module. Save and restore
     * around the import loop so sibling modules at the same level still see
     * the parent's source_dir. The parent's is kept as it is, whole, and
     * put back (#2543). */
    char* saved_source_dir = global_module_registry->source_dir;
    int saved_is_module = global_module_registry->source_is_module;
    global_module_registry->source_dir = NULL;
    module_set_source_dir(file_path);
    global_module_registry->source_is_module = 1;

    // Recursively process this module's imports
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;

        char* sub_file = resolve_import_path(child);
        if (import_lane_target(child)) {
            if (!module_bind_target_import(child, file_path)) {   /* #2607 */
                free(sub_file);
                module_restore_source_dir(saved_source_dir, saved_is_module);
                return 0;
            }
        } else {
            module_bind_import_identity(child, sub_file, module_name, file_path);  /* #2255 */
        }
        const char* sub_path = child->value;

        warn_self_shadowing_import(module_name, child);   /* #1780 */
        dependency_graph_add_edge(graph, module_name, sub_path);
        module_add_import(mod, sub_path);

        if (sub_file) {
            if (!orchestrate_module(sub_path, sub_file, graph)) {
                free(sub_file);
                module_restore_source_dir(saved_source_dir, saved_is_module);
                return 0;
            }
            free(sub_file);
            module_note_lane_target(child);   /* #2607 */
            if (!check_selective_import_exports(child, file_path)) {
                module_restore_source_dir(saved_source_dir, saved_is_module);
                return 0;
            }
        } else {
            report_unresolved_import(child);
        }
    }

    module_restore_source_dir(saved_source_dir, saved_is_module);
    return 1;
}

// Reject the case where a user's top-level function forges the C symbol
// an imported module export mangles to (e.g. local `proxy_opts_new` vs
// `proxy.opts_new`). Defined below module_get_namespace / unwrap_export;
// forward-declared here so module_orchestrate can call it after imports
// are registered. Returns 0 (and prints a diagnostic) on collision.
static int check_namespace_prefix_collision(ASTNode* program);

// Top-level orchestration: scan program AST, resolve all imports,
// parse modules, build dependency graph, detect cycles.
int module_orchestrate(ASTNode* program) {
    module_registry_init();

    /* Issue #436 facet A: the same selective-import shadow check
     * orchestrate_module applies per-module also runs against the
     * entry-point AST, so `import std.X (foo)` + local `foo(...)`
     * in main.ae itself is caught with the same diagnostic. */
    if (!check_selective_import_shadow(program, "main program", NULL)) return 0;
    if (!module_check_source_directives(program)) return 0;   /* #2125 */
    /* #2976: a module file built or checked on its own (`ae check
     * lib/m/module.ae`, `--emit=lib`) is held to the rule its importers
     * hold it to, under the namespace a build gives it. */
    if (program && program->source_file) {
        /* Room for any leaf of the path or of its full path (#3545: 256
         * bytes, and a longer leaf left the check undone). */
        size_t cap = strlen(program->source_file) + 4096;
        char* leaf = malloc(cap);
        if (!leaf) {
            fprintf(stderr, "Fatal: out of memory\n");
            exit(1);
        }
        int ok = !module_leaf_of_file(program->source_file, leaf, cap)[0] ||
                 check_export_short_names(program, leaf, leaf, program->source_file, 0);
        free(leaf);
        if (!ok) return 0;
    }

    DependencyGraph* graph = dependency_graph_create();
    dependency_graph_add_node(graph, "__main__");

    // Scan top-level AST for imports
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;

        char* file_path = resolve_import_path(child);
        if (import_lane_target(child)) {
            if (!module_bind_target_import(child, NULL)) {   /* #2607 */
                free(file_path);
                dependency_graph_free(graph);
                return 0;
            }
        } else {
            module_bind_import_identity(child, file_path, NULL, NULL);  /* #2255 */
        }
        const char* module_path = child->value;

        dependency_graph_add_edge(graph, "__main__", module_path);

        if (file_path) {
            if (!orchestrate_module(module_path, file_path, graph)) {
                free(file_path);
                dependency_graph_free(graph);
                return 0;
            }
            free(file_path);
            module_note_lane_target(child);   /* #2607 */
            if (!check_selective_import_exports(child, NULL)) {
                dependency_graph_free(graph);
                return 0;
            }
        } else {
            report_unresolved_import(child);
        }
    }

    // Check for cycles
    if (dependency_graph_has_cycle(graph)) {
        dependency_graph_free(graph);
        return 0;
    }

    dependency_graph_free(graph);

    /* #2209: every module is loaded, so each can be given a namespace no
     * other loaded module shares. Before the collision check below, which
     * builds the `<ns>_<name>` symbols the merge will emit. */
    module_assign_namespaces();

    /* All imports are now registered, but merge hasn't run yet — so the
     * program's top-level functions are still exactly the user's. Reject
     * any that forge an imported export's mangled C symbol (the silent
     * `proxy_opts_new` ↔ `proxy.opts_new` shadow). */
    if (!check_namespace_prefix_collision(program)) return 0;

    return 1;
}

// --- Pure Aether Module Merging ---

// The per-module name lists module_merge_into_program renames merged
// bodies by. Every clone-and-rename pass reads the source module's function
// and constant names so that intra-module calls (`square(...)` referencing a
// sibling) are rewritten to the prefixed form (`mathy_square(...)`) before
// the body lands in the consumer. They were fixed arrays, of 128 and then of
// 4096 names, and a module past the cap silently left the rest bare: a call
// of a sibling past it failed E0301, and a bare name past it that the
// program also declares bound to the program's (#3545). Each list is now as
// long as the module needs (ModuleNames, kept on the AetherModule).

// Namespace a module path's merged symbols carry: "mypackage.utils" ->
// "utils", unless another loaded module also ends in `utils` (#2209, see
// module_assign_namespaces).
static const char* module_get_namespace(const char* module_path) {
    return module_namespace_of(module_path);
}


/* Does this import select names (`import m (a, b)`, or the `import m.sym`
 * form resolve_import_path rewrites into it)? Selectors are AST_IDENTIFIER
 * children, and so is an `as` alias — which the parser appends LAST, so it
 * is children[0] only when it is the sole child. #2209: `import m as x`
 * used to read as a selection of `x`, merging nothing from `m`. */
static int import_has_selection(const ASTNode* imp) {
    if (!imp || imp->child_count == 0 || !imp->children[0]) return 0;
    const ASTNode* first = imp->children[0];
    if (first->type != AST_IDENTIFIER) return 0;
    return !(first->annotation && strcmp(first->annotation, "module_alias") == 0);
}

// Get the actual declaration from a node (unwrap AST_EXPORT_STATEMENT if needed)
static ASTNode* unwrap_export(ASTNode* node) {
    if (node->type == AST_EXPORT_STATEMENT && node->child_count > 0) {
        return node->children[0];
    }
    return node;
}

/* #2699: the wrapper used to stay in the file being compiled, and each pass
 * walking its top-level definitions had to look through it. Most did not:
 * an exported function missed the type checker's third pass (W1001, W1002,
 * `@scoped`, Isolated[T]), return-type inference, the effect walk, the
 * rename off a libaether C name, its C prototype and its place in a clause
 * set; an exported constant was undeclared in the C and an exported actor
 * could not be spawned. The merge unwraps what it takes from a module the
 * same way. */
void module_unwrap_entry_exports(ASTNode* program) {
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* wrapper = program->children[i];
        if (!wrapper || wrapper->type != AST_EXPORT_STATEMENT || wrapper->child_count == 0)
            continue;
        ASTNode* decl = wrapper->children[0];
        /* `export name` names a definition rather than wrapping one. */
        if (!decl || decl->type == AST_IDENTIFIER) continue;
        program->children[i] = decl;
        wrapper->child_count = 0;
        free_ast_node(wrapper);
    }
}

/* #3545: `name` added to `l`, which grows to hold it, and to its index
 * (#3546). */
static void names_add(ModuleNames* l, const char* name) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        const char** grown = realloc(l->names, sizeof(*grown) * (size_t)cap);
        if (!grown) {
            fprintf(stderr, "Fatal: out of memory\n");
            exit(1);
        }
        l->names = grown;
        l->cap = cap;
    }
    l->names[l->count++] = name;
    strmap_put(&l->index, name, (void*)1);
}

/* #3546: is `name` one of `l`'s? One probe, where a walk of the list made
 * the rename of a merged body cost its identifiers times the module's names. */
static int names_has(const ModuleNames* l, const char* name) {
    return l && name && l->count > 0 && strmap_has(&l->index, name);
}

static void names_clear(ModuleNames* l) {
    l->count = 0;
    strmap_free(&l->index);
}

static void names_free(ModuleNames* l) {
    free(l->names);
    l->names = NULL;
    l->count = l->cap = 0;
    strmap_free(&l->index);
}

/* The function names and the constant names `m` declares (#3545): read
 * from its AST once, as many as it has, and again only if the AST is
 * another or has gained or lost a declaration since. A fault's members are
 * const-like: a bare `return NotFound` inside the module is renamed to
 * `<ns>_NotFound` as a constant's reference is, so each member's bare name
 * is among the constants. */
static void module_collect_names(AetherModule* m) {
    if (!m || !m->ast) return;
    if (m->names_ast == m->ast && m->names_children == m->ast->child_count) return;
    names_clear(&m->func_names);
    names_clear(&m->const_names);
    for (int i = 0; i < m->ast->child_count; i++) {
        ASTNode* decl = unwrap_export(m->ast->children[i]);
        if (!decl) continue;
        if ((decl->type == AST_FUNCTION_DEFINITION || decl->type == AST_BUILDER_FUNCTION) &&
            decl->value) {
            names_add(&m->func_names, decl->value);
        } else if (decl->type == AST_CONST_DECLARATION && decl->value) {
            names_add(&m->const_names, decl->value);
        } else if (decl->type == AST_FAULT_DEFINITION) {
            for (int mi = 0; mi < decl->child_count; mi++) {
                ASTNode* mem = decl->children[mi];
                if (mem && mem->value) names_add(&m->const_names, mem->value);
            }
        }
    }
    m->names_ast = m->ast;
    m->names_children = m->ast->child_count;
}

static const ModuleNames* module_func_names(AetherModule* m) {
    module_collect_names(m);
    return &m->func_names;
}

static const ModuleNames* module_const_names(AetherModule* m) {
    module_collect_names(m);
    return &m->const_names;
}

/* ---- #3546: an AST's top-level declarations by name ----
 *
 * The merge asks of a program or a module "is a function (a constant, a
 * module `var`, an extern, a fault member) of this name declared here?" for
 * every declaration it merges and for every reference it renames, and "which
 * modules does it import?" for every declaration it clones. Each question
 * walked the whole top level, so merging a module of n declarations cost n
 * squared. An index per AST answers each in one probe. It is built the first
 * time an AST is asked about and kept up to date by insert_child_at, the one
 * way the merge adds a declaration; a top level that changed any other way
 * (its count is not the indexed one) is indexed again. A module's goes with
 * the module (module_free), the program's when the merge ends. */
enum {
    DECL_FN = 1,            /* a function or builder definition */
    DECL_CONST = 2,         /* a constant or a module `var` */
    DECL_VAR = 4,           /* a module `var` (annotation global_var) */
    DECL_EXTERN = 8,
    DECL_FAULT_MEMBER = 16  /* a member of a fault definition */
};

typedef struct {
    int kinds;
    int fn_count;           /* function and builder definitions of the name */
    int first_fn;           /* position of the first, -1 for none */
    int first_csym_extern;  /* position of the first extern `@extern("sym")` binds */
    ASTNode* first_extern;  /* the first extern of the name (unwrapped) */
} DeclEntry;

typedef struct {
    const ASTNode* ast;
    int indexed;            /* the top-level children indexed */
    StrMap names;           /* name -> DeclEntry* */
    StrMap c_symbols;       /* the C symbol an extern binds -> (void*)1 */
    StrMap typed;           /* `<node kind>:<name>` -> the first definition (unwrapped) */
    ASTNode** imports;      /* the AST_IMPORT_STATEMENTs, in order */
    int import_count, import_cap;
} DeclIndex;

static DeclIndex** g_decl_indexes = NULL;
static int g_decl_index_count = 0;
static int g_decl_index_cap = 0;

static void decl_index_clear(DeclIndex* ix) {
    for (int i = 0; i < strmap_count(&ix->names); i++) free(strmap_value_at(&ix->names, i));
    strmap_free(&ix->names);
    strmap_free(&ix->c_symbols);
    strmap_free(&ix->typed);
    free(ix->imports);
    ix->imports = NULL;
    ix->import_count = ix->import_cap = 0;
    ix->indexed = 0;
}

static DeclEntry* decl_entry(DeclIndex* ix, const char* name) {
    DeclEntry* e = strmap_get(&ix->names, name);
    if (!e) {
        e = calloc(1, sizeof(DeclEntry));
        if (!e) {
            fprintf(stderr, "Fatal: out of memory\n");
            exit(1);
        }
        e->first_fn = -1;
        e->first_csym_extern = -1;
        strmap_put(&ix->names, name, e);
    }
    return e;
}

/* Note in `ix` the C symbol extern `decl` binds: its own name, or c_name
 * for `@extern("c_name") name(...)` (annotation `c_symbol:c_name`, with an
 * optional `;varargs`). */
static void decl_index_add_c_symbol(DeclIndex* ix, const ASTNode* decl) {
    if (decl->annotation && strncmp(decl->annotation, "c_symbol:", 9) == 0) {
        const char* s = decl->annotation + 9;
        const char* semi = strchr(s, ';');
        size_t n = semi ? (size_t)(semi - s) : strlen(s);
        char* sym = malloc(n + 1);
        if (!sym) {
            fprintf(stderr, "Fatal: out of memory\n");
            exit(1);
        }
        memcpy(sym, s, n);
        sym[n] = '\0';
        strmap_put(&ix->c_symbols, sym, (void*)1);
        free(sym);
        return;
    }
    strmap_put(&ix->c_symbols, decl->value, (void*)1);
}

/* The key of definition `name` of node kind `type` in DeclIndex.typed. */
static char* decl_typed_key(int type, const char* name) {
    size_t n = strlen(name) + 16;
    char* key = malloc(n);
    if (!key) {
        fprintf(stderr, "Fatal: out of memory\n");
        exit(1);
    }
    snprintf(key, n, "%d:%s", type, name);
    return key;
}

/* `child`, the top-level declaration at position `pos`, into the index. */
static void decl_index_add(DeclIndex* ix, ASTNode* child, int pos) {
    if (!child) return;
    if (child->type == AST_IMPORT_STATEMENT) {
        if (ix->import_count == ix->import_cap) {
            int cap = ix->import_cap ? ix->import_cap * 2 : 16;
            ASTNode** grown = realloc(ix->imports, sizeof(*grown) * (size_t)cap);
            if (!grown) {
                fprintf(stderr, "Fatal: out of memory\n");
                exit(1);
            }
            ix->imports = grown;
            ix->import_cap = cap;
        }
        ix->imports[ix->import_count++] = child;
        return;
    }
    ASTNode* decl = unwrap_export(child);
    if (!decl) return;
    if ((decl->type == AST_FUNCTION_DEFINITION || decl->type == AST_BUILDER_FUNCTION) && decl->value) {
        DeclEntry* e = decl_entry(ix, decl->value);
        e->kinds |= DECL_FN;
        if (e->fn_count++ == 0) e->first_fn = pos;
    } else if (decl->type == AST_CONST_DECLARATION && decl->value) {
        DeclEntry* e = decl_entry(ix, decl->value);
        e->kinds |= DECL_CONST;
        if (decl->annotation && strcmp(decl->annotation, "global_var") == 0) e->kinds |= DECL_VAR;
    } else if (decl->type == AST_EXTERN_FUNCTION && decl->value) {
        DeclEntry* e = decl_entry(ix, decl->value);
        e->kinds |= DECL_EXTERN;
        if (!e->first_extern) e->first_extern = decl;
        if (e->first_csym_extern < 0 && decl->annotation &&
            strncmp(decl->annotation, "c_symbol:", 9) == 0)
            e->first_csym_extern = pos;
        decl_index_add_c_symbol(ix, decl);
    } else if (decl->type == AST_FAULT_DEFINITION) {
        for (int k = 0; k < decl->child_count; k++) {
            ASTNode* mem = decl->children[k];
            if (mem && mem->value) decl_entry(ix, mem->value)->kinds |= DECL_FAULT_MEMBER;
        }
    }
    /* Every named definition by its node kind as well, the first one kept
     * (program_find_def). */
    if (decl->value) {
        char* key = decl_typed_key((int)decl->type, decl->value);
        if (!strmap_has(&ix->typed, key)) strmap_put(&ix->typed, key, decl);
        free(key);
    }
}

/* The index of `ast`, built or brought up to date. */
static DeclIndex* decl_index(const ASTNode* ast) {
    if (!ast) return NULL;
    DeclIndex* ix = NULL;
    for (int i = 0; i < g_decl_index_count; i++) {
        if (g_decl_indexes[i]->ast == ast) {
            ix = g_decl_indexes[i];
            break;
        }
    }
    if (!ix) {
        if (g_decl_index_count == g_decl_index_cap) {
            int cap = g_decl_index_cap ? g_decl_index_cap * 2 : 32;
            DeclIndex** grown = realloc(g_decl_indexes, sizeof(*grown) * (size_t)cap);
            if (!grown) {
                fprintf(stderr, "Fatal: out of memory\n");
                exit(1);
            }
            g_decl_indexes = grown;
            g_decl_index_cap = cap;
        }
        ix = calloc(1, sizeof(DeclIndex));
        if (!ix) {
            fprintf(stderr, "Fatal: out of memory\n");
            exit(1);
        }
        ix->ast = ast;
        g_decl_indexes[g_decl_index_count++] = ix;
        ix->indexed = -1;
    }
    if (ix->indexed != ast->child_count) {
        decl_index_clear(ix);
        for (int i = 0; i < ast->child_count; i++) decl_index_add(ix, ast->children[i], i);
        ix->indexed = ast->child_count;
    }
    return ix;
}

/* What `ast` declares under `name`: DECL_* bits, 0 for nothing. */
static int ast_decl_kinds(const ASTNode* ast, const char* name) {
    if (!ast || !name) return 0;
    DeclEntry* e = strmap_get(&decl_index(ast)->names, name);
    return e ? e->kinds : 0;
}

/* `child`, just inserted into `parent`'s children (insert_child_at), into
 * an index of `parent` that was current before it. */
static void decl_index_note_insert(const ASTNode* parent, ASTNode* child, int pos) {
    for (int i = 0; i < g_decl_index_count; i++) {
        DeclIndex* ix = g_decl_indexes[i];
        if (ix->ast != parent) continue;
        if (ix->indexed == parent->child_count - 1) {
            decl_index_add(ix, child, pos);
            ix->indexed++;
        }
        return;
    }
}

ASTNode* module_ast_extern_named(const ASTNode* ast, const char* name) {
    if (!ast || !name) return NULL;
    DeclEntry* e = strmap_get(&decl_index(ast)->names, name);
    return e ? e->first_extern : NULL;
}

/* Forget the index of `ast`, whose AST is about to be freed or done with. */
static void decl_index_drop(const ASTNode* ast) {
    for (int i = 0; i < g_decl_index_count; i++) {
        if (g_decl_indexes[i]->ast != ast) continue;
        decl_index_clear(g_decl_indexes[i]);
        free(g_decl_indexes[i]);
        g_decl_indexes[i] = g_decl_indexes[--g_decl_index_count];
        return;
    }
}

/* #937: is `name` a MUTABLE module-level `var` (annotation "global_var",
 * introduced by #701) in the module identified by namespace `ns`? Distinct
 * from a plain `const`: a `var` is a real lvalue (file-scope static) whose
 * cross-function writes must reach the shared cell, whereas a `const` lowers
 * to a `#define` and a `name = expr` against it is a genuine shadowing local.
 * Used to scope the import-boundary write rename to vars only — without the
 * `global_var` gate, the rename would rewrite a const-shadowing local's
 * assignment target into the `#define`, producing invalid C. */
static int name_is_module_global_var(const char* ns, const char* name) {
    if (!ns || !name) return 0;
    /* `ns` is the namespace the codegen prefix is built from, not the
     * module's full dotted path; module_find_by_namespace maps it back
     * (#2209: by the assigned namespace first, so two co-imported modules
     * sharing a last segment resolve to the right one). */
    AetherModule* m = module_find_by_namespace(ns);
    if (!m || !m->ast) return 0;
    return (ast_decl_kinds(m->ast, name) & DECL_VAR) != 0;
}

// Does the module declare an extern C function with this exact name?
static int module_has_extern_named(ASTNode* mod_ast, const char* name) {
    return (ast_decl_kinds(mod_ast, name) & DECL_EXTERN) != 0;
}

/* #2978: the C name the merge gives function (or builder) `name` of the
 * module `mod_ast`, whose namespace is `ns`: `<ns>_<name>`, the spelling C
 * uses for its own functions, unless the module declares an extern of that
 * name, or an extern of the build binds that C symbol (c_symbol_bound_by_extern).
 * Then it is `ae_<ns>_<name>`, a spelling no C symbol uses.
 *
 * A module like contrib.aether_ui has both
 *     extern aether_ui_app_create(...) -> int                 // the C name
 *     app_create(...) { return aether_ui_app_create(...) }   // its wrapper
 * and merged as `aether_ui_app_create` the wrapper was the extern's C
 * symbol: its call of the extern bound to itself and recursed until the
 * stack blew. The merge used to drop such a wrapper and let `ns.f` reach
 * the extern, which is the same only when the wrapper forwards unchanged:
 * `http.server_set_host` returned the extern's nothing where it documents
 * a string, and `http.ws_send_binary(ws, data)` took the extern's three
 * parameters where it documents two. Under its own spelling the wrapper is
 * kept, its body's call of the extern by the extern's name still reaches
 * the extern, and `ns.f` reaches the wrapper (module_function_c_name), as
 * the entry file's function colliding with an extern becomes `ae_<name>`
 * (#1366). */
static int module_defines_function(ASTNode* mod_ast, const char* name) {
    return (ast_decl_kinds(mod_ast, name) & DECL_FN) != 0;
}

/* Does an extern of `mod_ast` bind the C symbol `sym`? A bare extern binds
 * its own name; `@extern("c_name") name(...)` binds c_name. */
static int ast_binds_c_symbol(const ASTNode* mod_ast, const char* sym) {
    if (!mod_ast || !sym) return 0;
    return strmap_has(&decl_index(mod_ast)->c_symbols, sym);
}

/* Is the C symbol `sym` one an extern of the build binds: the module's own
 * (bare, or through `@extern("sym")`), or another loaded module's? Then a
 * function of the module cannot have it as its C name. std.file's `close`
 * as `file_close` beside std.fs's `extern file_close` was two conflicting
 * declarations of one C function, a C compile error. */
static int c_symbol_bound_by_extern(ASTNode* mod_ast, const char* sym) {
    if (ast_binds_c_symbol(mod_ast, sym)) return 1;
    ModuleRegistry* reg = global_module_registry;
    if (!reg) return 0;
    for (int i = 0; i < reg->module_count; i++) {
        AetherModule* m = reg->modules[i];
        if (m && m->ast != mod_ast && ast_binds_c_symbol(m->ast, sym)) return 1;
    }
    return 0;
}

/* The merged C name (malloc'd: as long as it is, #3545; cut to a buffer,
 * two long names became one). */
static char* merged_function_name(const char* ns, ASTNode* mod_ast, const char* name) {
    char* joined = mod_strf("%s_%s", ns, name);
    if (module_defines_function(mod_ast, name) &&
        (module_has_extern_named(mod_ast, joined) || c_symbol_bound_by_extern(mod_ast, joined))) {
        free(joined);
        return mod_strf("ae_%s_%s", ns, name);
    }
    return joined;
}

/* A merged function whose C name is not `<ns>_<name>` keeps the name it was
 * written with, for diagnostics (ast_source_name). */
static void stamp_written_name(ASTNode* clone, const char* ns, const char* name) {
    char* joined = mod_strf("%s_%s", ns, name);
    int same = !clone->value || strcmp(clone->value, joined) == 0;
    free(joined);
    if (same) return;
    free(clone->source_name);
    clone->source_name = strdup(name);
}

/* merged_function_name for a name of the module whose namespace is `ns`,
 * found by it (a rename inside a merged body knows only that). A name the
 * module binds to one of its externs (`length` for std.string's
 * `string_length`, from a glob) is that extern's `<ns>_<name>`. */
static char* merged_function_name_ns(const char* ns, const char* name) {
    AetherModule* m = module_find_by_namespace(ns);
    return merged_function_name(ns, m ? m->ast : NULL, name);
}

/* The C name, malloc'd and as long as it is, or NULL when it is no function
 * of the module. It wrote into its caller's buffer, and a name past the
 * buffer named nothing (#3545, #3569). */
char* module_function_c_name_dup(const char* ns, const char* name) {
    if (!ns || !name) return NULL;
    AetherModule* m = module_find_by_namespace(ns);
    if (!m || !m->ast) return NULL;
    /* #3546: the first function (or builder) of the name, or the first
     * extern `@extern("sym")` binds to it, whichever the module declares
     * first: from the index, where a walk of the module made every
     * qualified call of a large module's function cost its size. */
    DeclEntry* e = strmap_get(&decl_index(m->ast)->names, name);
    if (!e) return NULL;
    int fn = e->first_fn;
    int ext = e->first_csym_extern;
    if (fn >= 0 && (ext < 0 || fn < ext))
        return merged_function_name(m->ns ? m->ns : ns, m->ast, name);
    if (ext >= 0) return mod_strf("%s_%s", m->ns ? m->ns : ns, name);
    return NULL;
}

/* #2632: does the module define `name` itself (a function, builder,
 * constant or extern of its own)? Such a name shadows the same name one of
 * its glob imports would bind. */
static int module_defines_own_name(ASTNode* mod_ast, const char* name) {
    return (ast_decl_kinds(mod_ast, name) & (DECL_FN | DECL_CONST | DECL_EXTERN)) != 0;
}

/* Reject user-function-vs-imported-export symbol collisions.
 *
 * A module export `ns.name` mangles to the flat C symbol `ns_name`.
 * Because `_` is a legal identifier char, a user top-level function
 * literally named `ns_name` forges the *same* C symbol — and the merge's
 * `program_has_function` dedup then silently skips cloning the import, so
 * every `ns.name(...)` call binds to the user's function. Clean compile,
 * clean link, wrong dispatch at runtime (the avnproxy `proxy_opts_new` ↔
 * `proxy.opts_new` hunt — see user-identifiers-must-not-collide spec).
 *
 * This is the general form of the builder-vs-function (0.178.0) and
 * selective-import-shadow (#436) rejections. Run after imports are
 * registered but before merge, when the program's top-level functions
 * are still exactly the user's. Loud `error[E1001]` instead of a silent
 * pick — the spec's "minimum fix" backstop. (Full Java-style internal
 * qualification of every non-exported symbol is the larger follow-on.)
 */
static int check_namespace_prefix_collision(ASTNode* program) {
    if (!program) return 1;

    /* User (pre-merge, top-level) function / builder / extern symbols, each
     * name's first declaration, however many (#3545: 4096 were read, and a
     * collision past them was not refused). */
    StrMap user = {0};
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d || !d->value) continue;
        if ((d->type == AST_FUNCTION_DEFINITION ||
             d->type == AST_BUILDER_FUNCTION ||
             d->type == AST_EXTERN_FUNCTION) && !strmap_has(&user, d->value))
            strmap_put(&user, d->value, d);
    }
    int ok = 1;
    for (int i = 0; ok && strmap_count(&user) > 0 && i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
        AetherModule* mod = module_find(child->value);
        if (!mod || !mod->ast) continue;
        const char* ns = module_get_namespace(child->value);
        if (!ns) continue;

        for (int j = 0; ok && j < mod->ast->child_count; j++) {
            ASTNode* decl = unwrap_export(mod->ast->children[j]);
            if (!decl || !decl->value) continue;
            // Only entities the merge prefixes into a callable `ns_name`
            // C symbol: Aether functions/builders and public `@extern`s.
            // Every one of them, whatever a selective import names: the
            // merge clones the whole module (#2613).
            int callable = (decl->type == AST_FUNCTION_DEFINITION ||
                            decl->type == AST_BUILDER_FUNCTION ||
                            (decl->type == AST_EXTERN_FUNCTION && decl->annotation &&
                             strncmp(decl->annotation, "c_symbol:", 9) == 0));
            if (!callable) continue;
            char* prefixed = decl->type == AST_EXTERN_FUNCTION
                                 ? mod_strf("%s_%s", ns, decl->value)
                                 : merged_function_name(ns, mod->ast, decl->value);
            ASTNode* u = strmap_get(&user, prefixed);
            if (u) {
                fprintf(stderr,
                    "error[E1001]: local function '%s' (%d:%d) collides with "
                    "imported '%s.%s' from '%s'\n",
                    prefixed, u->line, u->column, ns, decl->value, child->value);
                fprintf(stderr,
                    "  an import mangles '%s.%s' to the flat C symbol '%s', the\n"
                    "  same symbol your local function emits. Every '%s.%s(...)'\n"
                    "  call would silently dispatch to your local function: clean\n"
                    "  compile, clean link, wrong function at runtime.\n",
                    ns, decl->value, prefixed, ns, decl->value);
                fprintf(stderr,
                    "  fix one of:\n"
                    "    - rename the local function so it isn't '%s_<name>'\n"
                    "    - drop the local and call '%s.%s(...)' directly\n",
                    ns, ns, decl->value);
                ok = 0;
            }
            free(prefixed);
        }
    }
    strmap_free(&user);
    return ok;
}

// Check if a name is in a string array
static int name_in_list(const char* name, const char** list, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(name, list[i]) == 0) return 1;
    }
    return 0;
}

// Collect all names that are locally bound in a function (params + local variable declarations).
// Recursively walks blocks to find all AST_VARIABLE_DECLARATION and AST_CONST_DECLARATION names.
static void collect_local_names(ASTNode* node, ModuleNames* names) {
    if (!node) return;
    /* #1606: AST_CLOSURE_PARAM belongs here alongside AST_PATTERN_VARIABLE.
       A closure parameter binds a name exactly as a function parameter does,
       so it must shadow a same-named module function or const. Without it,
       `|item: ptr| { f(item) }` inside a module that also defines `item()`
       had its `item` rewritten to `<ns>_item` — the ADDRESS OF THE FUNCTION
       passed where the parameter's value belonged. That compiled, and the
       callee then read a function pointer as data. */
    if ((node->type == AST_PATTERN_VARIABLE || node->type == AST_VARIABLE_DECLARATION ||
         node->type == AST_CONST_DECLARATION || node->type == AST_CLOSURE_PARAM) &&
        node->value) {
        if (!names_has(names, node->value)) names_add(names, node->value);
    }
    for (int i = 0; i < node->child_count; i++) {
        // Don't recurse into nested function definitions (they have their own scope)
        if (node->children[i] && node->children[i]->type == AST_FUNCTION_DEFINITION) continue;
        /* Nor into a nested closure: its parameters and locals are its own
         * scope, which the walkers add when they enter it (the AST_CLOSURE
         * branches). Collected here they shadowed the enclosing function's
         * whole body, so a call to the module's `item` beside a closure
         * whose parameter is `item` was taken for the parameter (#2637). */
        if (node->children[i] && (node->children[i]->type == AST_CLOSURE ||
                                  node->children[i]->type == AST_QUOTE)) continue;
        collect_local_names(node->children[i], names);
    }
}

/* #2683: a local's scope starts after its initializer, so in `helper =
 * helper() + 1` the call is the module's function and every later `helper`
 * is the local. The walk below leaves a name the body binds anywhere
 * unrenamed throughout the body (#2637), the initializer included, so the
 * merged program called a `helper` it did not have (E0301 in a program that
 * imports the module, while `ae check` of the module passed). A reference
 * to a module function that comes, in source order, before every binding of
 * its name in the body cannot be the local: it gets the module rename here,
 * ahead of that walk.
 * Children are visited before their node, so an initializer comes before its
 * binding; a binding lasts to the end of the body (a closure's or a match
 * arm's included), which errs toward leaving a reference as it was. */
static void rename_refs_before_binding(ASTNode* node, const char* prefix,
                                       const ModuleNames* funcs,
                                       const ModuleNames* locals,
                                       ModuleNames* bound) {
    if (!node || node->type == AST_OFFSETOF) return;
    for (int i = 0; i < node->child_count; i++) {
        rename_refs_before_binding(node->children[i], prefix, funcs, locals, bound);
    }
    if (!node->value) return;
    if (node->type == AST_VARIABLE_DECLARATION || node->type == AST_PATTERN_VARIABLE ||
        node->type == AST_CONST_DECLARATION || node->type == AST_CLOSURE_PARAM) {
        /* Borrowed from binding nodes, which this pass never renames. */
        if (!names_has(bound, node->value)) names_add(bound, node->value);
        return;
    }
    if (node->type != AST_FUNCTION_CALL && node->type != AST_IDENTIFIER) return;
    if (!names_has(locals, node->value) || names_has(bound, node->value)) return;
    /* Functions only. A local named after a module const, read before its
     * binding (a loop's, from the previous pass), is the local to the
     * checker, which passes over such a local only for a function. */
    if (!names_has(funcs, node->value)) return;
    char* prefixed = merged_function_name_ns(prefix, node->value);
    free(node->value);
    node->value = prefixed;
}

static void rename_refs_before_binding_in(ASTNode* scope, const char* prefix,
                                          const ModuleNames* funcs,
                                          const ModuleNames* locals) {
    if (!locals || locals->count == 0 || !funcs || funcs->count == 0) return;
    ModuleNames bound = {0};
    rename_refs_before_binding(scope, prefix, funcs, locals, &bound);
    names_free(&bound);
}

// Recursively rename intra-module function calls and constant references in a cloned AST.
// local_names/local_count: names bound in the enclosing function (params + locals) that shadow constants.
static void rename_intra_module_refs(ASTNode* node, const char* prefix,
                                      const ModuleNames* funcs,
                                      const ModuleNames* consts,
                                      const ModuleNames* locals) {
    if (!node) return;

    /* #2320: `offsetof(T, f)` names `f` as a member of T, never as a
     * reference, so no rename applies to it (a module function or const
     * called `f` turned it into `<prefix>_f`, a member T does not have). */
    if (node->type == AST_OFFSETOF) return;

    /* #3088: in `ns.member`, a namespace the module imports is that module,
     * whatever function of its own the module names `ns`: a function has
     * no fields, as the checker reads it (value_hides_namespace). Renamed
     * to the module's function, pkg.user's own `frame()` turned its
     * `frame.SETS` into `user_frame.SETS`. A local of the name is a value,
     * and stays one. */
    if (node->type == AST_MEMBER_ACCESS && node->child_count > 0 && node->children[0] &&
        node->children[0]->type == AST_IDENTIFIER && node->children[0]->value &&
        names_has(funcs, node->children[0]->value) &&
        !names_has(consts, node->children[0]->value) &&
        !name_is_module_global_var(prefix, node->children[0]->value) &&
        !names_has(locals, node->children[0]->value)) {
        AetherModule* self = module_find_by_namespace(prefix);
        if (self && self->name && module_sees_namespace(self->name, node->children[0]->value) &&
            strcmp(module_namespace_of(self->name), node->children[0]->value) != 0)
            return;
    }

    if (node->type == AST_FUNCTION_CALL && node->value) {
        // Check if this call targets a function defined in the same module.
        /* #2637: a call through a local or parameter of that name (a
         * closure bound to `helper`, say) is the local's, as the checker
         * resolves it; renaming it to `<prefix>_helper` called the module's
         * function instead, so the build failed or ran the wrong code where
         * `ae check` of the module and a plain program ran the closure. It
         * matters more now that a glob binds an imported module's whole
         * extern surface (`trim`, `length`, ...) in a merged module. */
        int renamed = names_has(locals, node->value);
        if (!renamed && names_has(funcs, node->value)) {
            /* #3116: the module's own `free` taking one argument. The
             * checker gives a slice argument to the builtin instead, once
             * its type is known (typecheck_function_call_inner). */
            int own_free = strcmp(node->value, "free") == 0 &&
                           node->child_count == 1 && !node->annotation;
            char* prefixed = merged_function_name_ns(prefix, node->value);
            free(node->value);
            node->value = prefixed;
            if (own_free) node->annotation = strdup("own_free");
            renamed = 1;
        }
        /* #2200: a call THROUGH a module-level `var` holding a function
         * pointer (`gen_buffers(1, ids)` where `var gen_buffers: GenBuffers`
         * is the module's own state). The cell itself merges as
         * `<prefix>_gen_buffers` and its reads and writes are renamed below
         * and above, but the call kept the bare name and the checker then
         * reported "Undefined function". A local of that name shadows it. */
        if (!renamed && !names_has(locals, node->value) &&
            name_is_module_global_var(prefix, node->value)) {
            char* prefixed = mod_strf("%s_%s", prefix, node->value);
            free(node->value);
            node->value = prefixed;
            renamed = 1;
        }
        /* A method called on a module-level `var` (`views.push(h)`, a
         * slice the module grows): the receiver is the module's own cell,
         * `<prefix>_views`, as a bare read of it is; kept bare, the checker
         * found no value of the name and reported "Undefined function". A
         * local of the name is the receiver instead, and one dot only: a
         * longer path is a module's. */
        const char* dot = renamed ? NULL : strchr(node->value, '.');
        if (dot && dot != node->value && !strchr(dot + 1, '.')) {
            char* recv = mod_strf("%.*s", (int)(dot - node->value), node->value);
            if (!names_has(locals, recv) && name_is_module_global_var(prefix, recv)) {
                char* prefixed = mod_strf("%s_%s%s", prefix, recv, dot);
                free(node->value);
                node->value = prefixed;
            }
            free(recv);
        }
    }

    /* #937: a bare-name WRITE to a module-level `var` (`counter = n`) is
     * parsed as an AST_VARIABLE_DECLARATION / AST_ASSIGNMENT whose target
     * name is in node->value (not a child AST_IDENTIFIER), so the identifier
     * branch below never sees it — the write target stays bare `counter` and
     * codegen emits a shadowing local (`int counter = n;`) instead of a store
     * to the shared `<prefix>_counter` static, losing the write across the
     * import boundary. Rename the target so codegen's is_module_global_var
     * write-steering fires. Gated on `global_var` (NOT plain consts): a
     * `const` lowers to a `#define`, and a `name = expr` against it is a real
     * shadowing local that must stay local. */
    if ((node->type == AST_VARIABLE_DECLARATION || node->type == AST_ASSIGNMENT) &&
        node->value && name_is_module_global_var(prefix, node->value)) {
        char* prefixed = mod_strf("%s_%s", prefix, node->value);
        free(node->value);
        node->value = prefixed;
    }

    /* `builder name(...) with <factory>` carries the factory name in
     * the AST_BUILDER_FUNCTION node's `annotation` field (see
     * parser.c:3626 and codegen_func.c:get_builder_factory which
     * reads it back). When the builder is cloned across an import
     * boundary, the factory needs the same intra-module rename — the
     * codegen later emits `_bcfg = <annotation>()` verbatim, so
     * without this the consumer TU emits a bare `mkfac()` and gets
     * "undefined reference to mkfac" at link time even though the
     * builder itself was renamed to `<ns>_<builder>` correctly.
     * Filed in aether/new_aevg_asks.md ASK 1 from the AeVG port. */
    if (node->type == AST_BUILDER_FUNCTION && node->annotation &&
        names_has(funcs, node->annotation)) {
        char* prefixed = merged_function_name_ns(prefix, node->annotation);
        free(node->annotation);
        node->annotation = prefixed;
    }

    if (node->type == AST_IDENTIFIER && node->value) {
        // Only rename if this identifier matches a module constant AND is not
        // shadowed by a local variable or parameter in the enclosing function
        if (!names_has(locals, node->value)) {
            int renamed = 0;
            if (names_has(consts, node->value)) {
                char* prefixed = mod_strf("%s_%s", prefix, node->value);
                free(node->value);
                node->value = prefixed;
                renamed = 1;
            }
            // Also rewrite identifier-as-value references to module
            // functions (e.g. `my_handler` passed as a `ptr` argument
            // to a C extern that takes a function pointer). Without
            // this, the C compiler sees the unprefixed bare name and
            // fails with "undeclared identifier" — see #235. The
            // call-site rewrite above only catches AST_FUNCTION_CALL,
            // not value-position AST_IDENTIFIER.
            if (!renamed && names_has(funcs, node->value)) {
                char* prefixed = merged_function_name_ns(prefix, node->value);
                free(node->value);
                node->value = prefixed;
            }
        }
    }

    /* #1606: a closure introduces its own scope, so its parameters (and the
       locals in its body) shadow module functions/consts for the whole body.
       Previously only AST_FUNCTION_DEFINITION established a scope, so a
       closure parameter never made it into local_names and a same-named
       module function won the rename — silently substituting the function's
       address for the parameter's value in argument position.

       The enclosing scope's names stay in effect (a closure can read them),
       so this EXTENDS local_names rather than replacing it: parameters and
       body-locals of the closure are appended to whatever the enclosing
       function already contributed. */
    if (node->type == AST_CLOSURE || node->type == AST_QUOTE) {
        /* The closure's own bindings, then the enclosing scope's, as many
         * as there are (#3545: they were capped at 128, and a local past the
         * cap was taken for a same-named module function). */
        ModuleNames clo = {0};
        collect_local_names(node, &clo);
        /* #2683 inside the closure: its own names, where the enclosing body
         * binds none of them (that body's walk has placed those). */
        {
            ModuleNames own = {0};
            for (int i = 0; i < clo.count; i++) {
                if (!names_has(locals, clo.names[i]) &&
                    !name_is_module_global_var(prefix, clo.names[i]))
                    names_add(&own, clo.names[i]);
            }
            rename_refs_before_binding_in(node, prefix, funcs, &own);
            names_free(&own);
        }
        for (int i = 0; locals && i < locals->count; i++) {
            if (!names_has(&clo, locals->names[i])) names_add(&clo, locals->names[i]);
        }
        ModuleNames kept = {0};
        for (int i = 0; i < clo.count; i++) {
            if (!name_is_module_global_var(prefix, clo.names[i])) names_add(&kept, clo.names[i]);
        }
        names_free(&clo);
        for (int i = 0; i < node->child_count; i++) {
            rename_intra_module_refs(node->children[i], prefix, funcs, consts, &kept);
        }
        names_free(&kept);
        return;
    }

    // When entering a function definition, collect its local names for shadowing checks,
    // every one of them (#3545).
    /* A builder body is a function body: its parameters and locals shadow
     * a same-named module function exactly as a plain function's do.
     * Without AST_BUILDER_FUNCTION here a builder entered no scope, so
     * `builder java_main(main_class: string)` beside a `main_class()` setter
     * had every read of the parameter renamed to `<prefix>_main_class` -- the
     * setter's ADDRESS interpolated, compared with 1, or passed as an int
     * (aeb asks/builder-local-resolves-to-module-function.md). */
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) {
        ModuleNames nested = {0};
        collect_local_names(node, &nested);
        /* #937: collect_local_names counts a bare `var = expr` write as a
         * local (it's shaped as an AST_VARIABLE_DECLARATION). For a module
         * `global_var` that's wrong — the write is to the shared global, and
         * leaving the name in `local_names` would shadow it out of the read
         * rename below (so reads-after-write stay bare and resolve to the
         * codegen's shadowing local). Drop global_var names so reads and the
         * write target both rename. A real local shadowing a `const` is NOT a
         * global_var, so it's correctly retained. */
        ModuleNames kept = {0};
        for (int i = 0; i < nested.count; i++) {
            if (!name_is_module_global_var(prefix, nested.names[i])) names_add(&kept, nested.names[i]);
        }
        names_free(&nested);
        rename_refs_before_binding_in(node, prefix, funcs, &kept);
        for (int i = 0; i < node->child_count; i++) {
            rename_intra_module_refs(node->children[i], prefix, funcs, consts, &kept);
        }
        names_free(&kept);
        return;
    }

    for (int i = 0; i < node->child_count; i++) {
        rename_intra_module_refs(node->children[i], prefix, funcs, consts, locals);
    }
}

// Is a function with the given (already-prefixed) name present in the program?
static int program_has_function(ASTNode* program, const char* prefixed_name) {
    return (ast_decl_kinds(program, prefixed_name) & DECL_FN) != 0;
}

/* #2643: the definitions one module's merge pass has cloned so far. A
 * function written as several clauses (`f(0) -> ...`, `f(n) -> ...`) is
 * several definitions of one name, so a name the same pass merged a moment
 * ago is a further clause to merge, not a duplicate: only a definition the
 * program held before the pass (its own, an earlier import of the module,
 * a revisit) makes the module's one a duplicate. program_has_function alone
 * skipped every clause after the first, and an imported clause set kept
 * only its first clause. #3546: counted by name, so the question is two
 * probes, not a walk of the program and of the pass. */
typedef struct {
    StrMap merged;   /* name -> (intptr_t) definitions of it this pass merged */
} MergePass;

static void merge_pass_add(MergePass* pass, ASTNode* clone) {
    if (!clone || !clone->value) return;
    intptr_t n = (intptr_t)strmap_get(&pass->merged, clone->value);
    strmap_put(&pass->merged, clone->value, (void*)(n + 1));
}

static void merge_pass_free(MergePass* pass) {
    strmap_free(&pass->merged);
}

/* Is a function of the given prefixed name in the program, other than one
 * this pass merged? */
static int program_has_function_before(ASTNode* program, const char* prefixed_name,
                                       const MergePass* pass) {
    if (!program || !prefixed_name) return 0;
    DeclEntry* e = strmap_get(&decl_index(program)->names, prefixed_name);
    intptr_t merged = (intptr_t)strmap_get(&pass->merged, prefixed_name);
    return e && e->fn_count > merged;
}

// Is a constant with the given prefixed name already a top-level child of
// `program`? Mirrors program_has_function for the const-clone path — needed
// because the merge loop can now revisit a synthetic import (aether#1009), and
// re-cloning a module's consts without this guard emits a duplicate C
// definition (`redefinition of 'sha2_K256'`).
static int program_has_const(ASTNode* program, const char* prefixed_name) {
    return (ast_decl_kinds(program, prefixed_name) & DECL_CONST) != 0;
}

/* Is a fault member with the given (already-prefixed) C symbol name already
 * present in a merged AST_FAULT_DEFINITION? Dedup guard for a module imported
 * more than once — without it the member's `static const char` would be
 * emitted twice (duplicate C definition). */
static int program_has_fault_member(ASTNode* program, const char* prefixed_name) {
    return (ast_decl_kinds(program, prefixed_name) & DECL_FAULT_MEMBER) != 0;
}

// The definition of kind `node_type` named `name` among `program`'s top-level
// children, or NULL. The merge dedups imported definitions with it: a
// struct by its merged name (`<ns>_<Name>`, see ss_build_table), the other
// kinds by the name they were written with.
static ASTNode* program_find_def(ASTNode* program, ASTNodeType node_type, const char* name) {
    if (!program || !name) return NULL;
    char* key = decl_typed_key((int)node_type, name);
    ASTNode* def = strmap_get(&decl_index(program)->typed, key);
    free(key);
    return def;
}

/* Structural equality of two definitions: node kind, name, annotation,
 * bit width, type and children, one for one — what the program's text
 * says, not where it says it. Struct-typed fields compare by name, as
 * the language does; a differing nested struct is reported when IT is
 * merged. */
static int ast_defs_equal(ASTNode* a, ASTNode* b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    if ((a->value == NULL) != (b->value == NULL)) return 0;
    if (a->value && strcmp(a->value, b->value) != 0) return 0;
    if ((a->annotation == NULL) != (b->annotation == NULL)) return 0;
    if (a->annotation && strcmp(a->annotation, b->annotation) != 0) return 0;
    if (a->bit_width != b->bit_width) return 0;
    if (!types_equal(a->node_type, b->node_type)) return 0;
    if (a->child_count != b->child_count) return 0;
    for (int i = 0; i < a->child_count; i++) {
        if (!ast_defs_equal(a->children[i], b->children[i])) return 0;
    }
    return 1;
}

/* Render a definition's field list for a diagnostic: `{v: Vec3, s: float}`. */
static void struct_fields_summary(ASTNode* def, char* out, size_t size) {
    size_t off = 0;
    off += (size_t)snprintf(out + off, size - off, "{");
    for (int i = 0; i < def->child_count && off + 4 < size; i++) {
        ASTNode* f = def->children[i];
        if (!f) continue;
        const char* tn = f->node_type ? type_spelled(f->node_type) : "?";
        off += (size_t)snprintf(out + off, size - off, "%s%s: %s",
                                i ? ", " : "", f->value ? f->value : "?", tn);
    }
    if (off + 2 < size) snprintf(out + off, size - off, "}");
    else { out[size - 4] = '.'; out[size - 3] = '.'; out[size - 2] = '}'; out[size - 1] = 0; }
}

/* #2129: struct, message and actor names are one namespace across
 * modules, and the merge keeps the first definition it saw. Two modules
 * defining one name the same way share the definition; two DIFFERENT
 * definitions used to merge silently — the loser's own code then failed
 * in the typechecker ("Struct 'Quat' has no field 's'") or the C
 * compiler, or, for an actor, the loser's `spawn` quietly built the
 * winner and its messages went unanswered. The same definition reached
 * twice (a module imported directly and transitively) is the same file
 * and line and is skipped as before. Returns 1 when `decl` is a clash
 * (reported) or a duplicate to skip; 0 when the name is new. */
static int definition_already_merged(ASTNode* program, ASTNode* decl, const char* what) {
    ASTNode* existing = program_find_def(program, decl->type, decl->value);
    if (!existing) return 0;
    if (existing->source_file && decl->source_file &&
        strcmp(existing->source_file, decl->source_file) == 0 &&
        existing->line == decl->line) return 1;
    if (ast_defs_equal(existing, decl)) return 1;
    char msg[900];
    const char* here = decl->source_file ? decl->source_file : "<module>";
    const char* there = existing->source_file ? existing->source_file : "<program>";
    if (decl->type == AST_ACTOR_DEFINITION) {
        snprintf(msg, sizeof(msg),
                 "%s '%s' is defined in two modules: %s and %s. Actor names are one "
                 "namespace across modules, so `spawn(%s())` in either would build the "
                 "same actor; rename one of them",
                 what, decl->value, here, there, decl->value);
    } else if (decl->type == AST_STRUCT_DEFINITION) {
        /* Struct types are module-scoped, each merged as `<ns>_<Name>`;
         * only an `--emit=lib` build's exported structs keep their plain
         * C name (ss_build_table), and two of those can still meet. */
        char have[256], want[256];
        struct_fields_summary(existing, have, sizeof(have));
        struct_fields_summary(decl, want, sizeof(want));
        snprintf(msg, sizeof(msg),
                 "struct '%s' is defined differently in two modules: %s in %s and %s in %s. "
                 "In an --emit=lib build a struct its module exports keeps its plain C "
                 "name, the library's ABI, so both would be one C type; rename one of "
                 "them, stop exporting one, or make the definitions identical",
                 decl->value, want, here, have, there);
    } else {
        char have[256], want[256];
        struct_fields_summary(existing, have, sizeof(have));
        struct_fields_summary(decl, want, sizeof(want));
        snprintf(msg, sizeof(msg),
                 "%s '%s' is defined differently in two modules: %s in %s and %s in %s. "
                 "%s names are one namespace across modules, so both would share "
                 "one layout; rename one of them (or make the definitions identical)",
                 what, decl->value, want, here, have, there,
                 "Message");
    }
    AetherError e = { decl->source_file, NULL, decl->line, decl->column, msg, NULL, NULL,
                      AETHER_ERR_TYPE_MISMATCH };
    aether_error_report(&e);
    return 1;
}

/* #908: a `type X = distinct Base` def already present in the program (the
 * consumer's own, or an earlier merge). Used to dedup distinct-def merges. */
static int program_has_distinct(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_DISTINCT_TYPE_DEF &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* #2200: a `cfn Name(...) -> R` def already present in the program (the
 * consumer's own, or an earlier merge). Used to dedup cfn-def merges. */
static int program_has_cfn(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_CFN_TYPE_DEF &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* A `bitstruct Name : <backing> { ... }` def already present in the program
 * (the consumer's own, or an earlier merge). Used to dedup bitstruct merges. */
static int program_has_bitstruct(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_BITSTRUCT_DEFINITION &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* An `enum X { ... }` already present in the program (the consumer's own, or
 * an earlier merge). Used to dedup enum merges. */
static int program_has_enum(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_ENUM_DEFINITION &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* A `type X = A | B | C` sum-type def already present in the program (the
 * consumer's own, or an earlier merge). Used to dedup sum-type merges. */
static int program_has_sum(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_SUM_TYPE_DEF &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Apply M's own `import X (a, b, c)` selective-import rewrites to a body
// that's about to be cloned out of M into the consumer's program AST.
//
// Without this, a module that does `import std.http.client (set_header,
// send_request)` and exposes `do_get()` calling `set_header(req, ...)`
// type-checks fine standalone — M's symbol table aliases `set_header` to
// `client_set_header`. But once M is imported into a consumer, the
// consumer clones `do_get`'s AST verbatim. The bare `set_header(...)`
// call in the cloned body has no binding in the consumer's symbol
// table (the consumer never wrote `import std.http.client`) and the
// typer fires E0301: Undefined function 'set_header'.
//
// Fix: walk M's AST for AST_IMPORT_STATEMENTs that carry a selection
// list, and for each selected name rewrite bare references in the
// cloned body to the prefixed form (`set_header` → `client_set_header`).
// The transitive cross-module BFS in module_merge_into_program already
// pulls those prefixed definitions into the consumer's program AST, so
// the consumer's typer can now resolve them.
//
// Bypassed names: nodes annotated `module_alias` (the parser uses the
// same AST_IDENTIFIER child shape for `import X as Y`).
//
// Glob (`*`) imports stamped `glob_import` aren't handled here because
// the selection list is computed at typecheck time from the source
// module's exports, not stored on the AST. Most ports use the explicit
// selective form, so deferring glob to a follow-up PR.
/* Rename references bound through a per-symbol import alias
 * (`import X (name as alias)`) inside a cloned module body: bare
 * `alias(...)` calls become `<ns>_<name>(...)`; when the aliased
 * symbol is a const, bare `alias` identifiers rename the same way.
 * Mirrors the two rename branches of rename_intra_module_refs. */
static void rename_aliased_import_refs(ASTNode* node, const char* alias,
                                       const char* prefixed, int is_const) {
    if (!node) return;
    if (node->type == AST_OFFSETOF) return;   /* #2320: its field is a member name */
    if (node->type == AST_FUNCTION_CALL && node->value &&
        strcmp(node->value, alias) == 0) {
        free(node->value);
        node->value = strdup(prefixed);
    } else if (is_const && node->type == AST_IDENTIFIER && node->value &&
               strcmp(node->value, alias) == 0) {
        free(node->value);
        node->value = strdup(prefixed);
    }
    for (int i = 0; i < node->child_count; i++) {
        rename_aliased_import_refs(node->children[i], alias, prefixed, is_const);
    }
}

/* Does the scope `node` (a function, a closure) bind `name`, as a
 * parameter, a local or a constant of its own (collect_local_names)? */
static int scope_binds_name(ASTNode* node, const char* name) {
    ModuleNames bound = {0};
    collect_local_names(node, &bound);
    int has = names_has(&bound, name);
    names_free(&bound);
    return has;
}

/* #2209: rewrite the qualified prefix a scope WROTE for an import to the
 * namespace the import's symbols actually carry after the merge.
 *
 *   import mine.vk                      vk.mine()      -> mine_vk.mine()
 *   import gfx.vk as vkapi              vkapi.ok()     -> gfx_vk.ok()
 *
 * The rewrite is what makes a module's imports resolve IN THAT MODULE, by
 * their full path or alias, instead of program-wide by last segment: once
 * a body reaches the program it names the module it meant, whatever the
 * entry program or a sibling library calls its own `vk`. It runs on the
 * program's own code (module_merge_into_program) and on every cloned
 * module body (apply_inherited_selective_imports), before the typechecker
 * sees either.
 *
 * `from` is skipped on AST_MEMBER_ACCESS when it binds a parameter or local
 * there AND the target module doesn't export a member of that name: `vk.field`
 * on a local struct is a field read, the same shadowing rule
 * rename_intra_module_refs and the typechecker apply. But a real struct field
 * and a module's export list are never the same query — `region.LIVE_RASTER`
 * with `region: ptr` a parameter can't be a field of an opaque pointer, and
 * IS how this codebase spells a qualified constant reference through a
 * same-named wrapper — so when `to`'s module exports the member name, that
 * reading wins over the shadow guard. AST_FUNCTION_CALL and AST_IDENTIFIER
 * are different: the parser only ever produces a dotted `from.suffix` value
 * for those two node types from literal qualified-call syntax
 * (`from.suffix(...)` or a bare reference to it as a value) — Aether has no
 * method-call sugar that would let a local value named `from` change what
 * that syntax means — so those two rewrite unconditionally. A local or
 * parameter named identically to a module is a real, common pattern here
 * (`region_set_animated(region: ptr, on: int) { region.region_set_animated(region, on) }`,
 * a same-named forwarding wrapper): skipping the whole node for it used to
 * be harmless because no rename was needed without a collision, but a
 * collision-driven rename now needs exactly this call rewritten, and used
 * to skip it, leaving `region.` unrewritten and misresolved. */
static void rewrite_qualified_prefix(ASTNode* node, const char* from, const char* to,
                                     int shadowed) {
    if (!node) return;
    size_t from_len = strlen(from);

    if ((node->type == AST_FUNCTION_CALL || node->type == AST_IDENTIFIER) &&
        node->value && strncmp(node->value, from, from_len) == 0 &&
        node->value[from_len] == '.') {
        size_t len = strlen(to) + strlen(node->value + from_len) + 1;
        char* renamed = malloc(len);
        if (renamed) {
            snprintf(renamed, len, "%s%s", to, node->value + from_len);
            free(node->value);
            node->value = renamed;
        }
    }
    if (node->type == AST_MEMBER_ACCESS && node->child_count > 0 && node->value) {
        ASTNode* base = node->children[0];
        int target_exports_it = shadowed &&
            module_exports_symbol(module_find_by_namespace(to), node->value);
        if (base && base->type == AST_IDENTIFIER && base->value &&
            strcmp(base->value, from) == 0 && (!shadowed || target_exports_it)) {
            free(base->value);
            base->value = strdup(to);
        }
    }

    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE || node->type == AST_QUOTE) {
        /* #3545: whether the scope binds `from`, however many names it binds
         * (its locals were read into 128 slots, and one past them was not
         * seen to shadow the module). */
        int here = shadowed || scope_binds_name(node, from);
        for (int i = 0; i < node->child_count; i++) {
            rewrite_qualified_prefix(node->children[i], from, to, here);
        }
        return;
    }
    for (int i = 0; i < node->child_count; i++) {
        rewrite_qualified_prefix(node->children[i], from, to, shadowed);
    }
}

/* Same job as rewrite_qualified_prefix, for the case where TWO OR MORE of a
 * scope's imports reduce to the SAME written prefix (two same-last-segment
 * modules, at least one imported unaliased, neither aliased to tell them
 * apart in the source text). rewrite_qualified_prefix alone would let
 * whichever import rewrite_import_prefixes processes first claim every
 * `written.name` in the body — including ones the OTHER import's qualified
 * calls meant — silently misrouting them to a module that never defined
 * them (E0303/E0301 naming the wrong module, or worse, a same-named export
 * on the wrong side going uncaught). Each occurrence is instead resolved by
 * which CANDIDATE actually exports that specific suffix name; first match
 * among `candidates` wins (declaration order), and an occurrence none of
 * them export is left as written, so it fails with the ORIGINAL prefix in
 * its diagnostic rather than one silently swapped in for it. */
static void rewrite_qualified_prefix_ambiguous(ASTNode* node, const char* from,
                                               AetherModule** candidates, int ncandidates,
                                               int shadowed) {
    if (!node) return;
    size_t from_len = strlen(from);

    /* Same reasoning as rewrite_qualified_prefix: a dotted AST_FUNCTION_CALL/
     * AST_IDENTIFIER value is qualified-call syntax the parser committed to,
     * unaffected by `from` also being a local/parameter here, so no shadow
     * check gates this branch. */
    if ((node->type == AST_FUNCTION_CALL || node->type == AST_IDENTIFIER) &&
        node->value && strncmp(node->value, from, from_len) == 0 &&
        node->value[from_len] == '.') {
        const char* suffix = node->value + from_len + 1;
        for (int i = 0; i < ncandidates; i++) {
            if (!candidates[i] || !module_exports_symbol(candidates[i], suffix)) continue;
            const char* to = module_namespace_of(candidates[i]->name);
            size_t len = strlen(to) + strlen(node->value + from_len) + 1;
            char* renamed = malloc(len);
            if (renamed) {
                snprintf(renamed, len, "%s%s", to, node->value + from_len);
                free(node->value);
                node->value = renamed;
            }
            break;
        }
    }
    if (node->type == AST_MEMBER_ACCESS && node->child_count > 0 && node->value) {
        ASTNode* base = node->children[0];
        if (base && base->type == AST_IDENTIFIER && base->value &&
            strcmp(base->value, from) == 0) {
            for (int i = 0; i < ncandidates; i++) {
                if (!candidates[i]) continue;
                if (shadowed && !module_exports_symbol(candidates[i], node->value)) continue;
                free(base->value);
                base->value = strdup(module_namespace_of(candidates[i]->name));
                break;
            }
            /* A shadowed base none of the candidates export the member of is
             * left as written — same policy as an unmatched suffix above and
             * as rewrite_qualified_prefix's shadow guard: it's a real field
             * read on the local, not a qualified reference. */
        }
    }

    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE || node->type == AST_QUOTE) {
        /* #3545: whether the scope binds `from`, however many names it binds
         * (its locals were read into 128 slots, and one past them was not
         * seen to shadow the module). */
        int here = shadowed || scope_binds_name(node, from);
        for (int i = 0; i < node->child_count; i++) {
            rewrite_qualified_prefix_ambiguous(node->children[i], from, candidates, ncandidates,
                                               here);
        }
        return;
    }
    for (int i = 0; i < node->child_count; i++) {
        rewrite_qualified_prefix_ambiguous(node->children[i], from, candidates, ncandidates,
                                           shadowed);
    }
}

/* The prefix a scope writes for one of its imports: the `as` alias when
 * there is one, else the import path's last segment. */
static const char* import_written_prefix(ASTNode* imp) {
    for (int k = 0; k < imp->child_count; k++) {
        ASTNode* c = imp->children[k];
        if (c && c->type == AST_IDENTIFIER && c->value && c->annotation &&
            strcmp(c->annotation, "module_alias") == 0)
            return c->value;
    }
    return module_last_segment(imp->value);
}

/* #2209: for every import `scope_ast` declares, rewrite `body`'s uses of the
 * written prefix to the imported module's namespace (a no-op when the two
 * already agree, which is every unaliased import whose last segment no other
 * loaded module shares). `self_name` is the module `scope_ast` belongs to,
 * NULL for the entry program: an import written with the module's OWN last
 * segment (module `audio` importing `std.audio`, #1780) is left alone, so
 * `audio.x` there keeps resolving to the module itself, as documented.
 *
 * Collected into one pass, rather than rewritten import-by-import, so a
 * SCOPE that writes two same-last-segment imports unaliased (`vg.geom.region`
 * and `vg.region`, both renamed off "region" by module_assign_namespaces)
 * doesn't have the first one's rewrite blindly consume every "region.name"
 * in the body before the second's ever runs — see
 * rewrite_qualified_prefix_ambiguous. The imports are collected once per
 * scope (#3499): collected again for each of a program's top-level nodes,
 * the merge grew with the square of the program. */
typedef struct {
    /* One entry per import whose written prefix is not its namespace, as
     * many as the scope has (#3545: they were capped at 64, and an import
     * past the cap kept a prefix nothing binds). */
    const char** writtens;
    AetherModule** mods;
    int n, cap;
} ImportPrefixes;

static void import_prefixes_free(ImportPrefixes* ip) {
    free(ip->writtens);
    free(ip->mods);
    ip->writtens = NULL;
    ip->mods = NULL;
    ip->n = ip->cap = 0;
}

static void import_prefixes_collect(ImportPrefixes* ip, ASTNode* scope_ast,
                                    const char* self_name) {
    ip->writtens = NULL;
    ip->mods = NULL;
    ip->n = ip->cap = 0;
    /* #3546: a module's imports from its index (a module's top level does
     * not change while it merges); the program's, which the merge inserts
     * into, are read in program order. */
    DeclIndex* dix = self_name && scope_ast ? decl_index(scope_ast) : NULL;
    int n = dix ? dix->import_count : scope_ast ? scope_ast->child_count : 0;
    for (int i = 0; i < n; i++) {
        ASTNode* imp = dix ? dix->imports[i] : scope_ast->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        if (imp->annotation && strcmp(imp->annotation, "synthetic") == 0) continue;
        const char* written = import_written_prefix(imp);
        const char* ns = module_namespace_of(imp->value);
        if (strcmp(written, ns) == 0) continue;
        if (self_name && strcmp(written, module_last_segment(self_name)) == 0) continue;
        if (ip->n == ip->cap) {
            ip->cap = ip->cap ? ip->cap * 2 : 16;
            ip->writtens = aether_xrealloc(ip->writtens, (size_t)ip->cap * sizeof(*ip->writtens));
            ip->mods = aether_xrealloc(ip->mods, (size_t)ip->cap * sizeof(*ip->mods));
        }
        ip->writtens[ip->n] = written;
        ip->mods[ip->n] = module_find(imp->value);
        ip->n++;
    }
}

static void import_prefixes_rewrite(const ImportPrefixes* ip, ASTNode* body) {
    if (!body || ip->n == 0) return;
    int* done = calloc((size_t)ip->n, sizeof(int));
    AetherModule** candidates = malloc((size_t)ip->n * sizeof(*candidates));
    if (!done || !candidates) {
        fprintf(stderr, "Fatal: out of memory\n");
        exit(1);
    }
    for (int i = 0; i < ip->n; i++) {
        if (done[i]) continue;
        int ncand = 0;
        candidates[ncand++] = ip->mods[i];
        done[i] = 1;
        for (int j = i + 1; j < ip->n; j++) {
            if (!done[j] && strcmp(ip->writtens[i], ip->writtens[j]) == 0) {
                candidates[ncand++] = ip->mods[j];
                done[j] = 1;
            }
        }
        if (ncand == 1) {
            if (ip->mods[i]) rewrite_qualified_prefix(body, ip->writtens[i], module_namespace_of(ip->mods[i]->name), 0);
        } else {
            rewrite_qualified_prefix_ambiguous(body, ip->writtens[i], candidates, ncand, 0);
        }
    }
    free(done);
    free(candidates);
}

static void rewrite_import_prefixes(ASTNode* body, ASTNode* scope_ast,
                                    const char* self_name) {
    if (!body || !scope_ast) return;
    ImportPrefixes ip;
    import_prefixes_collect(&ip, scope_ast, self_name);
    import_prefixes_rewrite(&ip, body);
    import_prefixes_free(&ip);
}

/* The registered module whose parsed AST is `mod_ast`, or NULL. */
static AetherModule* module_owning_ast(ASTNode* mod_ast) {
    if (!global_module_registry || !mod_ast) return NULL;
    for (int i = 0; i < global_module_registry->module_count; i++) {
        AetherModule* m = global_module_registry->modules[i];
        if (m && m->ast == mod_ast) return m;
    }
    return NULL;
}

/* 1 when `name` is the bare form of one of `sub_ast`'s externs, the
 * `<sub_ns>_<name>` a qualified `sub_ns.name` or a glob import binds
 * (std.http's `extern http_server_create` is `server_create`). */
static int module_extern_short_name(ASTNode* sub_ast, const char* sub_ns, const char* name) {
    if (!sub_ast || !sub_ns || !name || !*name || name[0] == '_') return 0;
    /* #3546: one probe of the module's index for `<sub_ns>_<name>`. */
    size_t n = strlen(sub_ns) + strlen(name) + 2;
    char* full = malloc(n);
    if (!full) {
        fprintf(stderr, "Fatal: out of memory\n");
        exit(1);
    }
    snprintf(full, n, "%s_%s", sub_ns, name);
    int found = (ast_decl_kinds(sub_ast, full) & DECL_EXTERN) != 0;
    free(full);
    return found;
}

static void apply_inherited_selective_imports(ASTNode* clone, ASTNode* mod_ast) {
    if (!clone || !mod_ast) return;

    /* #2209: the module's qualified uses name the modules IT imported. */
    AetherModule* owner = module_owning_ast(mod_ast);
    rewrite_import_prefixes(clone, mod_ast, owner ? owner->name : NULL);

    /* #3546: the module's imports from its index, where a walk of its
     * whole top level for each declaration cloned out of it made the merge
     * of a large module grow with the square of its size. */
    DeclIndex* dix = decl_index(mod_ast);
    for (int ii = 0; ii < dix->import_count; ii++) {
        ASTNode* imp = dix->imports[ii];
        if (!imp->value) continue;

        // #896: a glob import (`import M (*)`) brings every exported name of M
        // into the module's bare scope. Its selection list is NOT on the AST
        // (it's computed at typecheck time from M's exports), so it's handled
        // separately below — the selection is M's full export set rather than
        // the explicit selector children. A selective import needs its
        // identifier children; a glob has none.
        int is_glob = (imp->annotation &&
                       strcmp(imp->annotation, "glob_import") == 0);
        if (!is_glob && !import_has_selection(imp)) continue;

        // Resolve the import path to the merged-name prefix the
        // transitive pass will / has used (`std.http.client` →
        // `client`, `mymath` → `mymath`, etc.). Same rule used at
        // every other merge site in this file.
        const char* sub_ns = module_get_namespace(imp->value);

        // Resolve the source module's func/const tables so we can
        // classify each selector. If the source isn't loaded yet
        // (orchestrator hasn't visited it), skip — the cloned body
        // would fail to link anyway and the user's direct error
        // points at the missing module, not this rename.
        AetherModule* sub_mod = module_find(imp->value);
        if (!sub_mod || !sub_mod->ast) continue;

        const ModuleNames* sub_funcs = module_func_names(sub_mod);
        const ModuleNames* sub_consts = module_const_names(sub_mod);
        const char** sub_func_names = sub_funcs->names;
        int sub_func_count = sub_funcs->count;
        const char** sub_const_names = sub_consts->names;
        int sub_const_count = sub_consts->count;

        ModuleNames sel_funcs = {0};
        ModuleNames sel_consts = {0};

        if (is_glob) {
            // #896: the glob selection is M's whole export surface. Take every
            // func/const M defines (skipping leading-underscore privates, the
            // same privacy rule the typecheck-time glob expansion applies) so
            // a bare `clean(...)` in M's merged body is rewritten to the
            // prefixed `fs_clean(...)` the transitive pass pulls in — exactly
            // what the selective and qualified forms already get.
            //
            // #2632: a name M defines itself is not taken from the glob, as
            // the typechecker does not bind it for a program either: M's own
            // definition wins. Its functions and constants were renamed to
            // `<M>_<name>` before this runs; this keeps M's own externs, which
            // keep their bare name, from being rewritten to the glob's.
            for (int k = 0; k < sub_func_count; k++) {
                if (sub_func_names[k] && sub_func_names[k][0] != '_' &&
                    !module_defines_own_name(mod_ast, sub_func_names[k])) {
                    names_add(&sel_funcs, sub_func_names[k]);
                }
            }
            for (int k = 0; k < sub_const_count; k++) {
                if (sub_const_names[k] && sub_const_names[k][0] != '_' &&
                    !module_defines_own_name(mod_ast, sub_const_names[k])) {
                    names_add(&sel_consts, sub_const_names[k]);
                }
            }
            /* #2637: and its extern-backed names, as the checker's glob binds
             * them for a program or for M checked on its own: an extern
             * `<sub_ns>_<name>` (std.string's `string_length`) is the glob's
             * bare `<name>`. Only Aether functions were taken, so M's bare
             * `length("xy")` stayed bare in every build and failed E0301
             * while `ae check` of M passed. The rename below turns it into
             * `<sub_ns>_<name>`, the extern itself. Same privacy and #2632
             * rules as the functions above. */
            size_t sub_ns_len = strlen(sub_ns);
            for (int j = 0; j < sub_mod->ast->child_count; j++) {
                ASTNode* ext = unwrap_export(sub_mod->ast->children[j]);
                if (!ext || ext->type != AST_EXTERN_FUNCTION || !ext->value) continue;
                if (strncmp(ext->value, sub_ns, sub_ns_len) != 0 ||
                    ext->value[sub_ns_len] != '_') continue;
                const char* tail = ext->value + sub_ns_len + 1;
                if (!*tail || tail[0] == '_') continue;
                if (module_defines_own_name(mod_ast, tail)) continue;
                if (names_has(&sel_funcs, tail)) continue;
                names_add(&sel_funcs, tail);
            }
        } else {
            for (int k = 0; k < imp->child_count; k++) {
                ASTNode* sel = imp->children[k];
                if (!sel || sel->type != AST_IDENTIFIER || !sel->value) continue;
                if (sel->annotation && strcmp(sel->annotation, "module_alias") == 0) continue;
                /* Per-symbol alias inside a module's own import: the
                 * cloned body's bare refs use the ALIAS; rewrite them
                 * straight to the prefixed EXPORTED name (the generic
                 * name->ns_name pass below cannot express the rename,
                 * so aliased entries get a dedicated pass here). */
                if (sel->annotation &&
                    strncmp(sel->annotation, "select_alias:", 13) == 0) {
                    int is_const = names_has(sub_consts, sel->value);
                    char* prefixed = is_const ? mod_strf("%s_%s", sub_ns, sel->value)
                                              : merged_function_name(sub_ns, sub_mod->ast, sel->value);
                    rename_aliased_import_refs(clone, sel->annotation + 13, prefixed, is_const);
                    free(prefixed);
                    continue;
                }
                if (names_has(sub_funcs, sel->value)) {
                    names_add(&sel_funcs, sel->value);
                } else if (module_extern_short_name(sub_mod->ast, sub_ns, sel->value) &&
                           !module_defines_own_name(mod_ast, sel->value)) {
                    /* An extern-backed name (`import std.http (server_create)`
                     * for `extern http_server_create`): renamed to the extern,
                     * as the glob form does since #2637. */
                    names_add(&sel_funcs, sel->value);
                } else if (names_has(sub_consts, sel->value)) {
                    names_add(&sel_consts, sel->value);
                }
            }
        }

        if (sel_funcs.count > 0 || sel_consts.count > 0)
            rename_intra_module_refs(clone, sub_ns,
                                     &sel_funcs, &sel_consts, NULL);
        names_free(&sel_funcs);
        names_free(&sel_consts);
    }
}

// Insert a node into program->children at a specific index, shifting others right.
static void insert_child_at(ASTNode* parent, ASTNode* child, int index) {
    if (!parent || !child) return;
    ASTNode** new_children = realloc(parent->children, (parent->child_count + 1) * sizeof(ASTNode*));
    if (!new_children) { fprintf(stderr, "Fatal: out of memory\n"); exit(1); }
    parent->children = new_children;
    parent->child_capacity = 0;
    // Shift elements right
    for (int i = parent->child_count; i > index; i--) {
        parent->children[i] = parent->children[i - 1];
    }
    parent->children[index] = child;
    parent->child_count++;
    decl_index_note_insert(parent, child, index);   /* #3546 */
}

/* #2614: record the module a merged declaration was written in, so the
 * checker resolves its qualified names against that module's imports. */
static void stamp_origin(ASTNode* clone, const AetherModule* m) {
    if (!clone || !m || !m->name) return;
    free(clone->origin_module);
    clone->origin_module = strdup(m->name);
}

// Merge pure Aether module functions into the main program AST.
// For each non-stdlib import, clones function definitions with namespace-prefixed
// names and inserts them before main() so constants and functions are available.
/* #870: a merged module's own `import` statements are not cloned into the
 * program (only its function/const/struct bodies are). When a merged module
 * bare-imports a stdlib/local module, the QUALIFIED-call surface for that
 * module (`string.concat`, `json.stringify`, ...) used inside the merged
 * body must stay resolvable: the namespace has to be VISIBLE in the merged
 * compilation unit even if the entry file never imported that module itself.
 *
 * Re-establish that visibility by injecting a synthetic BARE import (no
 * selection children) for each module the merged module imported. The
 * typechecker's import pass registers the namespace so qualified calls
 * resolve for the whole merged unit. The "synthetic" annotation keeps the
 * namespace out of the user-explicit registry (issue #243 sealed-scope
 * isolation): it is visible to merged bodies, not re-granted to user code
 * that never imported it. Deduped against any bare import the program already
 * carries (user-written or earlier-injected).
 *
 * (#878: a selective import no longer restricts the qualified surface, so
 * this injection is purely about namespace visibility across the merge, not
 * about overriding a per-module selective filter.) */
static void inject_synthetic_bare_imports_from(ASTNode* program,
                                               ASTNode* mod_ast,
                                               int* insert_idx) {
    if (!program || !mod_ast || !insert_idx) return;
    for (int j = 0; j < mod_ast->child_count; j++) {
        ASTNode* imp = mod_ast->children[j];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        /* A selective import carries AST_IDENTIFIER selection children; only
         * bare imports re-open the qualified surface. */
        if (import_has_selection(imp)) continue;
        /* Dedup: skip if the program already carries a bare import of this
         * path (so two merged modules importing the same one inject once). */
        int already = 0;
        for (int p = 0; p < program->child_count; p++) {
            ASTNode* pc = program->children[p];
            if (pc && pc->type == AST_IMPORT_STATEMENT && pc->value &&
                strcmp(pc->value, imp->value) == 0 &&
                !import_has_selection(pc)) {
                already = 1;
                break;
            }
        }
        if (already) continue;
        ASTNode* synth = create_ast_node(AST_IMPORT_STATEMENT, imp->value, 0, 0);
        synth->annotation = strdup("synthetic");
        insert_child_at(program, synth, (*insert_idx)++);
    }
}

/* ---- Module-scoped struct types -------------------------------------------
 *
 * A struct belongs to the module that defines it. `gpu.Shader` and
 * `core.Shader` are two types, and a program may use both: each module's
 * struct is merged under `<ns>_<Name>`, the namespace its functions carry
 * (`gpu_Shader`), which is also its C name. The program's own structs keep
 * their names. Before this, struct names were one namespace across modules
 * and two modules defining one name differently was a clash (#2129).
 *
 * A struct name is resolved where it is written, the way a function's is:
 *   - qualified, `gpu.Shader` / `*gpu.Shader` / `gpu.Shader { ... }`: the
 *     module the qualifier names in that file (its imports, by namespace,
 *     path or alias, or the file's own module), or the module a re-export
 *     there leads to (#924);
 *   - bare, `Shader`: the file's own struct of that name; else the one
 *     struct of that name among the modules the file imports (a selective
 *     `import gpu (Shader)` decides between several); else, as before, the
 *     one struct of that name in the build. Two candidates are an error
 *     that names them and asks for the qualified spelling.
 *
 * Two modules defining one name IDENTICALLY (same fields, each field's type
 * resolving to the same type) still share one type, as #2129 made them: the
 * later definition resolves to the earlier one. So does an imported struct
 * identical to one of the program's own.
 *
 * `--emit=lib`: a struct a module exports keeps its plain C name in a
 * library build, since a C host or a header may spell it (the ABI note in
 * AGENTS.md); two exported structs of one name in one library are still the
 * #2129 clash. Only unexported module structs are mangled there.
 *
 * `extern struct`s name C's own types and are never renamed. */

typedef struct {
    AetherModule* mod;     /* NULL: the program's own */
    const char* name;      /* as written, interned */
    const char* cname;     /* the merged and C name, interned */
    ASTNode* decl;         /* the definition as parsed */
    int shared;            /* takes an earlier identical definition's name */
} ScopedStruct;

static ScopedStruct* g_ss = NULL;
static int g_ss_count = 0, g_ss_cap = 0;
static int g_ss_emit_lib = 0;
static ASTNode* g_ss_program = NULL;

void module_set_emit_lib(int on) { g_ss_emit_lib = on ? 1 : 0; }

static void ss_reset(void) {
    free(g_ss);
    g_ss = NULL;
    g_ss_count = g_ss_cap = 0;
}

static int ss_is_scoped_struct(const ASTNode* d) {
    return d && d->type == AST_STRUCT_DEFINITION && d->value &&
           !(d->annotation && strncmp(d->annotation, "extern", 6) == 0);
}

static void ss_add(AetherModule* m, ASTNode* decl, const char* cname) {
    if (g_ss_count == g_ss_cap) {
        g_ss_cap = g_ss_cap ? g_ss_cap * 2 : 64;
        g_ss = aether_xrealloc(g_ss, (size_t)g_ss_cap * sizeof(ScopedStruct));
    }
    ScopedStruct* s = &g_ss[g_ss_count++];
    s->mod = m;
    s->name = aether_intern(decl->value);
    s->cname = aether_intern(cname);
    s->decl = decl;
    s->shared = 0;
}

static ScopedStruct* ss_find(const AetherModule* m, const char* name) {
    for (int i = 0; i < g_ss_count; i++)
        if (g_ss[i].mod == m && strcmp(g_ss[i].name, name) == 0) return &g_ss[i];
    return NULL;
}

/* Does `ast` define a type other than a scoped struct under `name` (an enum,
 * sum, distinct, cfn, bitstruct, C struct or `extern struct`)? Then a bare
 * `name` written there is that type, not some module's struct. */
static int ss_ast_has_other_type(const ASTNode* ast, const char* name) {
    for (int i = 0; ast && i < ast->child_count; i++) {
        const ASTNode* d = ast->children[i];
        if (d && d->type == AST_EXPORT_STATEMENT && d->child_count > 0) d = d->children[0];
        if (!d || !d->value || strcmp(d->value, name) != 0) continue;
        switch (d->type) {
            case AST_STRUCT_DEFINITION:
                if (!ss_is_scoped_struct(d)) return 1;
                break;
            case AST_C_STRUCT_DEF: case AST_DISTINCT_TYPE_DEF: case AST_SUM_TYPE_DEF:
            case AST_ENUM_DEFINITION: case AST_BITSTRUCT_DEFINITION:
            case AST_CFN_TYPE_DEF: case AST_MESSAGE_DEFINITION:
                return 1;
            default:
                break;
        }
    }
    return 0;
}

/* The struct `name` of module `m`, followed through a re-export (#924). */
static ScopedStruct* ss_of_module(AetherModule* m, const char* name) {
    if (!m) return NULL;
    ScopedStruct* s = ss_find(m, name);
    if (s) return s;
    AetherModule* origin = module_resolve_reexport(m, name);
    return origin ? ss_find(origin, name) : NULL;
}

/* The module the qualifier `q` names in the file whose AST is `scope_ast`
 * (`self` its module, NULL for the program): the file's own module, or an
 * import, by alias, path or namespace. Mirrors the typechecker's
 * check_qualified_types, which reports what this cannot resolve. */
static AetherModule* ss_qualifier_module(AetherModule* self, ASTNode* scope_ast, const char* q) {
    if (self && self->name &&
        (strcmp(q, self->name) == 0 || strcmp(q, module_namespace_of(self->name)) == 0 ||
         strcmp(q, module_last_segment(self->name)) == 0))
        return self;
    for (int i = 0; scope_ast && i < scope_ast->child_count; i++) {
        ASTNode* imp = scope_ast->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        if (imp->annotation && strcmp(imp->annotation, "synthetic") == 0) continue;
        if (strcmp(q, import_written_prefix(imp)) == 0 || strcmp(q, imp->value) == 0 ||
            strcmp(q, module_namespace_of(imp->value)) == 0 ||
            strcmp(q, module_last_segment(imp->value)) == 0) {
            AetherModule* m = module_find(imp->value);
            return m ? m : module_find_by_namespace(q);
        }
    }
    return NULL;
}

typedef struct {
    AetherModule* self;    /* the module the code was written in; NULL: the program */
    ASTNode* scope_ast;    /* the AST whose imports are in scope there */
    const char* file;      /* for diagnostics */
} SsScope;

/* One ambiguity report per name and line, however many there are (#3545:
 * 64 were remembered, and a report past them could be repeated). Keyed
 * `<line>:<name>`. */
static StrMap g_ss_reported = {0};

static int g_ss_quiet = 0;   /* resolving to compare, not for a use: report nothing */

static void ss_report_ambiguous(const SsScope* sc, const char* name, ScopedStruct** cands,
                                int ncand, int line, int column, int via_imports) {
    if (g_ss_quiet) return;
    char* key = mod_strf("%d:%s", line, name);
    int seen = strmap_has(&g_ss_reported, key);
    if (!seen) strmap_put(&g_ss_reported, key, (void*)1);
    free(key);
    if (seen) return;
    char where[400];
    size_t off = 0;
    where[0] = '\0';
    for (int i = 0; i < ncand && off + 40 < sizeof(where); i++) {
        const char* mn = cands[i]->mod ? cands[i]->mod->name : "the program";
        off += (size_t)snprintf(where + off, sizeof(where) - off, "%s'%s'",
                                i == 0 ? "" : (i == ncand - 1 ? " and " : ", "), mn);
    }
    char msg[900], hint[600];
    snprintf(msg, sizeof(msg),
             "struct '%s' is ambiguous here: %s modules %s each define one",
             name, via_imports ? "the imported" : "the", where);
    off = 0;
    hint[0] = '\0';
    off += (size_t)snprintf(hint, sizeof(hint), "name the one you mean with its module: ");
    for (int i = 0; i < ncand && off + 40 < sizeof(hint); i++) {
        if (!cands[i]->mod) continue;
        off += (size_t)snprintf(hint + off, sizeof(hint) - off, "%s%s.%s",
                                i == 0 ? "" : " or ",
                                module_namespace_of(cands[i]->mod->name), name);
    }
    AetherError e = { sc->file, NULL, line, column, msg, hint, NULL,
                      AETHER_ERR_UNDEFINED_TYPE };
    aether_error_report(&e);
}

/* `s` added to the candidate list `*v` (a struct reached twice is one). */
static void ss_cand_add(ScopedStruct*** v, int* n, int* cap, ScopedStruct* s) {
    for (int k = 0; k < *n; k++)
        if ((*v)[k]->cname == s->cname) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = aether_xrealloc(*v, (size_t)*cap * sizeof(**v));
    }
    (*v)[(*n)++] = s;
}

/* The struct a name written in `sc` resolves to, or NULL when it is no
 * module-scoped struct (then the name is left as written). */
static ScopedStruct* ss_resolve(const SsScope* sc, const char* name, const char* qualifier,
                                int line, int column) {
    if (!name || g_ss_count == 0) return NULL;
    if (qualifier) {
        AetherModule* m = ss_qualifier_module(sc->self, sc->scope_ast, qualifier);
        return m ? ss_of_module(m, name) : NULL;
    }
    ScopedStruct* own = ss_find(sc->self, name);
    if (own) return own;
    if (ss_ast_has_other_type(sc->scope_ast, name)) return NULL;

    /* The modules the file imports. A selective import naming `name` picks
     * that module outright. */
    /* The candidates, as many as there are (#3545: 16 were kept, and the
     * ambiguity report named only those). */
    ScopedStruct** cands = NULL;
    int ncand = 0, cand_cap = 0;
    int other = 0;   /* an import defines an enum, sum, ... of that name */
    for (int i = 0; sc->scope_ast && i < sc->scope_ast->child_count; i++) {
        ASTNode* imp = sc->scope_ast->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        if (imp->annotation && strcmp(imp->annotation, "synthetic") == 0) continue;
        AetherModule* im = module_find(imp->value);
        ScopedStruct* s = ss_of_module(im, name);
        if (!s) {
            if (im && ss_ast_has_other_type(im->ast, name)) other = 1;
            continue;
        }
        for (int k = 0; k < imp->child_count; k++) {
            ASTNode* sel = imp->children[k];
            if (sel && sel->type == AST_IDENTIFIER && sel->value &&
                !(sel->annotation && strcmp(sel->annotation, "module_alias") == 0) &&
                strcmp(sel->value, name) == 0) {
                free(cands);
                return s;
            }
        }
        ss_cand_add(&cands, &ncand, &cand_cap, s);
    }
    ScopedStruct* found = NULL;
    if (ncand == 1) {
        found = cands[0];
    } else if (ncand > 1) {
        ss_report_ambiguous(sc, name, cands, ncand, line, column, 1);
    } else if (!other) {
        /* None imported: the one struct of that name in the build, as when
         * struct names were global (a module's body may name a struct of a
         * module it reaches only transitively). */
        for (int i = 0; i < g_ss_count; i++) {
            ScopedStruct* s = &g_ss[i];
            if (strcmp(s->name, name) == 0) ss_cand_add(&cands, &ncand, &cand_cap, s);
        }
        if (ncand == 1) found = cands[0];
        else if (ncand > 1) ss_report_ambiguous(sc, name, cands, ncand, line, column, 0);
    }
    free(cands);
    return found;
}

static void ss_rename(char** slot, const ScopedStruct* s) {
    if (!s || !*slot || strcmp(*slot, s->cname) == 0) return;
    free(*slot);
    *slot = strdup(s->cname);
}

static void ss_rewrite_type(const SsScope* sc, Type* t, const ASTNode* at) {
    if (!t) return;
    if (t->kind == TYPE_STRUCT && t->struct_name) {
        ScopedStruct* s = ss_resolve(sc, t->struct_name, t->qualifier,
                                     at ? at->line : 0, at ? at->column : 0);
        ss_rename(&t->struct_name, s);
    }
    ss_rewrite_type(sc, t->element_type, at);
    ss_rewrite_type(sc, t->return_type, at);
    for (int i = 0; t->tuple_types && i < t->tuple_count; i++)
        ss_rewrite_type(sc, t->tuple_types[i], at);
    for (int i = 0; t->param_types && i < t->param_count; i++)
        ss_rewrite_type(sc, t->param_types[i], at);
}

static void ss_rewrite_node(const SsScope* sc, ASTNode* n) {
    if (!n) return;
    ss_rewrite_type(sc, n->node_type, n);
    switch (n->type) {
        case AST_STRUCT_LITERAL:
        case AST_PTR_AS_STRUCT_CAST:
        case AST_SIZEOF:
        case AST_OFFSETOF:
        case AST_PATTERN_STRUCT:
        case AST_HEAP_NEW:
            if (n->value) {
                /* A qualified `gpu.T { ... }` / `as *gpu.T` carries the
                 * qualifier on its parsed type, already resolved above. */
                const char* q = NULL;
                if (n->node_type) {
                    Type* t = n->node_type;
                    while (t && t->kind != TYPE_STRUCT) t = t->element_type;
                    if (t && t->struct_name) {
                        q = t->qualifier;
                        if (q) {
                            if (strcmp(n->value, t->struct_name) != 0 &&
                                strcmp(ast_struct_source_name(t->struct_name), n->value) == 0) {
                                free(n->value);
                                n->value = strdup(t->struct_name);
                                /* Resolved. A literal's qualifier was never
                                 * held to the module's exports (the parser
                                 * dropped it), and is not now; one that
                                 * resolves to nothing stays for
                                 * check_qualified_types to report. */
                                if (n->type == AST_STRUCT_LITERAL) t->qualifier = NULL;
                            }
                            break;
                        }
                    }
                }
                ss_rename(&n->value, ss_resolve(sc, n->value, NULL, n->line, n->column));
            }
            break;
        case AST_SUM_TYPE_DEF:
            /* `type Shape = Circle | Rect`: each variant names a struct. */
            for (int i = 0; i < n->child_count; i++) {
                ASTNode* v = n->children[i];
                if (v && v->type == AST_IDENTIFIER && v->value)
                    ss_rename(&v->value, ss_resolve(sc, v->value, NULL, v->line, v->column));
            }
            return;
        default:
            break;
    }
    for (int i = 0; i < n->child_count; i++) ss_rewrite_node(sc, n->children[i]);
}

/* Resolve the struct names in `node`, code written in module `m` (NULL: the
 * program). */
static void ss_scope_node(ASTNode* node, AetherModule* m) {
    if (!node || g_ss_count == 0) return;
    SsScope sc = { m, m ? m->ast : g_ss_program,
                   node->source_file ? node->source_file : (m ? m->file_path : NULL) };
    ss_rewrite_node(&sc, node);
}

/* Is the field list of `a` (module `ma`) the same as `b`'s (module `mb`),
 * each struct-typed field resolving to one type in both? */
static int ss_fields_resolve_alike(const Type* ta, const Type* tb, AetherModule* ma,
                                   AetherModule* mb) {
    if (!ta || !tb) return ta == tb;
    if (ta->kind == TYPE_STRUCT && ta->struct_name && tb->struct_name) {
        SsScope sa = { ma, ma ? ma->ast : g_ss_program, NULL };
        SsScope sb = { mb, mb ? mb->ast : g_ss_program, NULL };
        g_ss_quiet = 1;   /* a real use of the name reports */
        ScopedStruct* ra = ss_resolve(&sa, ta->struct_name, ta->qualifier, 0, 0);
        ScopedStruct* rb = ss_resolve(&sb, tb->struct_name, tb->qualifier, 0, 0);
        g_ss_quiet = 0;
        if (strcmp(ra ? ra->cname : ta->struct_name, rb ? rb->cname : tb->struct_name) != 0)
            return 0;
    }
    if (!ss_fields_resolve_alike(ta->element_type, tb->element_type, ma, mb)) return 0;
    if (!ss_fields_resolve_alike(ta->return_type, tb->return_type, ma, mb)) return 0;
    return 1;
}

static int ss_same_definition(const ScopedStruct* a, const ScopedStruct* b) {
    if (!ast_defs_equal(a->decl, b->decl)) return 0;
    for (int i = 0; i < a->decl->child_count; i++) {
        ASTNode* fa = a->decl->children[i];
        ASTNode* fb = b->decl->children[i];
        if (fa && fb && !ss_fields_resolve_alike(fa->node_type, fb->node_type, a->mod, b->mod))
            return 0;
    }
    return 1;
}

/* Every struct of the program and of each loaded module, with the name it
 * is merged and emitted under. */
static void ss_build_table(ASTNode* program) {
    ss_reset();
    ast_struct_names_reset();
    strmap_free(&g_ss_reported);
    g_ss_program = program;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (ss_is_scoped_struct(d) && !d->is_imported && !ss_find(NULL, d->value))
            ss_add(NULL, d, d->value);
    }
    if (!global_module_registry) return;
    int first_module = g_ss_count;
    for (int mi = 0; mi < global_module_registry->module_count; mi++) {
        AetherModule* m = global_module_registry->modules[mi];
        if (!m || !m->ast || m->ast == program || !m->name) continue;
        const char* ns = module_namespace_of(m->name);
        for (int j = 0; j < m->ast->child_count; j++) {
            ASTNode* d = unwrap_export(m->ast->children[j]);
            if (!ss_is_scoped_struct(d) || ss_find(m, d->value)) continue;
            int keep_plain = g_ss_emit_lib &&
                             (m->export_count == 0 || module_exports_symbol(m, d->value));
            if (keep_plain) {
                ss_add(m, d, d->value);
            } else {
                char* cname = mod_strf("%s_%s", ns, d->value);
                ss_add(m, d, cname);
                free(cname);
            }
        }
    }
    /* Identical definitions are one type (#2129): a module's struct takes
     * the name of an earlier identical one, the program's own first. Runs
     * to a fixed point, since a struct's fields may name one that only
     * becomes shared in an earlier round. */
    for (int changed = 1, rounds = 0; changed && rounds < 8; rounds++) {
        changed = 0;
        for (int i = first_module; i < g_ss_count; i++) {
            ScopedStruct* s = &g_ss[i];
            for (int k = 0; k < i; k++) {
                ScopedStruct* p = &g_ss[k];
                if (p->mod == s->mod || strcmp(p->name, s->name) != 0) continue;
                if (p->cname == s->cname) break;
                if (ss_same_definition(p, s)) {
                    s->cname = p->cname;
                    s->shared = 1;
                    changed = 1;
                    break;
                }
            }
        }
    }
    for (int i = first_module; i < g_ss_count; i++) {
        ScopedStruct* s = &g_ss[i];
        if (s->shared) continue;
        ast_struct_name_register(s->cname, s->name,
                                 aether_internf("%s.%s", module_namespace_of(s->mod->name), s->name));
    }
}

/* The merged definition of module `m`'s struct `decl`: renamed to its
 * merged name, the struct names its fields write resolved in `m`. NULL when
 * the program already has it (the same struct reached twice, or one
 * identical to an earlier definition). */
static ASTNode* ss_clone_struct(ASTNode* program, ASTNode* decl, AetherModule* m) {
    ScopedStruct* s = ss_find(m, decl->value);
    const char* cname = s ? s->cname : decl->value;
    ASTNode* existing = program_find_def(program, AST_STRUCT_DEFINITION, cname);
    if (existing) {
        /* The same struct reached again, or one identical to an earlier
         * definition and so sharing its name. Two different plain names,
         * which only `--emit=lib`'s exported structs keep, are still the
         * clash #2129 reports. */
        int plain = !s || strcmp(cname, decl->value) == 0;
        if (plain && !(s && s->shared))
            definition_already_merged(program, decl, "struct");
        return NULL;
    }
    ASTNode* clone = clone_ast_node(decl);
    clone->is_imported = 1;
    if (strcmp(cname, decl->value) != 0) {
        free(clone->source_name);
        clone->source_name = strdup(decl->value);
        free(clone->value);
        clone->value = strdup(cname);
    }
    stamp_origin(clone, m);
    ss_scope_node(clone, m);
    return clone;
}

void module_merge_into_program(ASTNode* program) {
    if (!program || !global_module_registry) return;
    decl_index_drop(program);   /* #3546: indexed afresh for this merge */

    /* Module-scoped structs: name each struct of each module, then resolve
     * the struct names the program's own code writes, and those of the
     * modules' bare externs, which stay in the registry rather than being
     * cloned (their prototypes are emitted from there). */
    ss_build_table(program);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type == AST_IMPORT_STATEMENT || child->is_imported) continue;
        ss_scope_node(child, NULL);
    }
    for (int mi = 0; mi < global_module_registry->module_count; mi++) {
        AetherModule* m = global_module_registry->modules[mi];
        if (!m || !m->ast || m->ast == program) continue;
        for (int j = 0; j < m->ast->child_count; j++) {
            ASTNode* d = unwrap_export(m->ast->children[j]);
            if (d && d->type == AST_EXTERN_FUNCTION) ss_scope_node(d, m);
        }
    }

    /* #2209: the program's own code names its imports by alias or last
     * segment; point each at the imported module's namespace before any
     * module body is cloned in beside it. */
    ImportPrefixes prog_imports;
    import_prefixes_collect(&prog_imports, program, NULL);
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type == AST_IMPORT_STATEMENT) continue;
        import_prefixes_rewrite(&prog_imports, child);
    }
    import_prefixes_free(&prog_imports);

    // Find insertion point: just before AST_MAIN_FUNCTION
    int insert_idx = program->child_count;
    for (int i = 0; i < program->child_count; i++) {
        if (program->children[i]->type == AST_MAIN_FUNCTION) {
            insert_idx = i;
            break;
        }
    }

    // Save original child count — we scan the original program's imports, PLUS
    // any synthetic bare imports injected during the loop by
    // inject_synthetic_bare_imports_from (#870). Those re-open the qualified
    // surface for a merged dependency's `ns.fn()` calls, but their module
    // bodies still need cloning here — and depending on import order a synthetic
    // can land at an index >= the original count, past a frozen `orig_count`
    // bound. That left the dependency's wrappers un-cloned whenever the entry
    // also selectively imported the same module and that module wasn't the
    // dependency's first import (aether#1009: `os.now_monotonic_ns` Undefined).
    // We therefore extend the scan to the growing child_count but process only
    // *synthetic* import nodes beyond orig_count — originals all live below it,
    // and clone dedup (`program_has_function`) makes reprocessing a no-op. The
    // growth terminates: each module injects finitely and injection dedups.
    int orig_count = program->child_count;

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;
        // Beyond the original imports, only synthetic (#870) imports are in
        // scope for body-cloning; skip anything else that grew the array.
        if (i >= orig_count &&
            !(child->annotation && strcmp(child->annotation, "synthetic") == 0)) {
            continue;
        }

        const char* module_path = child->value;

        // Merge function and constant declarations from every module,
        // including stdlib modules. Stdlib used to be skipped under the
        // assumption that it only contained externs, but stdlib can now
        // carry Aether-native wrappers (e.g. Go-style result-type wrappers
        // over the raw externs), which need to be cloned into the program
        // AST just like user modules. module_collect_names only
        // takes AST_FUNCTION_DEFINITION nodes as functions, so externs are untouched
        // by this path.

        AetherModule* mod = module_find(module_path);
        if (!mod || !mod->ast) continue;

        ASTNode* mod_ast = mod->ast;
        const char* ns = module_get_namespace(module_path);

        /* #2613: a selective import (`import m (a, b)`) merges the whole
         * module like a bare one. The selection decides which names the
         * importing file may write bare (the second pass below and the
         * typechecker's alias table), not which of the module's
         * declarations exist: every function of every module in the build
         * is type-checked, the ones nothing calls included, and the prune
         * after type checking drops those from the C. Filtering here left a
         * module's unselected functions unchecked, a selected function that
         * read an unselected constant undefined (`Undefined variable
         * 'clib_K'`), and the qualified `m.other()` a selective import
         * keeps (#878) unresolvable (#2630). */

        // #870: re-open the qualified-call surface for any module this
        // imported module bare-imports, so its merged bodies' `ns.fn(...)`
        // calls resolve even when the entry file imported the same module
        // selectively.
        inject_synthetic_bare_imports_from(program, mod_ast, &insert_idx);

        MergePass pass = {0};
        for (int j = 0; j < mod_ast->child_count; j++) {
            ASTNode* decl = unwrap_export(mod_ast->children[j]);

            if ((decl->type == AST_FUNCTION_DEFINITION || decl->type == AST_BUILDER_FUNCTION) && decl->value) {
                // Build prefixed name: "double_it" -> "mymath_double_it",
                // apart from an extern of the module (#2978).
                char* prefixed = merged_function_name(ns, mod_ast, decl->value);

                // Skip if already merged (e.g. from a prior non-selective
                // import); every clause of a set is merged (#2643).
                if (program_has_function_before(program, prefixed, &pass)) {
                    free(prefixed);
                    continue;
                }

                ASTNode* clone = clone_ast_node(decl);
                free(clone->value);
                clone->value = prefixed;
                // Mark as imported so codegen emits this function with the
                // C `static` storage class. Each translation unit that
                // imports the same module gets its own private copy, and
                // the linker no longer sees them as duplicate symbols
                // when several .o files are linked together (e.g. macOS
                // ld64, which does not support --allow-multiple-definition).
                clone->is_imported = 1;
                stamp_origin(clone, mod);
                stamp_written_name(clone, ns, decl->value);   /* #2978 */

                // Rename intra-module function calls and constant refs within the cloned body
                rename_intra_module_refs(clone, ns, module_func_names(mod),
                                         module_const_names(mod), NULL);
                // Also rewrite bare-name calls to selectively-imported
                // names from M's own `import X (a, b)` statements — see
                // apply_inherited_selective_imports comment for why.
                apply_inherited_selective_imports(clone, mod_ast);
                ss_scope_node(clone, mod);

                insert_child_at(program, clone, insert_idx++);
                merge_pass_add(&pass, clone);
            } else if (decl->type == AST_EXTERN_FUNCTION && decl->value &&
                       decl->annotation &&
                       strncmp(decl->annotation, "c_symbol:", 9) == 0) {
                // `@extern("c_symbol") name(...)` declarations are part
                // of the module's public surface (issue #234) — unlike
                // bare `extern` C bindings, which stay private. Clone
                // them into the consumer just like Aether-native
                // wrappers so a qualified `module.name(...)` call
                // resolves. The clone keeps the `c_symbol:` annotation,
                // so codegen still forwards every call to the chosen C
                // symbol; only the Aether-side name is namespace-
                // prefixed. This is the path that makes a variadic
                // `@extern` (e.g. std.strbuilder's `append_format`)
                // reachable cross-module — an ordinary wrapper cannot
                // forward a `...` tail.
                char* prefixed = mod_strf("%s_%s", ns, decl->value);

                // Dedup — a module imported twice must not clone the
                // extern twice (duplicate registry entries / prototypes).
                // #3546: from the program's index.
                if (ast_decl_kinds(program, prefixed) & DECL_EXTERN) {
                    free(prefixed);
                    continue;
                }

                ASTNode* clone = clone_ast_node(decl);
                free(clone->value);
                clone->value = prefixed;
                /* Merged, not the program's own (#2632 reads the mark). */
                stamp_origin(clone, mod);
                ss_scope_node(clone, mod);
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_CONST_DECLARATION && decl->value) {
                /* Constants and module-level `var`s (#701) merge whatever the
                 * import selects: the module's functions read them (#1573:
                 * `import std.spec (fail)` failed with "Undefined variable
                 * 'spec_current_fw'"), and every one of its functions is now
                 * merged (#2613). */

                // Skip if already merged (e.g. from a prior import of the same
                // module, or a synthetic revisit — aether#1009). Without this
                // the const is cloned twice → duplicate C definition.
                char* prefixed = mod_strf("%s_%s", ns, decl->value);
                if (program_has_const(program, prefixed)) {
                    free(prefixed);
                    continue;
                }

                // Clone and rename constants too
                ASTNode* clone = clone_ast_node(decl);
                free(clone->value);
                clone->value = prefixed;
                stamp_origin(clone, mod);

                // Rename references to other module constants in the value expression
                rename_intra_module_refs(clone, ns, module_func_names(mod),
                                         module_const_names(mod), NULL);
                apply_inherited_selective_imports(clone, mod_ast);
                ss_scope_node(clone, mod);

                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_FAULT_DEFINITION) {
                /* #error-unification P3: a fault set imported from module `ns`.
                 * Mirror the const branch: clone the definition, prefix each
                 * member's C symbol to `<ns>_<name>` (so the member-access
                 * rewrite resolves `ns.Name` → that identifier, exactly like a
                 * const), and rewrite each member's interned string CONTENT to
                 * the qualified `"<ns>.<name>"` (so cross-module identity is
                 * unambiguous — two modules' same-named faults no longer
                 * compare equal by content). The parser/typecheck-registration/
                 * codegen stages already consume AST_FAULT_DEFINITION with this
                 * exact `<ns>_<name>` symbol / `<ns>.<name>` content contract;
                 * this branch is the piece that fulfils it. */
                ASTNode* clone = clone_ast_node(decl);
                int kept = 0;
                for (int mi = 0; mi < clone->child_count; mi++) {
                    ASTNode* mem = clone->children[mi];
                    if (!mem || !mem->value) {
                        free_ast_node(mem);
                        continue;
                    }
                    /* Every member, whatever a selective import names: the
                     * module's own functions may return any of them (#2613). */
                    char* sym = mod_strf("%s_%s", ns, mem->value);
                    if (program_has_fault_member(program, sym)) {
                        free(sym);
                        free_ast_node(mem);
                        continue;
                    }
                    char* qualified = mod_strf("%s.%s", ns, mem->value);
                    /* Rewrite the interned content (children[0], an
                     * AST_LITERAL) to the qualified name, then the symbol. */
                    if (mem->child_count > 0 && mem->children[0] &&
                        mem->children[0]->value) {
                        free(mem->children[0]->value);
                        mem->children[0]->value = strdup(qualified);
                    }
                    free(qualified);
                    free(mem->value);
                    mem->value = sym;
                    /* Compact surviving members down over filtered/duped ones. */
                    clone->children[kept++] = mem;
                }
                clone->child_count = kept;
                if (kept > 0) {
                    insert_child_at(program, clone, insert_idx++);
                } else {
                    free_ast_node(clone);
                }
            } else if (decl->type == AST_STRUCT_DEFINITION && decl->value) {
                // Struct definitions from imported modules enter the
                // consumer's program AST. Closes the cross-`import` struct
                // visibility gap that blocked the opaque-handle pattern: a
                // module exposing `new_slot() -> ptr` whose body does
                // `s = raw as *Slot` failed to typecheck once imported,
                // because the consumer's symbol table lacked `Slot`.
                //
                // Bypasses the selective-import filter on purpose: a
                // function body that casts to `*Slot` cannot type-check
                // without the struct in scope, so even
                // `import handle (new_slot)` must pull in `Slot`.
                //
                // Module-scoped: merged as `<ns>_<Name>` (ss_build_table),
                // so two modules' same-named structs are two types; the
                // same struct reached twice is merged once, as is one
                // identical to an earlier definition (#2129).
                ASTNode* clone = ss_clone_struct(program, decl, mod);
                if (!clone) continue;
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_DISTINCT_TYPE_DEF && decl->value) {
                // #908: `type X = distinct Base` from an imported module must
                // enter the consumer's program AST (bare name, like structs)
                // so resolve_distinct_types learns `X` and rewrites every
                // `expr as X` / `x as Base` reference inside the merged
                // constructor/unwrap/builder-child functions. Without this the
                // imported distinct stays an unresolved TYPE_STRUCT{X}
                // placeholder: the `as` cast fails its kind check ("cannot
                // cast X to Base") and codegen emits an unknown C type `X`.
                // Bypasses the selective-import filter on purpose (same as
                // structs): a merged body that casts to/from `X` cannot
                // type-check without the def in scope.
                if (program_has_distinct(program, decl->value)) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                ss_scope_node(clone, mod);
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_CFN_TYPE_DEF && decl->value) {
                // #2200: a `cfn Name(...) -> R` from an imported module enters
                // the consumer's program AST (bare name, like distincts) so
                // resolve_cfn_types learns `Name` and rewrites the `x: Name`
                // annotations and `p as Name` casts inside the merged bodies.
                // Bypasses the selective-import filter on purpose, same as
                // structs: a merged body cannot type-check without it.
                if (program_has_cfn(program, decl->value)) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                ss_scope_node(clone, mod);
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_BITSTRUCT_DEFINITION && decl->value) {
                // A `bitstruct Name : <backing> { ... }` from an imported module
                // must enter the consumer's program AST (bare name, like structs
                // and distinct types) so resolve_bitstruct_types registers it and
                // codegen emits its `typedef <backing> Name;`. Without this the
                // imported bitstruct is unknown to both passes: the `word as
                // Name` cast in the module's own (merged) function fails its kind
                // check, and codegen emits a prototype referencing `Name` with no
                // typedef ("unknown type name 'Name'"). Bypasses the selective-
                // import filter on purpose (same as structs/distincts): a merged
                // body that casts to/reads a field of the bitstruct cannot
                // type-check without the def in scope. Dedup by name.
                if (program_has_bitstruct(program, decl->value)) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_ENUM_DEFINITION && decl->value) {
                // An `enum X { ... }` from an imported module must enter the
                // consumer's program AST (bare name, like structs/bitstructs) so
                // the typechecker's enum registry learns `X` (resolving `X.Member`
                // selectors and `match` arms) and codegen emits its typedef.
                // Without this an imported enum used by name fails: `Undefined
                // variable 'X'` at type-check, or `unknown type name 'X'` in the
                // emitted C. Bypasses the selective-import filter (same as
                // structs): a merged body over the enum can't type-check without
                // the def in scope. Dedup by name.
                if (program_has_enum(program, decl->value)) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_SUM_TYPE_DEF && decl->value) {
                // A `type X = A | B | C` sum type from an imported module enters
                // the consumer's program AST (bare name) so its variant names and
                // typedef resolve on both the type-check and codegen sides. Same
                // failure/fix shape as enums/structs. Dedup by name.
                if (program_has_sum(program, decl->value)) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                ss_scope_node(clone, mod);
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_MESSAGE_DEFINITION && decl->value) {
                // #1006: `message X { ... }` from an imported module enters the
                // consumer's program AST under its bare name (like structs), so
                // `actor ! X { ... }` sends and `receive { X(..) -> }` patterns
                // resolve, and codegen assigns X a runtime message-type id from
                // the (per-program) message registry. Bypasses the selective-
                // import filter on purpose: an imported actor's handlers cannot
                // type-check without their message types in scope. Dedup by
                // name; a different definition under the same name is a
                // clash, reported (#2129).
                if (definition_already_merged(program, decl, "message")) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                ss_scope_node(clone, mod);
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_PROTOCOL_DEFINITION && decl->value) {
                // #385: a `protocol` enters under its bare name, as the
                // messages it names do, so an imported actor that implements
                // it and an `actor_ref[P]` written by the importer resolve.
                if (definition_already_merged(program, decl, "protocol")) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_ACTOR_DEFINITION && decl->value) {
                // #1006: `actor X { ... }` from an imported module enters the
                // consumer's program AST under its bare name. `spawn(X())`
                // lowers to a `spawn_X` call and message sends reference bare
                // message names, so the consumer needs the actor (and its
                // messages, handled above) in scope by bare name. Bypasses the
                // selective-import filter (same reasoning as structs/messages).
                // Dedup by name; a second actor of the same name from another
                // module is a clash, reported (#2129).
                if (definition_already_merged(program, decl, "actor")) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                stamp_origin(clone, mod);
                // Unlike structs, an actor has a body: its receive handlers may
                // call module-local functions or read module constants, which
                // were cloned above under their `<ns>_` prefixed names. Rewrite
                // those references so they resolve, exactly as function bodies do.
                rename_intra_module_refs(clone, ns, module_func_names(mod),
                                         module_const_names(mod), NULL);
                apply_inherited_selective_imports(clone, mod_ast);
                ss_scope_node(clone, mod);
                insert_child_at(program, clone, insert_idx++);
            }
            // Skip AST_MAIN_FUNCTION, AST_IMPORT_STATEMENT, etc.
        }
        merge_pass_free(&pass);
    }

    // Transitive cross-module merge (#243). When user code does
    // `import std.http.client` and that module's body internally does
    // `import std.json` and calls `json.stringify(...)`, the cloned
    // `client_post_json` body still contains `json.stringify(...)`
    // after the main loop above. The orchestrator already loaded
    // `std.json` into the registry (see orchestrate_module's recursive
    // step), but this merger only iterated the user's direct imports,
    // so json's exported functions never got cloned into the program
    // AST. The typechecker then can't resolve `json.stringify` and
    // forces the user to write `import std.json` themselves — a leak
    // of internal dependencies analogous to C/C++'s `#include`-style
    // dependency exposure rather than encapsulation.
    //
    // Fix: BFS from the user's directly imported modules through
    // each module's `imports` list, merging every transitively-
    // reachable module that hasn't been merged yet. Treat them as
    // non-selective (pull all exported names) since the user's direct
    // import implies they want the whole transitive closure to work.
    // No selection list applies; the typechecker still gates names
    // via is_export_blocked at qualified-call sites, so a user who
    // didn't write `import std.json` can't directly call
    // `json.stringify` from their own code — only the merged
    // `client_post_json` body can, which is exactly what we want.
    {
        // Collect direct imports as the BFS frontier. The list is the queue
        // too, read from `q_head` on, and as long as the modules reached
        // (#3545: it held 256, and a module past them was never merged).
        ModuleNames visited = {0};
        int q_head = 0;

        for (int i = 0; i < orig_count; i++) {
            ASTNode* child = program->children[i];
            if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
            if (!names_has(&visited, child->value)) names_add(&visited, child->value);
        }

        // BFS: for each enqueued module, look at its `imports` list and
        // enqueue any not yet visited.
        while (q_head < visited.count) {
            const char* mod_path = visited.names[q_head++];
            AetherModule* mod = module_find(mod_path);
            if (!mod) continue;

            for (int i = 0; i < mod->import_count; i++) {
                const char* dep = mod->imports[i];
                if (!dep || names_has(&visited, dep)) continue;
                names_add(&visited, dep);
            }
        }

        // For each transitively-reachable module, merge its exported functions
        // and constants the same way the main loop merged direct imports.
        for (int v = 0; v < visited.count; v++) {
            const char* dep_path = visited.names[v];

            // Skip the program's direct imports: the main loop merged every
            // declaration of each, selective or not (#2613). Before that a
            // selective direct import that another module also needed had to
            // fall through here for the rest of its exports (#1097).
            int is_direct = 0;
            for (int i = 0; i < orig_count; i++) {
                ASTNode* child = program->children[i];
                if (child && child->type == AST_IMPORT_STATEMENT &&
                    child->value && strcmp(child->value, dep_path) == 0) {
                    is_direct = 1;
                    break;
                }
            }
            if (is_direct) continue;

            AetherModule* dep_mod = module_find(dep_path);
            if (!dep_mod || !dep_mod->ast) continue;

            ASTNode* mod_ast = dep_mod->ast;
            const char* ns = module_get_namespace(dep_path);

            // Add a synthetic AST_IMPORT_STATEMENT for this transitive
            // dep so the typechecker's namespace-registration pass
            // picks it up. Without this, qualified calls to the dep's
            // namespace from inside merged module bodies (e.g.
            // `json.stringify(...)` inside a merged
            // `client_post_json`) get rejected by the typechecker
            // even though the prefixed `json_stringify` symbol is
            // present in the program AST. Insert at insert_idx so it
            // lives between the existing imports and the main
            // function — same region as the merged decls below it.
            //
            // Issue #243 sealed-scope follow-up: tag the synthetic
            // import with annotation="synthetic" so the typechecker
            // can register the namespace globally (for cloned merged
            // bodies that need to resolve their own internal
            // qualified calls) BUT skip the user-explicit registry
            // (so user code can't accidentally call into the
            // transitively-pulled-in namespace it never imported).
            // A dep the program already carries an import for (a #870
            // synthetic one) has its namespace registered; don't add a
            // redundant synthetic node for it. Only inject the synthetic
            // import when the program has no import of this path yet (the
            // pure-transitive case #243 targeted).
            int has_import_of_dep = 0;
            for (int p = 0; p < program->child_count; p++) {
                ASTNode* pc = program->children[p];
                if (pc && pc->type == AST_IMPORT_STATEMENT && pc->value &&
                    strcmp(pc->value, dep_path) == 0) {
                    has_import_of_dep = 1;
                    break;
                }
            }
            if (!has_import_of_dep) {
                ASTNode* synth_import = create_ast_node(AST_IMPORT_STATEMENT,
                                                       dep_path, 0, 0);
                synth_import->annotation = strdup("synthetic");
                insert_child_at(program, synth_import, insert_idx++);
            }

            MergePass pass = {0};
            for (int j = 0; j < mod_ast->child_count; j++) {
                ASTNode* decl = unwrap_export(mod_ast->children[j]);
                if (!decl || !decl->value) continue;

                if (decl->type == AST_FUNCTION_DEFINITION ||
                    decl->type == AST_BUILDER_FUNCTION) {
                    char* prefixed = merged_function_name(ns, mod_ast, decl->value);
                    /* Every clause of a set (#2643). */
                    if (program_has_function_before(program, prefixed, &pass)) {
                        free(prefixed);
                        continue;
                    }

                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = prefixed;
                    clone->is_imported = 1;
                    stamp_origin(clone, dep_mod);
                    stamp_written_name(clone, ns, decl->value);   /* #2978 */

                    rename_intra_module_refs(clone, ns, module_func_names(dep_mod),
                                             module_const_names(dep_mod), NULL);
                    apply_inherited_selective_imports(clone, mod_ast);
                    ss_scope_node(clone, dep_mod);

                    insert_child_at(program, clone, insert_idx++);
                    merge_pass_add(&pass, clone);
                } else if (decl->type == AST_CONST_DECLARATION) {
                    char* prefixed = mod_strf("%s_%s", ns, decl->value);

                    // Dedup against existing CONSTS (not functions — the old
                    // program_has_function guard here never matched a const, so
                    // a revisited module re-cloned it: aether#1009 exposed this
                    // as `redefinition of 'sha2_K256'`).
                    if (program_has_const(program, prefixed)) {
                        free(prefixed);
                        continue;
                    }

                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = prefixed;
                    stamp_origin(clone, dep_mod);

                    rename_intra_module_refs(clone, ns, module_func_names(dep_mod),
                                             module_const_names(dep_mod), NULL);
                    apply_inherited_selective_imports(clone, mod_ast);
                    ss_scope_node(clone, dep_mod);

                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_STRUCT_DEFINITION) {
                    // Same shape as the direct-import struct merge above,
                    // for transitively-reachable modules pulled in via the
                    // BFS. A merged function body that casts to `*T` needs
                    // T in scope regardless of which import edge brought
                    // the function in.
                    ASTNode* clone = ss_clone_struct(program, decl, dep_mod);
                    if (!clone) continue;
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_DISTINCT_TYPE_DEF && decl->value) {
                    // #908: transitive sibling of the distinct-def merge above.
                    if (program_has_distinct(program, decl->value)) continue;
                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
                    ss_scope_node(clone, dep_mod);
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_CFN_TYPE_DEF && decl->value) {
                    // #2200: transitive sibling of the cfn-def merge above.
                    if (program_has_cfn(program, decl->value)) continue;
                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
                    ss_scope_node(clone, dep_mod);
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_BITSTRUCT_DEFINITION && decl->value) {
                    // Transitive sibling of the bitstruct merge above: a
                    // bitstruct reached through a chain of imports must still
                    // land in program->children so its typedef is emitted.
                    if (program_has_bitstruct(program, decl->value)) continue;
                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_ENUM_DEFINITION && decl->value) {
                    // Transitive sibling of the enum merge above.
                    if (program_has_enum(program, decl->value)) continue;
                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_SUM_TYPE_DEF && decl->value) {
                    // Transitive sibling of the sum-type merge above.
                    if (program_has_sum(program, decl->value)) continue;
                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
                    ss_scope_node(clone, dep_mod);
                    insert_child_at(program, clone, insert_idx++);
                }
            }
            merge_pass_free(&pass);
        }
        names_free(&visited);
    }

    // Second pass: for each selective import (either the parenthesised
    // `import mod (a, b, c)` form or the trailing-component `import mod.a`
    // form, which resolve_import_path rewrites into the same shape),
    // rewrite bare-name references in user code to their module-prefixed
    // form so callers can write `button("1")` instead of
    // `aether_ui.button("1")` after `import contrib.aether_ui.button` or
    // `import contrib.aether_ui (button, hstack, ...)`.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* import_node = program->children[i];
        if (!import_node || import_node->type != AST_IMPORT_STATEMENT || !import_node->value) continue;
        int is_glob = (import_node->annotation &&
                       strcmp(import_node->annotation, "glob_import") == 0);
        if (!is_glob && !import_has_selection(import_node)) continue;

        const char* module_path = import_node->value;
        AetherModule* mod = module_find(module_path);
        if (!mod || !mod->ast) continue;
        const char* ns = module_get_namespace(module_path);

        ModuleNames sel_funcs = {0};
        ModuleNames sel_consts = {0};

        const char** mod_const_names = module_const_names(mod)->names;
        int mod_const_count = module_const_names(mod)->count;

        /* Per-symbol aliases: the name written in the program (`from`),
         * and the module's name it stands for (`to`). */
        ModuleNames alias_from = {0};
        ModuleNames alias_to = {0};

        /* A glob import's constants, written bare. The typechecker binds
         * each `<ns>_<NAME>` a glob brings in to its bare `NAME`, so a bare
         * `KIND_NO_SPACE` after `import std.fs (*)` checked, but nothing
         * renamed it, and the C named an undeclared `KIND_NO_SPACE`. Its
         * functions need no rename here: the checker rewrites a call
         * through its import alias. Same rules as the glob's binding: a
         * leading underscore is private, and a name the program defines
         * itself is the program's (#2632). */
        if (is_glob) {
            for (int k = 0; k < mod_const_count; k++) {
                const char* cn = mod_const_names[k];
                if (!cn || cn[0] == '_' || module_defines_own_name(program, cn)) continue;
                names_add(&sel_consts, cn);
            }
        }

        for (int k = 0; !is_glob && k < import_node->child_count; k++) {
            ASTNode* sel = import_node->children[k];
            if (!sel || sel->type != AST_IDENTIFIER || !sel->value) continue;
            // Skip alias nodes — the parser marks them with annotation="module_alias".
            if (sel->annotation && strcmp(sel->annotation, "module_alias") == 0) continue;
            /* Per-symbol alias (`import M (path as vgpath)`): the local
             * name in the program body is the ALIAS, and only the alias.
             * The exported name must NOT be added to the plain rename
             * list — leaving it there would capture a same-named local
             * definition, which is exactly the collision the alias
             * exists to avoid. */
            if (sel->annotation &&
                strncmp(sel->annotation, "select_alias:", 13) == 0) {
                if (names_has(module_func_names(mod), sel->value) ||
                    names_has(module_const_names(mod), sel->value)) {
                    names_add(&alias_from, sel->annotation + 13);
                    names_add(&alias_to, sel->value);
                }
                continue;
            }
            if (names_has(module_func_names(mod), sel->value)) {
                names_add(&sel_funcs, sel->value);
            } else if (names_has(module_const_names(mod), sel->value)) {
                names_add(&sel_consts, sel->value);
            }
        }

        for (int m = 0; m < program->child_count &&
                        (sel_funcs.count > 0 || sel_consts.count > 0 || alias_to.count > 0);
             m++) {
            ASTNode* top = program->children[m];
            if (!top) continue;
            if (top->type == AST_IMPORT_STATEMENT) continue;
            if (top->is_imported) continue;
            rename_intra_module_refs(top, ns, &sel_funcs, &sel_consts, NULL);
            for (int a = 0; a < alias_to.count; a++) {
                int is_const = names_has(module_const_names(mod), alias_to.names[a]);
                char* prefixed = is_const ? mod_strf("%s_%s", ns, alias_to.names[a])
                                          : merged_function_name_ns(ns, alias_to.names[a]);
                rename_aliased_import_refs(top, alias_from.names[a], prefixed, is_const);
                free(prefixed);
            }
        }
        names_free(&sel_funcs);
        names_free(&sel_consts);
        names_free(&alias_from);
        names_free(&alias_to);
    }

    /* #924 re-export pull-in. A directly-imported module may list, in its
     * `exports`, a symbol it imported from another module. Such a symbol is
     * resolved (in the typechecker) to the DEFINING module's prefixed name
     * (`hub.X` -> `<origin>_X`), so that definition must be present in the
     * merged program. Driven off the export list (re-export is opt-in via
     * `exports`), not the import-graph BFS above — that BFS depends on a
     * module's import_count being populated at merge time, which a bodyless
     * re-export hub may not satisfy. For each re-exported name we clone the
     * origin's func/const under the origin namespace, exactly as the
     * transitive BFS would. */
    for (int i = 0; i < orig_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
        AetherModule* hub = module_find(child->value);
        if (!hub) continue;

        for (int e = 0; e < hub->export_count; e++) {
            const char* sym = hub->exports[e];
            if (!sym) continue;
            AetherModule* origin = module_resolve_reexport(hub, sym);
            if (!origin || !origin->ast || !origin->name) continue;

            const char* ons = module_get_namespace(origin->name);
            char* prefixed = mod_strf("%s_%s", ons, sym);
            /* A function's, apart from an extern (#2978). */
            char* fn_name = merged_function_name(ons, origin->ast, sym);
            if (program_has_function(program, fn_name)) {
                free(prefixed);
                free(fn_name);
                continue;
            }

            for (int j = 0; j < origin->ast->child_count; j++) {
                ASTNode* decl = unwrap_export(origin->ast->children[j]);
                if (!decl || !decl->value || strcmp(decl->value, sym) != 0) continue;

                if (decl->type == AST_FUNCTION_DEFINITION ||
                    decl->type == AST_BUILDER_FUNCTION) {
                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(fn_name);
                    clone->is_imported = 1;
                    stamp_origin(clone, origin);
                    stamp_written_name(clone, ons, sym);   /* #2978 */
                    rename_intra_module_refs(clone, ons, module_func_names(origin),
                                             module_const_names(origin), NULL);
                    apply_inherited_selective_imports(clone, origin->ast);
                    ss_scope_node(clone, origin);
                    insert_child_at(program, clone, insert_idx++);
                    /* Every clause of a set written as several (#2643). */
                    continue;
                } else if (decl->type == AST_CONST_DECLARATION) {
                    // The block-level guard above checks functions only; a
                    // const needs its own dedup or a revisit re-clones it.
                    if (program_has_const(program, prefixed)) break;
                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(prefixed);
                    stamp_origin(clone, origin);
                    rename_intra_module_refs(clone, ons, module_func_names(origin),
                                             module_const_names(origin), NULL);
                    apply_inherited_selective_imports(clone, origin->ast);
                    ss_scope_node(clone, origin);
                    insert_child_at(program, clone, insert_idx++);
                }
                break;
            }
            free(prefixed);
            free(fn_name);
        }
    }
    /* #3546: the program's index goes with the merge: what comes after it
     * rewrites the top level in place (the prune, the renames off C names). */
    decl_index_drop(program);
}

// ----------------------------------------------------------------------------
// Reachability-based pruning of merged AST
//
// `module_merge_into_program` clones every exported function from every
// imported module (plus its transitive helpers) into the program AST.
// Many of those functions never get called from the user's program —
// most stdlib modules expose far more surface than any one program uses.
//
// Without filtering, the C compiler then has to compile (and the linker
// has to discard) thousands of dead functions per build. Removing them
// at the AST level skips that wasted gcc work entirely.
//
// They are still type-checked (#2613). The prune used to run before the
// typechecker, so a function of an imported module that nothing called
// was never checked at all: a library's type error built clean in every
// consumer until one of them first called it, while the same function
// uncalled in the main file failed the build. So the mark runs before
// type checking (module_mark_unreachable: what the program reaches is
// decided on the merged source, as before) and the sweep after it
// (module_sweep_unreachable): every function of every module in the
// build is checked, and only the reachable ones reach the C.
//
// Algorithm: classic mark-and-sweep over the call graph.
//   1. Seed the reachable set from main + actor handlers + exports +
//      every non-imported (user-written) function and builder + the
//      initializer of every module-level const and var (#2650).
//   2. Walk each seed, collecting AST_FUNCTION_CALL targets and bare
//      AST_IDENTIFIER references that name a top-level function.
//   3. For each newly-discovered name, find its definition in the
//      program AST and walk it the same way. Repeat until fixed point.
//   4. Record any AST_FUNCTION_DEFINITION / AST_BUILDER_FUNCTION whose
//      `is_imported` flag is set and whose name is NOT in the reachable
//      set; the sweep drops exactly those once they are checked.
//   5. The imported constants nothing names are dropped by the sweep too
//      (module_sweep_unreferenced_consts): not for their size, but because
//      each is an unused `static const` in the C, which -Wall -Werror
//      rejects.
//
// Qualified call sites carry dotted names (`os.argv0`); codegen rewrites
// dot-to-underscore at emission. We normalise the same way when adding
// to the set so it matches the prefixed function-definition names that
// `module_merge_into_program` produced (`os_argv0`).
// ----------------------------------------------------------------------------

/* #2007: the reachable set is a hash set, and the worklist a plain stack.
 *
 * The set holds every reachable function name and is probed once per call
 * site in the program; a linear scan made the prune quadratic in the
 * number of functions. Names are normalised on the way in -- a dotted
 * call `os.argv0` files as the merged definition's `os_argv0` -- so both
 * structures take the same key. */
typedef StrMap NameSet;

/* `name` with its dots made underscores, malloc'd when it has any (as long
 * as it is, #3545: cut to 255 bytes, a long name filed under another and
 * its function was pruned); `name` itself, in `*owned` NULL, otherwise. */
static const char* prune_normalise(const char* name, char** owned) {
    *owned = NULL;
    if (!strchr(name, '.')) return name;
    char* s = mod_strf("%s", name);
    for (char* p = s; *p; p++)
        if (*p == '.') *p = '_';
    *owned = s;
    return s;
}

static int nameset_contains(const NameSet* s, const char* name) {
    return name && strmap_has(s, name);
}

/* Adds the normalised name; returns 1 when it was new. */
static int nameset_add(NameSet* s, const char* name) {
    if (!name) return 0;
    char* owned;
    const char* key = prune_normalise(name, &owned);
    int fresh = !strmap_has(s, key);
    if (fresh) strmap_put(s, key, NULL);
    free(owned);
    return fresh;
}

static void nameset_free(NameSet* s) {
    strmap_free(s);
}

/* Every push is gated by a successful nameset_add on the reachable set,
 * so the stack never sees a duplicate and needs no membership test: it
 * is the names still to be walked, popped from the top. */
typedef struct {
    char** names;
    int count;
    int capacity;
} NameStack;

static void namestack_push(NameStack* s, const char* name) {
    if (!name) return;
    char* owned;
    const char* key = prune_normalise(name, &owned);
    if (s->count >= s->capacity) {
        int new_cap = s->capacity ? s->capacity * 2 : 64;
        char** nn = realloc(s->names, sizeof(char*) * new_cap);
        if (!nn) {
            free(owned);
            return;
        }
        s->names = nn;
        s->capacity = new_cap;
    }
    s->names[s->count++] = owned ? owned : strdup(key);
}

/* #2007: what the worklist drain asks of the program, indexed once.
 *
 * For each name popped it needs (a) the definitions with that name (every
 * clause of a function written as several, #2643: the calls of a later
 * clause reach what it calls), and (b) every imported definition whose
 * prefixed form ends in `_<name>` -- the glob-import / selective-import
 * case where user code calls the merged `mathlist_cube` as `cube`. Both
 * were linear scans of the top level per popped name, which made the drain
 * quadratic. The index maps a key to the definitions spelt exactly so, and
 * to the list of imported definitions for which the key is a `_`-delimited
 * suffix; an imported `a_b_c` is filed under `b_c` and `c`. */
typedef struct {
    ASTNode** defs;
    int def_count;
    int def_cap;
    ASTNode** suffix_defs;
    int suffix_count;
    int suffix_cap;
} PruneEntry;

static PruneEntry* prune_index_entry(StrMap* ix, const char* key) {
    PruneEntry* e = strmap_get(ix, key);
    if (e) return e;
    e = calloc(1, sizeof(PruneEntry));
    if (!e) return NULL;
    strmap_put(ix, key, e);
    return e;
}

static void prune_index_build(StrMap* ix, ASTNode* program) {
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || !c->value) continue;
        if (c->type != AST_FUNCTION_DEFINITION && c->type != AST_BUILDER_FUNCTION) continue;
        PruneEntry* e = prune_index_entry(ix, c->value);
        if (e && e->def_count >= e->def_cap) {
            int new_cap = e->def_cap ? e->def_cap * 2 : 2;
            ASTNode** nd = realloc(e->defs, sizeof(ASTNode*) * (size_t)new_cap);
            if (nd) {
                e->defs = nd;
                e->def_cap = new_cap;
            }
        }
        if (e && e->def_count < e->def_cap) e->defs[e->def_count++] = c;
        if (!c->is_imported) continue;
        for (const char* p = strchr(c->value, '_'); p; p = strchr(p + 1, '_')) {
            if (!p[1]) break;
            PruneEntry* se = prune_index_entry(ix, p + 1);
            if (!se) continue;
            if (se->suffix_count >= se->suffix_cap) {
                int new_cap = se->suffix_cap ? se->suffix_cap * 2 : 4;
                ASTNode** nd = realloc(se->suffix_defs, sizeof(ASTNode*) * (size_t)new_cap);
                if (!nd) continue;
                se->suffix_defs = nd;
                se->suffix_cap = new_cap;
            }
            se->suffix_defs[se->suffix_count++] = c;
        }
    }
}

static void prune_index_free(StrMap* ix) {
    for (int k = 0; k < strmap_count(ix); k++) {
        PruneEntry* e = strmap_value_at(ix, k);
        if (e) { free(e->defs); free(e->suffix_defs); free(e); }
    }
    strmap_free(ix);
}

static void prune_seed_ufcs_method(const char* method, NameSet* seen,
                                   NameStack* worklist) {
    if (!method || !global_module_registry) return;
    for (int mi = 0; mi < global_module_registry->module_count; mi++) {
        AetherModule* m = global_module_registry->modules[mi];
        if (!m || !m->name) continue;
        if (!module_is_exported(m, method)) continue;
        char* q = mod_strf("%s.%s", m->name, method);
        if (nameset_add(seen, q)) namestack_push(worklist, q);
        free(q);
    }
}

/* #2218: the names a function binds for itself -- parameters, locals,
 * loop and tuple variables, closure parameters, match bindings and catch
 * names. A bare identifier spelt like one of these is that binding, not a
 * function reference, so the reachability walk must not seed it: seeding
 * `record` pulled in every imported `<mod>_record` through the suffix
 * index below, and an unrelated local kept aether-ui's `ui_record` alive.
 * The set covers the whole function body, nested closures included; an
 * over-wide set can only drop a seed for a name the body has already
 * rebound, which no call in that body can mean as a function. */
static int prune_binds_name(const ASTNode* node) {
    switch (node->type) {
        case AST_VARIABLE_DECLARATION:
        case AST_PATTERN_VARIABLE:
        case AST_CLOSURE_PARAM:
        case AST_CATCH_CLAUSE:
            return node->value != NULL;
        default:
            return 0;
    }
}

static void prune_collect_locals(const ASTNode* node, NameSet* locals) {
    if (!node) return;
    if (prune_binds_name(node) && !strchr(node->value, '.')) {
        nameset_add(locals, node->value);
    }
    for (int i = 0; i < node->child_count; i++) {
        prune_collect_locals(node->children[i], locals);
    }
}

static int prune_is_function_scope(const ASTNode* node) {
    return node->type == AST_FUNCTION_DEFINITION ||
           node->type == AST_BUILDER_FUNCTION ||
           node->type == AST_MAIN_FUNCTION;
}

/* A bare (undotted) name the enclosing function binds as a local. */
static int prune_is_local(const NameSet* locals, const char* name) {
    return locals && !strchr(name, '.') && nameset_contains(locals, name);
}

static void prune_collect_calls_in(ASTNode* node, NameSet* seen, NameStack* worklist,
                                   const NameSet* locals, int in_dsl_block);

static void prune_collect_calls(ASTNode* node, NameSet* seen, NameStack* worklist) {
    prune_collect_calls_in(node, seen, worklist, NULL, 0);
}

/* `in_dsl_block`: `node` is in the trailing block of a member-access call,
 * where a bare call is the receiver's function (`<receiver>_<name>`) ahead
 * of the enclosing function's names (#333): such a call is a reference to
 * every function it may name, a local's name included. */
static void prune_collect_calls_in(ASTNode* node, NameSet* seen, NameStack* worklist,
                                   const NameSet* locals, int in_dsl_block) {
    if (!node) return;
    /* #3142: an owned type's drop function is called by the code that
     * releases its values, which the tree does not spell. */
    if (node->type == AST_DISTINCT_TYPE_DEF && node->annotation &&
        strncmp(node->annotation, "owned_drop:", 11) == 0) {
        const char* f = node->annotation + 11;
        if (*f && nameset_add(seen, f)) namestack_push(worklist, f);
        return;
    }
    if (prune_is_function_scope(node)) {
        NameSet own = {0};
        prune_collect_locals(node, &own);
        for (int i = 0; i < node->child_count; i++) {
            prune_collect_calls_in(node->children[i], seen, worklist, &own, in_dsl_block);
        }
        if (node->type == AST_BUILDER_FUNCTION && node->annotation) {
            if (nameset_add(seen, node->annotation)) {
                namestack_push(worklist, node->annotation);
            }
        }
        nameset_free(&own);
        return;
    }
    if (node->type == AST_FUNCTION_CALL && node->value &&
        (in_dsl_block || !prune_is_local(locals, node->value))) {
        if (nameset_add(seen, node->value)) {
            namestack_push(worklist, node->value);
        }
        /* #924 re-export: a `hub.fn(...)` call is cloned under the DEFINING
         * module's name (`<origin>_fn`). Seed the origin-qualified form so
         * the reachability sweep doesn't prune the re-exported body. */
        char* dot = strchr(node->value, '.');
        if (dot && dot != node->value && !strchr(dot + 1, '.')) {
            {
                char* hubname = mod_strf("%.*s", (int)(dot - node->value), node->value);
                AetherModule* hub = module_find(hubname);
                free(hubname);
                AetherModule* origin = module_resolve_reexport(hub, dot + 1);
                if (origin && origin->name) {
                    char* oq = mod_strf("%s.%s", origin->name, dot + 1);
                    if (nameset_add(seen, oq)) namestack_push(worklist, oq);
                    free(oq);
                }
                /* #934 shape (b): `value.method(...)` where the receiver is
                 * not a module — keep every imported `mod.method` candidate
                 * alive for the post-prune UFCS rewrite. (When `hubname` IS a
                 * module this is harmless; the real qualified target is
                 * already seeded by node->value above.) */
                prune_seed_ufcs_method(dot + 1, seen, worklist);
            }
        }
        /* #934 shape (a): parser-tagged UFCS node — bare method in value,
         * receiver subtree at children[0]. Seed imported candidates too. */
        if (node->annotation && strcmp(node->annotation, "ufcs") == 0 &&
            !strchr(node->value, '.')) {
            prune_seed_ufcs_method(node->value, seen, worklist);
        }
    }
    // Bare identifiers can name a function passed as a callback, taken
    // by address, or stored in a variable for later dispatch. Adding
    // every identifier name is a sound over-approximation: non-function
    // names won't match any function definition and harmlessly dead-end.
    if (node->type == AST_IDENTIFIER && node->value &&
        !prune_is_local(locals, node->value)) {
        if (nameset_add(seen, node->value)) {
            namestack_push(worklist, node->value);
        }
    }
    // Member access used as a value (no following call paren), e.g.
    // `cb = string.split`. The parser stores the namespace as
    // children[0] (an AST_IDENTIFIER) and the field name in `value`,
    // so reconstruct the dotted form so nameset_add normalises it
    // to the same C symbol as a direct `string.split(...)` call site.
    if (node->type == AST_MEMBER_ACCESS && node->value &&
        node->child_count > 0 && node->children[0] &&
        node->children[0]->type == AST_IDENTIFIER && node->children[0]->value) {
        char* qualified = mod_strf("%s.%s", node->children[0]->value, node->value);
        if (nameset_add(seen, qualified)) {
            namestack_push(worklist, qualified);
        }
        free(qualified);
        /* #924 re-export: `hub.X` is cloned under the DEFINING module's
         * name (`<origin>_X`), not `hub_X`. Seed the origin form too so the
         * reachability sweep keeps the re-exported definition. */
        AetherModule* hub = module_find(node->children[0]->value);
        AetherModule* origin = module_resolve_reexport(hub, node->value);
        if (origin && origin->name) {
            char* oq = mod_strf("%s.%s", origin->name, node->value);
            if (nameset_add(seen, oq)) namestack_push(worklist, oq);
            free(oq);
        }
    }
    /* `builder name(...) with <factory>` carries the factory name in
     * the AST_BUILDER_FUNCTION's `annotation` field. Codegen lowers
     * the call site as `_bcfg = <annotation>()`, so the factory is
     * a real (call-site) dependency of the builder even though it
     * doesn't appear in the body. Without seeding it here, the
     * mark-and-sweep dead-code prune drops the cross-module-cloned
     * factory body and the consumer TU emits an unresolved bare
     * call. Filed in aether/new_aevg_asks.md ASK 1. */
    if (node->type == AST_BUILDER_FUNCTION && node->annotation) {
        if (nameset_add(seen, node->annotation)) {
            namestack_push(worklist, node->annotation);
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        ASTNode* c = node->children[i];
        int dsl = in_dsl_block ||
                  (node->type == AST_FUNCTION_CALL && node->value &&
                   strchr(node->value, '.') && ast_is_trailing_block(c));
        prune_collect_calls_in(c, seen, worklist, locals, dsl);
    }
}


/* ---- #2297: a package built as one --emit=lib library ----
 *
 * `aetherc --lib-package <pkg>` (driven by `ae build --emit=lib --package`)
 * names a module prefix whose modules ARE the library: every module named
 * `<pkg>` or `<pkg>.<...>` is part of it, so its exported functions and
 * constants get stable `aether_<module>__<name>` wrappers and catalog
 * records instead of being private merged clones. */
static char* g_lib_package = NULL;

void module_set_lib_package(const char* pkg) {
    free(g_lib_package);
    g_lib_package = (pkg && *pkg) ? strdup(pkg) : NULL;
}

const char* module_lib_package(void) {
    return g_lib_package;
}

int module_in_lib_package(const AetherModule* m) {
    if (!g_lib_package || !m || !m->name) return 0;
    size_t n = strlen(g_lib_package);
    return strncmp(m->name, g_lib_package, n) == 0 &&
           (m->name[n] == '\0' || m->name[n] == '.');
}

/* The package module a merged declaration came from, or NULL. Identified by
 * the declaration's originating file: clones keep `source_file`. */
AetherModule* module_lib_package_module_of(const ASTNode* decl) {
    if (!g_lib_package || !decl || !decl->source_file) return NULL;
    /* Consecutive declarations nearly always share a file, and the lookup
     * compares files by identity (a stat per module): remember the last. */
    static char last_file[2048] = "";
    static AetherModule* last_module = NULL;
    static ModuleRegistry* last_registry = NULL;
    if (last_registry == global_module_registry && last_file[0] &&
        strcmp(last_file, decl->source_file) == 0) {
        return module_in_lib_package(last_module) ? last_module : NULL;
    }
    AetherModule* m = module_find_by_file(decl->source_file);
    snprintf(last_file, sizeof(last_file), "%s", decl->source_file);
    last_module = m;
    last_registry = global_module_registry;
    return module_in_lib_package(m) ? m : NULL;
}

/* #2613: the imported definitions the closure did not reach, recorded by
 * module_mark_unreachable before type checking and dropped by
 * module_sweep_unreachable after it. Held by pointer: the passes between
 * the two (derive, `when`, the typechecker) rewrite definitions in place
 * and never replace a merged one. */
static ASTNode** g_unreachable = NULL;
static int g_unreachable_count = 0;

static void module_unreachable_reset(void) {
    free(g_unreachable);
    g_unreachable = NULL;
    g_unreachable_count = 0;
}

static int unreachable_ptr_cmp(const void* a, const void* b) {
    uintptr_t x = (uintptr_t)*(ASTNode* const*)a;
    uintptr_t y = (uintptr_t)*(ASTNode* const*)b;
    return (x > y) - (x < y);
}

void module_mark_unreachable(ASTNode* program) {
    module_unreachable_reset();
    if (!program) return;

    NameSet reachable = {0};
    NameStack worklist = {0};
    StrMap index = {0};
    prune_index_build(&index, program);

    // Seed: main, non-imported user functions, actor decls, exports.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c) continue;
        switch (c->type) {
            case AST_MAIN_FUNCTION:
            case AST_ACTOR_DEFINITION:
            case AST_EXPORT_STATEMENT:
            /* #2650: a module-level `var` or `const` is always emitted, so
             * a function its initializer names (`var g: fn(string) ->
             * string = strfns.lit`) is reached: swept, it was undeclared
             * where the static was defined. */
            case AST_CONST_DECLARATION:
            /* #3142: an owned type, whose drop function its values call. */
            case AST_DISTINCT_TYPE_DEF:
                prune_collect_calls(c, &reachable, &worklist);
                break;
            case AST_FUNCTION_DEFINITION:
            case AST_BUILDER_FUNCTION:
                /* #2297: a package library exports its modules' functions,
                 * so they are roots even though nothing in the entry calls
                 * them. */
                if (!c->is_imported || (c->annotation && strncmp(c->annotation, "c_callback:", 11) == 0) ||
                    module_lib_package_module_of(c)) {
                    if (c->value) {
                        if (nameset_add(&reachable, c->value)) {
                            namestack_push(&worklist, c->value);
                        }
                    }
                    prune_collect_calls(c, &reachable, &worklist);
                }
                break;
            default:
                break;
        }
    }

    // Closure: drain worklist, walking each newly reachable function.
    // For each name we also pull in any imported function whose prefixed
    // form ends with `_<name>` — that's how glob-import (`import mathlist
    // (*)`) and selective-import (`import mathlist [cube]`) callers reach
    // a merged `mathlist_cube` by writing the bare `cube`. Without this
    // suffix match the prune sweep would drop those merged bodies even
    // though user code does call them.
    while (worklist.count > 0) {
        char* name = worklist.names[--worklist.count];
        PruneEntry* e = strmap_get(&index, name);
        for (int k = 0; e && k < e->def_count; k++)
            prune_collect_calls(e->defs[k], &reachable, &worklist);
        for (int k = 0; e && k < e->suffix_count; k++) {
            ASTNode* c = e->suffix_defs[k];
            if (nameset_add(&reachable, c->value)) {
                namestack_push(&worklist, c->value);
            }
        }
        free(name);
    }
    free(worklist.names);
    prune_index_free(&index);

    // Record the imported functions/builders the closure never reached;
    // they stay in the program until module_sweep_unreachable, so the
    // typechecker checks them like every other function.
    int cap = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!(c && c->is_imported && c->value &&
              (c->type == AST_FUNCTION_DEFINITION || c->type == AST_BUILDER_FUNCTION) &&
              !nameset_contains(&reachable, c->value))) continue;
        if (g_unreachable_count == cap) {
            int new_cap = cap ? cap * 2 : 64;
            ASTNode** grown = realloc(g_unreachable, sizeof(ASTNode*) * (size_t)new_cap);
            if (!grown) break;   /* OOM: what is not recorded is kept and emitted */
            g_unreachable = grown;
            cap = new_cap;
        }
        g_unreachable[g_unreachable_count++] = c;
    }

    nameset_free(&reachable);
}

/* An imported constant the sweep may drop: one merged in from a module
 * (origin_module; a merged constant is not marked is_imported) that the
 * module wrote as a plain `const`, a scalar or an array. Not a module `var`
 * (global_var), not one a C header owns (c_import_const), and not one a
 * package library exports. */
static int const_sweepable(const ASTNode* c) {
    if (!c || c->type != AST_CONST_DECLARATION || !c->origin_module || !c->value) return 0;
    if (c->annotation && strcmp(c->annotation, "array_const") != 0) return 0;
    return module_lib_package_module_of(c) == NULL;
}

/* Every name `node` and what it holds spell, normalised (`fs.KIND_OK` files
 * as `fs_KIND_OK`). A superset of the references: any identifier, call,
 * member or pattern that spells a constant's name keeps the constant. */
static void const_refs_collect(const ASTNode* node, NameSet* refs) {
    if (!node) return;
    if (node->value) nameset_add(refs, node->value);
    for (int i = 0; i < node->child_count; i++) const_refs_collect(node->children[i], refs);
}

/* Is the constant `name` spelled among `refs`, by its merged name or by any
 * `_`-separated tail of it (the bare `KIND_OK` a glob or selective import
 * writes for `fs_KIND_OK`, the same suffix match the function prune makes)? */
static int const_is_referenced(const NameSet* refs, const char* name) {
    if (nameset_contains(refs, name)) return 1;
    for (const char* p = strchr(name, '_'); p; p = strchr(p + 1, '_')) {
        if (p[1] && nameset_contains(refs, p + 1)) return 1;
    }
    return 0;
}

/* The imported constants nothing names, dropped. Each is a file-scope
 * `static const` in the C, and an unused one is -Wunused-const-variable,
 * which -Wall turns on for C on gcc and clang: `import std.fs` alone gave a
 * program fourteen of them (ae_const_fs_KIND_*), so it could not be built
 * with -Wall -Werror. Run with the function sweep, after type checking, so
 * the dropped ones were checked like every other declaration. A constant
 * another kept constant's initializer names is kept with it. */
static void module_sweep_unreferenced_consts(ASTNode* program) {
    int candidates = 0;
    for (int i = 0; i < program->child_count; i++) {
        if (const_sweepable(program->children[i])) candidates++;
    }
    if (candidates == 0) return;
    char* keep = calloc((size_t)program->child_count, 1);
    if (!keep) return;   /* OOM: every constant is kept and emitted */
    NameSet refs = {0};
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!const_sweepable(c)) const_refs_collect(c, &refs);
    }
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (keep[i] || !const_sweepable(c) || !const_is_referenced(&refs, c->value)) continue;
            keep[i] = 1;
            changed = 1;
            for (int k = 0; k < c->child_count; k++) const_refs_collect(c->children[k], &refs);
        }
    }
    int kept = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (const_sweepable(c) && !keep[i]) {
            free_ast_node(c);
        } else {
            program->children[kept++] = c;
        }
    }
    program->child_count = kept;
    nameset_free(&refs);
    free(keep);
}

void module_sweep_unreachable(ASTNode* program) {
    if (!program || g_unreachable_count == 0) {
        module_unreachable_reset();
        /* After the functions, so a constant only a dropped one named
         * goes too. */
        if (program) module_sweep_unreferenced_consts(program);
        return;
    }
    qsort(g_unreachable, (size_t)g_unreachable_count, sizeof(ASTNode*),
          unreachable_ptr_cmp);
    // Compaction is in-place; freeing the dead AST sub-trees keeps them
    // out of every pass after type checking and out of the C.
    int kept = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (c && bsearch(&c, g_unreachable, (size_t)g_unreachable_count,
                         sizeof(ASTNode*), unreachable_ptr_cmp)) {
            /* The trusted-call pass recorded the calls of every checked
             * body; the ones in this body go with it. */
            sandbox_trust_forget_within(c);
            free_ast_node(c);
        } else {
            program->children[kept++] = c;
        }
    }
    program->child_count = kept;
    module_unreachable_reset();
    module_sweep_unreferenced_consts(program);
}

void module_prune_unreachable(ASTNode* program) {
    module_mark_unreachable(program);
    module_sweep_unreachable(program);
}

