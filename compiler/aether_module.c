#include "aether_module.h"
#include "aether_error.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
    #define access _access
    #define F_OK 0
#else
    #include <unistd.h>
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

void module_set_source_dir(const char* source_path) {
    module_registry_init();
    if (!source_path) { global_module_registry->source_dir[0] = '\0'; return; }
    strncpy(global_module_registry->source_dir, source_path, sizeof(global_module_registry->source_dir) - 1);
    global_module_registry->source_dir[sizeof(global_module_registry->source_dir) - 1] = '\0';
    // Strip filename to get directory
    char* last_sep = NULL;
    for (char* p = global_module_registry->source_dir; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (last_sep) {
        *(last_sep + 1) = '\0';  // keep trailing slash
    } else {
        global_module_registry->source_dir[0] = '\0';  // no directory component
    }
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
     * separate `--lib` flags. `aether_lib_path_normalize` is a
     * no-op on POSIX.
     *
     * memcpy with an explicit length (rather than strncpy with
     * `sizeof(dst)-1`) keeps GCC's `-Wstringop-truncation` happy
     * AND is the faster shape. */
    char norm[256];
    aether_lib_path_normalize(dir, norm, sizeof(norm));
    size_t nlen = strlen(norm);
    while (nlen > 1 &&
           (norm[nlen - 1] == '/' || norm[nlen - 1] == '\\') &&
           norm[nlen - 2] != ':') {
        norm[--nlen] = '\0';
    }
    /* Skip duplicates so repeated `--lib /same/dir` doesn't waste
     * search slots. O(N) check over a fixed cap-of-8 list — trivial. */
    for (int i = 0; i < global_module_registry->lib_dir_count; i++) {
        if (strcmp(global_module_registry->lib_dirs[i], norm) == 0) return;
    }
    if (global_module_registry->lib_dir_count >= AETHER_LIB_DIRS_MAX) {
        fprintf(stderr,
            "warning: --lib search path is full (max %d entries); "
            "ignoring '%s'\n", AETHER_LIB_DIRS_MAX, norm);
        return;
    }
    int idx = global_module_registry->lib_dir_count;
    /* +1 includes the NUL — `nlen` is post-normalisation length,
     * always < sizeof(lib_dirs[idx]). memcpy here too: same warning
     * + perf rationale. */
    memcpy(global_module_registry->lib_dirs[idx], norm, nlen + 1);
    global_module_registry->lib_dir_count++;
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
    char buf[256];
    while (*cur) {
        const char* next = strchr(cur, AETHER_LIB_PATH_SEP_CHAR);
        size_t len = next ? (size_t)(next - cur) : strlen(cur);
        if (len > 0) {
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, cur, len);
            buf[len] = '\0';
            module_add_lib_dir(buf);
        }
        if (!next) break;
        cur = next + 1;
    }
}

void module_set_lib_dir(const char* lib_dir) {
    module_registry_init();
    if (!lib_dir || !lib_dir[0]) return;
    /* RESET — a fresh `--lib <path>` (or `AETHER_LIB_DIR=<path>`)
     * replaces, doesn't append. The append-form is
     * `module_add_lib_dirs`. */
    global_module_registry->lib_dir_count = 0;
    module_add_lib_dirs(lib_dir);
    /* Defensive: an entirely-empty path (all separators, no
     * segments) should fall back to the default so the toolchain
     * still finds stdlib modules. */
    if (global_module_registry->lib_dir_count == 0) {
        module_add_lib_dir(AETHER_DEFAULT_LIB_DIR);
    }
}

// Module management
void module_registry_init() {
    if (!global_module_registry) {
        global_module_registry = (ModuleRegistry*)malloc(sizeof(ModuleRegistry));
        global_module_registry->modules = NULL;
        global_module_registry->module_count = 0;
        global_module_registry->module_capacity = 0;
        global_module_registry->source_dir[0] = '\0';
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

void module_registry_shutdown() {
    if (global_module_registry) {
        for (int i = 0; i < global_module_registry->module_count; i++) {
            module_free(global_module_registry->modules[i]);
        }
        free(global_module_registry->modules);
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
    return module;
}

void module_free(AetherModule* module) {
    if (!module) return;
    
    free(module->name);
    free(module->file_path);
    
    if (module->ast) {
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

int module_is_exported(AetherModule* module, const char* symbol) {
    if (!module) return 0;

    for (int i = 0; i < module->export_count; i++) {
        if (strcmp(module->exports[i], symbol) == 0) {
            return 1;
        }
    }

    return 0;
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

DependencyGraph* dependency_graph_create() {
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
        if (pn <= 0) return 0;
        buf[pn] = '\0';
    }
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, bufsz - 1);
    if (n <= 0) return 0;
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
    char path[4096];

    // Try 1: Local development path (relative to CWD)
    snprintf(path, sizeof(path), "%s/%s/module.ae", root, converted);
    if (access(path, F_OK) == 0) return strdup(path);

    // Try 2: Installed path via AETHER_HOME
    const char* aether_home = getenv("AETHER_HOME");
    if (aether_home && aether_home[0]) {
        snprintf(path, sizeof(path), "%s/share/aether/%s/%s/module.ae", aether_home, root, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/%s/%s/module.ae", aether_home, root, converted);
        if (access(path, F_OK) == 0) return strdup(path);
    }

    // Try 3: Relative to the running aetherc binary. This is the
    // canonical lookup for installed downstream consumers — `ae
    // build` invokes aetherc from $prefix/bin/, so $exe_dir/../
    // points at the install prefix and ../share/aether/<root>/
    // finds the module.ae regardless of which prefix was used at
    // install time.
    char exe_dir[512];
    if (get_exe_directory(exe_dir, sizeof(exe_dir))) {
        snprintf(path, sizeof(path), "%s/../share/aether/%s/%s/module.ae", exe_dir, root, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/../%s/%s/module.ae", exe_dir, root, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/../lib/%s/%s/module.ae", exe_dir, root, converted);
        if (access(path, F_OK) == 0) return strdup(path);
    }

    // Try 4: User home directory (~/.aether)
    const char* home = get_user_home_dir();
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.aether/share/aether/%s/%s/module.ae", home, root, converted);
        if (access(path, F_OK) == 0) return strdup(path);
    }

#ifndef _WIN32
    // Try 5: System install locations (POSIX only)
    snprintf(path, sizeof(path), "/usr/local/share/aether/%s/%s/module.ae", root, converted);
    if (access(path, F_OK) == 0) return strdup(path);
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
    char converted[512];
    strncpy(converted, module_name, sizeof(converted) - 1);
    converted[sizeof(converted) - 1] = '\0';
    for (char* p = converted; *p; p++) {
        if (*p == '.') *p = '/';
    }
    return resolve_pkg_path("std", converted);
}

char* module_resolve_contrib_path(const char* module_name) {
    // Same dot-to-slash conversion as the stdlib resolver — required
    // for nested contrib modules like `contrib.host.python` →
    // `contrib/host/python/module.ae`.
    char converted[512];
    strncpy(converted, module_name, sizeof(converted) - 1);
    converted[sizeof(converted) - 1] = '\0';
    for (char* p = converted; *p; p++) {
        if (*p == '.') *p = '/';
    }
    return resolve_pkg_path("contrib", converted);
}

#define PKG_MAX_MATCHES 16

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
    char path[4096];
    if (sub_path) {
        snprintf(path, sizeof(path), "%s/src%s/module.ae", root, sub_path);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/src%s.ae", root, sub_path);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/lib%s/module.ae", root, sub_path);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s%s/module.ae", root, sub_path);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s%s.ae", root, sub_path);
        if (access(path, F_OK) == 0) return strdup(path);
    } else {
        snprintf(path, sizeof(path), "%s/src/module.ae", root);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/module.ae", root);
        if (access(path, F_OK) == 0) return strdup(path);
    }
    return NULL;
}

// Resolve a local module path (e.g., "mypackage.utils") to a file path.
char* module_resolve_local_path(const char* module_path) {
    char converted[512];
    char path[4096];

    // Convert dots to slashes
    strncpy(converted, module_path, sizeof(converted) - 1);
    converted[sizeof(converted) - 1] = '\0';
    for (char* p = converted; *p; p++) {
        if (*p == '.') *p = '/';
    }

    /* Try 1/2: search every CWD-relative lib_dirs[] entry in order,
     * first hit wins. Each entry probes both shapes (`<entry>/
     * <module>/module.ae` then `<entry>/<module>.ae`) so the
     * historical resolution semantics for any single entry are
     * preserved — what's new is the loop over multiple roots.
     * Issue #413. */
    for (int li = 0; li < global_module_registry->lib_dir_count; li++) {
        const char* lib = global_module_registry->lib_dirs[li];
        snprintf(path, sizeof(path), "%s/%s/module.ae", lib, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s/%s.ae", lib, converted);
        if (access(path, F_OK) == 0) return strdup(path);
    }

    // Try 3: src/module_path/module.ae
    snprintf(path, sizeof(path), "src/%s/module.ae", converted);
    if (access(path, F_OK) == 0) return strdup(path);

    // Try 4: src/module_path.ae
    snprintf(path, sizeof(path), "src/%s.ae", converted);
    if (access(path, F_OK) == 0) return strdup(path);

    // Try 5: module_path/module.ae (project root)
    snprintf(path, sizeof(path), "%s/module.ae", converted);
    if (access(path, F_OK) == 0) return strdup(path);

    // Try 6: module_path.ae (single file in root)
    snprintf(path, sizeof(path), "%s.ae", converted);
    if (access(path, F_OK) == 0) return strdup(path);

    // Try 6b: Search relative to source file directory
    if (global_module_registry->source_dir[0]) {
        /* Mirror the CWD-relative loop above, but anchored at the
         * source file's directory. Same left-to-right semantics
         * across the multi-entry lib path. Issue #413. */
        for (int li = 0; li < global_module_registry->lib_dir_count; li++) {
            const char* lib = global_module_registry->lib_dirs[li];
            snprintf(path, sizeof(path), "%s%s/%s/module.ae", global_module_registry->source_dir, lib, converted);
            if (access(path, F_OK) == 0) return strdup(path);
            snprintf(path, sizeof(path), "%s%s/%s.ae", global_module_registry->source_dir, lib, converted);
            if (access(path, F_OK) == 0) return strdup(path);
        }
        snprintf(path, sizeof(path), "%ssrc/%s/module.ae", global_module_registry->source_dir, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%ssrc/%s.ae", global_module_registry->source_dir, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s%s/module.ae", global_module_registry->source_dir, converted);
        if (access(path, F_OK) == 0) return strdup(path);
        snprintf(path, sizeof(path), "%s%s.ae", global_module_registry->source_dir, converted);
        if (access(path, F_OK) == 0) return strdup(path);
    }

    // Try 7-9: Search installed packages at ~/.aether/packages/
    // import mylib.utils → search ~/.aether/packages/*/mylib/src/utils/module.ae
    // The first path component (mylib) is the package name
    {
        const char* home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");  // Windows
        if (home) {
            // Extract the package name (first component of module path)
            char pkg_name[128];
            strncpy(pkg_name, converted, sizeof(pkg_name) - 1);
            pkg_name[sizeof(pkg_name) - 1] = '\0';
            char* slash = strchr(pkg_name, '/');
            if (slash) *slash = '\0';

            // The sub-path within the package (everything after package name)
            const char* sub_path = strchr(converted, '/');

            char pkg_base[512];
            snprintf(pkg_base, sizeof(pkg_base), "%s/.aether/packages", home);

            // Flat layout: ~/.aether/packages/<pkg_name>/
            char root[2048];
            snprintf(root, sizeof(root), "%s/%s", pkg_base, pkg_name);
            char* hit = pkg_probe_root(root, sub_path);
            if (hit) return hit;

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
             * to guess when two owners ship the same package name. */
            char* matches[PKG_MAX_MATCHES];
            int match_count = 0;
            int match_overflow = 0;
            size_t base_len = strlen(pkg_base);

            DIR* d_host = opendir(pkg_base);
            if (d_host) {
                struct dirent* e_host;
                while ((e_host = readdir(d_host)) != NULL) {
                    if (e_host->d_name[0] == '.') continue;
                    char host_dir[1024];
                    snprintf(host_dir, sizeof(host_dir), "%s/%s", pkg_base, e_host->d_name);
                    DIR* d_owner = opendir(host_dir);
                    if (!d_owner) continue;
                    struct dirent* e_owner;
                    while ((e_owner = readdir(d_owner)) != NULL) {
                        if (e_owner->d_name[0] == '.') continue;
                        snprintf(root, sizeof(root), "%s/%s/%s",
                                 host_dir, e_owner->d_name, pkg_name);
                        char* found = pkg_probe_root(root, sub_path);
                        if (!found) continue;
                        if (match_count < PKG_MAX_MATCHES) {
                            matches[match_count++] = found;
                        } else {
                            match_overflow = 1;
                            free(found);
                        }
                    }
                    closedir(d_owner);
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
                     * printed straight out of its match path, no copy. */
                    char msg[512];
                    int off = snprintf(msg, sizeof(msg),
                                       "ambiguous package import '%s': installed under ",
                                       pkg_name);
                    for (int m = 0; m < match_count && off < (int)sizeof(msg) - 64; m++) {
                        const char* rel = matches[m] + base_len + 1;
                        const char* host_end = strchr(rel, '/');
                        const char* owner_end = host_end ? strchr(host_end + 1, '/') : NULL;
                        int owner_len = owner_end ? (int)(owner_end - rel)
                                                  : (int)strlen(rel);
                        off += snprintf(msg + off, sizeof(msg) - off,
                                        "%s%.*s", (m ? ", " : ""), owner_len, rel);
                    }
                    snprintf(msg + off, sizeof(msg) - off,
                             "%s. Remove the duplicate from ~/.aether/packages.",
                             match_overflow ? ", ..." : "");
                    aether_error_simple(msg, 0, 0);
                    for (int m = 0; m < match_count; m++) free(matches[m]);
                    return NULL;
                }

                return matches[0];
            }
        }
    }

    return NULL;
}

// Parse a module file into an AST. Saves/restores lexer state.
ASTNode* module_parse_file(const char* file_path) {
    FILE* f = fopen(file_path, "r");
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

    char* source = malloc((size_t)size + 1);
    if (!source) {
        fclose(f);
        return NULL;
    }

    size_t bytes_read = fread(source, 1, size, f);
    fclose(f);
    source[bytes_read] = '\0';

    // Save lexer state (lexer is global)
    LexerState saved;
    lexer_save(&saved);

    // Re-point the diagnostic source context at THIS module for the
    // duration of its parse, so any parse error names the module's own
    // file and renders from the module's own buffer — not the importing
    // file's, which would mislabel the location and print an unrelated
    // source snippet (#646). Restored before `source` is freed below so
    // the context never dangles; nested imports save/restore correctly
    // because each call frame keeps its caller's context on the stack.
    const char* saved_err_filename = NULL;
    const char* saved_err_source = NULL;
    aether_error_get_source(&saved_err_filename, &saved_err_source);
    aether_error_set_source(file_path, source);

    // Tokenize
    lexer_init(source);
    // Heap-allocated to avoid a large fixed stack array; this also lets the
    // cap grow without risking stack overflow for token-dense modules.
    Token** tokens = malloc(sizeof(Token*) * MAX_MODULE_TOKENS);
    if (!tokens) {
        aether_error_set_source(saved_err_filename, saved_err_source);
        free(source);
        lexer_restore(&saved);
        return NULL;
    }
    int token_count = 0;

    while (token_count < MAX_MODULE_TOKENS - 1) {
        Token* token = next_token();
        tokens[token_count++] = token;
        if (token->type == TOKEN_EOF || token->type == TOKEN_ERROR) break;
    }

    // Parse
    Parser* parser = create_parser(tokens, token_count);
    ASTNode* ast = parse_program(parser);

    // Restore the caller's diagnostic source context before freeing this
    // module's source buffer (which current_source would otherwise point
    // into).
    aether_error_set_source(saved_err_filename, saved_err_source);

    // Stamp every node with the source path before this AST gets
    // cloned into the merged program. The clone preserves
    // source_file, so codegen can emit `#line N "path"` directives
    // pointing at the right .ae file even after module merging.
    if (ast) ast_stamp_source_file(ast, file_path);

    // Cleanup
    for (int i = 0; i < token_count; i++) {
        free_token(tokens[i]);
    }
    free(tokens);
    free_parser(parser);
    free(source);

    // Restore lexer state
    lexer_restore(&saved);

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
    if (prefix_len <= 0 || prefix_len >= 512) return NULL;
    char prefix[512];
    memcpy(prefix, module_path, prefix_len);
    prefix[prefix_len] = '\0';
    const char* symbol = last_dot + 1;
    if (!*symbol) return NULL;

    if (strncmp(prefix, "std.", 4) == 0) {
        file_path = module_resolve_stdlib_path(prefix + 4);
    } else if (strncmp(prefix, "contrib.", 8) == 0) {
        file_path = module_resolve_contrib_path(prefix + 8);
    } else {
        file_path = module_resolve_local_path(prefix);
    }
    if (!file_path) return NULL;

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
    import_node->value = strdup(prefix);

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
    const char* selected_names[256];
    const char* selected_from[256];
    int selected_count = 0;
    for (int i = 0; i < ast->child_count && selected_count < 256; i++) {
        ASTNode* child = ast->children[i];
        if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
        for (int sk = 0; sk < child->child_count && selected_count < 256; sk++) {
            ASTNode* sel = child->children[sk];
            if (!sel || sel->type != AST_IDENTIFIER || !sel->value) continue;
            if (sel->annotation &&
                strcmp(sel->annotation, "module_alias") == 0) continue;
            /* Per-symbol alias: the LOCAL name a def could shadow is
             * the alias, not the exported name (`import vg (path as
             * vgpath)` frees up `path` for local use; a local `vgpath`
             * is now the collision). */
            if (sel->annotation &&
                strncmp(sel->annotation, "select_alias:", 13) == 0) {
                selected_names[selected_count] = sel->annotation + 13;
            } else {
                selected_names[selected_count] = sel->value;
            }
            selected_from[selected_count] = child->value;
            selected_count++;
        }
    }
    if (selected_count == 0) return 1;  /* Fast path. */
    for (int i = 0; i < ast->child_count; i++) {
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
        if (!fn_name) continue;
        for (int s = 0; s < selected_count; s++) {
            if (strcmp(fn_name, selected_names[s]) == 0) {
                fprintf(stderr,
                    "error[E1000]: %s%s%s%s defines local function '%s' "
                    "(%d:%d) but also selectively imports '%s' from '%s'\n",
                    ctx_label,
                    ctx_path ? " (" : "",
                    ctx_path ? ctx_path : "",
                    ctx_path ? ")" : "",
                    fn_name, fn_line, fn_col,
                    selected_names[s], selected_from[s]);
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
                    selected_from[s]);
                return 0;
            }
        }
    }
    return 1;
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
    char ctx[300];
    snprintf(ctx, sizeof(ctx), "module '%s'", module_name);
    if (!check_selective_import_shadow(ast, ctx, file_path)) return 0;

    // Add node to dependency graph
    dependency_graph_add_node(graph, module_name);

    // Recursively process this module's imports
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;

        char* sub_file = resolve_import_path(child);
        const char* sub_path = child->value;

        dependency_graph_add_edge(graph, module_name, sub_path);
        module_add_import(mod, sub_path);

        if (sub_file) {
            if (!orchestrate_module(sub_path, sub_file, graph)) {
                free(sub_file);
                return 0;
            }
            free(sub_file);
        }
    }

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

    DependencyGraph* graph = dependency_graph_create();
    dependency_graph_add_node(graph, "__main__");

    // Scan top-level AST for imports
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;

        char* file_path = resolve_import_path(child);
        const char* module_path = child->value;

        dependency_graph_add_edge(graph, "__main__", module_path);

        if (file_path) {
            if (!orchestrate_module(module_path, file_path, graph)) {
                free(file_path);
                dependency_graph_free(graph);
                return 0;
            }
            free(file_path);
        }
        // If file not found: silently continue (backwards compat)
    }

    // Check for cycles
    if (dependency_graph_has_cycle(graph)) {
        dependency_graph_free(graph);
        return 0;
    }

    dependency_graph_free(graph);

    /* All imports are now registered, but merge hasn't run yet — so the
     * program's top-level functions are still exactly the user's. Reject
     * any that forge an imported export's mangled C symbol (the silent
     * `proxy_opts_new` ↔ `proxy.opts_new` shadow). */
    if (!check_namespace_prefix_collision(program)) return 0;

    return 1;
}

// --- Pure Aether Module Merging ---

// Soft ceiling for the per-module decl-name tables that
// module_merge_into_program builds while cloning a module's bodies into
// the consumer's program AST.
//
// Every clone-and-rename pass collects a snapshot of the source module's
// function and constant names so that intra-module calls (`square(...)`
// referencing a sibling) can be rewritten to the prefixed form
// (`mathy_square(...)`) before the body lands in the consumer.
//
// Pre-fix this was hardcoded to 128 at every collection site. Modules
// merged from large per-file shims (avn's working_copy/module.ae,
// ~150 funcs in 4300 lines) silently truncated at the 129th decl —
// every call to a beyond-128 sibling stayed bare and the consumer's
// typer fired E0301 even though the symbol existed in the registry
// under its prefixed name. The cap below leaves headroom for very
// large merged modules without forcing dynamic allocation.
//
// Stack-frame budget: each merge pass allocates up to ~6 arrays of
// (const char*) at this cap → 4096 × 8 bytes × 6 ≈ 192 KB peak. Well
// within Linux/macOS 8 MB defaults and Windows 1 MB default. If a
// future module pushes past 4096 we should switch to dynamic
// allocation rather than another bump.
#define AETHER_MODULE_MAX_DECLS 4096

// Extract namespace from module path: "mypackage.utils" -> "utils"
static const char* module_get_namespace(const char* module_path) {
    const char* last_dot = strrchr(module_path, '.');
    if (last_dot) return last_dot + 1;
    return module_path;
}

// Get the actual declaration from a node (unwrap AST_EXPORT_STATEMENT if needed)
static ASTNode* unwrap_export(ASTNode* node) {
    if (node->type == AST_EXPORT_STATEMENT && node->child_count > 0) {
        return node->children[0];
    }
    return node;
}

// Collect all function names defined in a module AST
static int collect_module_func_names(ASTNode* mod_ast, const char** names, int max) {
    int count = 0;
    for (int i = 0; i < mod_ast->child_count && count < max; i++) {
        ASTNode* decl = unwrap_export(mod_ast->children[i]);
        if ((decl->type == AST_FUNCTION_DEFINITION || decl->type == AST_BUILDER_FUNCTION) && decl->value) {
            names[count++] = decl->value;
        }
    }
    return count;
}

// Collect all constant names defined in a module AST
static int collect_module_const_names(ASTNode* mod_ast, const char** names, int max) {
    int count = 0;
    for (int i = 0; i < mod_ast->child_count && count < max; i++) {
        ASTNode* decl = unwrap_export(mod_ast->children[i]);
        if (decl->type == AST_CONST_DECLARATION && decl->value) {
            names[count++] = decl->value;
        }
        /* #error-unification P3: fault members are const-like — a bare
         * `return NotFound` inside the module must be renamed to
         * `<ns>_NotFound` just like a const reference, so include each
         * fault member's bare name in the intra-module rename list. */
        else if (decl->type == AST_FAULT_DEFINITION) {
            for (int mi = 0; mi < decl->child_count && count < max; mi++) {
                ASTNode* mem = decl->children[mi];
                if (mem && mem->value) names[count++] = mem->value;
            }
        }
    }
    return count;
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
    AetherModule* m = module_find(ns);
    if (!m) {
        // Modules are registered under their FULL dotted path
        // ("std.spec", "contrib.foo", nested local "a.b"), but `ns` here
        // is the short namespace tail ("spec") the codegen prefix is
        // built from — so module_find(ns) misses for every dotted path.
        // Fall back to scanning the registry for a module whose
        // namespace tail matches `ns`. Without this the mutable-`var`
        // write rename below never fires for a dotted-path module, and
        // `name = expr` against a module global lowers to a shadowing
        // local instead of a store to the shared static (a silent
        // miscompile → NULL global → crash on the next read). Single-
        // segment local imports (full path == namespace) already hit the
        // exact match above and are unaffected.
        //
        // KNOWN LIMIT: the scan takes the FIRST module whose namespace
        // tail matches, so two co-imported dotted modules sharing a tail
        // ("std.spec" and "a.spec") could resolve to the wrong one —
        // same global-namespace family as the cross-module struct-name
        // collisions. Correct fix is plumbing the full dotted path to
        // this call; do that if a real collision ever appears.
        if (global_module_registry) {
            for (int i = 0; i < global_module_registry->module_count; i++) {
                AetherModule* cand = global_module_registry->modules[i];
                if (cand && cand->name &&
                    strcmp(module_get_namespace(cand->name), ns) == 0) {
                    m = cand;
                    break;
                }
            }
        }
    }
    if (!m || !m->ast) return 0;
    for (int i = 0; i < m->ast->child_count; i++) {
        ASTNode* decl = unwrap_export(m->ast->children[i]);
        if (decl && decl->type == AST_CONST_DECLARATION && decl->value &&
            decl->annotation && strcmp(decl->annotation, "global_var") == 0 &&
            strcmp(decl->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Does the module declare an extern C function with this exact name?
//
// This is the check that prevents the namespace-prefix/extern collision
// bug: if a module named `contrib.aether_ui` has both
//     extern aether_ui_app_create(...) -> int       // raw C name
//     app_create(...) { return aether_ui_app_create(...) }  // wrapper
// then merging the wrapper with the `aether_ui_` prefix produces a C
// symbol that is identical to the extern's name. Both then live in the
// same translation unit: the extern is a forward declaration, the
// wrapper is the definition. The wrapper's body calls the extern by
// name, but C resolves the call to the nearest definition — itself —
// and the process infinite-recurses until the stack blows (rc=139).
//
// The fix is to detect this collision at merge time and skip emitting
// the wrapper. Qualified calls `aether_ui.app_create(...)` from outside
// the module are already mangled by codegen to `aether_ui_app_create`
// (see normalize_func_name in codegen_func.c), which is exactly the
// extern's name — so with the wrapper gone, those calls resolve
// directly to the extern. Net effect: zero change for callers, no
// recursive stub in the generated C.
static int module_has_extern_named(ASTNode* mod_ast, const char* name) {
    if (!mod_ast || !name) return 0;
    for (int i = 0; i < mod_ast->child_count; i++) {
        ASTNode* decl = unwrap_export(mod_ast->children[i]);
        if (decl && decl->type == AST_EXTERN_FUNCTION && decl->value &&
            strcmp(decl->value, name) == 0) {
            return 1;
        }
    }
    return 0;
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

    // User (pre-merge, top-level) function / builder / extern symbols.
    enum { MAX_USER = 4096 };
    const char* user_names[MAX_USER];
    int user_line[MAX_USER], user_col[MAX_USER];
    int un = 0;
    for (int i = 0; i < program->child_count && un < MAX_USER; i++) {
        ASTNode* d = unwrap_export(program->children[i]);
        if (!d || !d->value) continue;
        if (d->type == AST_FUNCTION_DEFINITION ||
            d->type == AST_BUILDER_FUNCTION ||
            d->type == AST_EXTERN_FUNCTION) {
            user_names[un] = d->value;
            user_line[un] = d->line;
            user_col[un]  = d->column;
            un++;
        }
    }
    if (un == 0) return 1;

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
        AetherModule* mod = module_find(child->value);
        if (!mod || !mod->ast) continue;
        const char* ns = module_get_namespace(child->value);
        if (!ns) continue;
        int has_selection = (child->child_count > 0 && child->children[0] &&
                             child->children[0]->type == AST_IDENTIFIER);

        for (int j = 0; j < mod->ast->child_count; j++) {
            ASTNode* decl = unwrap_export(mod->ast->children[j]);
            if (!decl || !decl->value) continue;
            // Only entities the merge prefixes into a callable `ns_name`
            // C symbol: Aether functions/builders and public `@extern`s.
            int callable = (decl->type == AST_FUNCTION_DEFINITION ||
                            decl->type == AST_BUILDER_FUNCTION ||
                            (decl->type == AST_EXTERN_FUNCTION && decl->annotation &&
                             strncmp(decl->annotation, "c_symbol:", 9) == 0));
            if (!callable) continue;
            if (has_selection) {
                int selected = 0;
                for (int k = 0; k < child->child_count; k++) {
                    ASTNode* sel = child->children[k];
                    if (sel && sel->type == AST_IDENTIFIER && sel->value &&
                        strcmp(sel->value, decl->value) == 0) { selected = 1; break; }
                }
                if (!selected) continue;
            }
            char prefixed[256];
            snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, decl->value);
            for (int u = 0; u < un; u++) {
                if (strcmp(user_names[u], prefixed) != 0) continue;
                fprintf(stderr,
                    "error[E1001]: local function '%s' (%d:%d) collides with "
                    "imported '%s.%s' from '%s'\n",
                    prefixed, user_line[u], user_col[u], ns, decl->value, child->value);
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
                return 0;
            }
        }
    }
    return 1;
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
static void collect_local_names(ASTNode* node, const char** names, int* count, int max) {
    if (!node || *count >= max) return;
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
        if (!name_in_list(node->value, names, *count)) {
            names[(*count)++] = node->value;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        // Don't recurse into nested function definitions (they have their own scope)
        if (node->children[i] && node->children[i]->type == AST_FUNCTION_DEFINITION) continue;
        collect_local_names(node->children[i], names, count, max);
    }
}

// Recursively rename intra-module function calls and constant references in a cloned AST.
// local_names/local_count: names bound in the enclosing function (params + locals) that shadow constants.
static void rename_intra_module_refs(ASTNode* node, const char* prefix,
                                      const char** func_names, int func_count,
                                      const char** const_names, int const_count,
                                      const char** local_names, int local_count) {
    if (!node) return;

    if (node->type == AST_FUNCTION_CALL && node->value) {
        // Check if this call targets a function defined in the same module
        for (int i = 0; i < func_count; i++) {
            if (strcmp(node->value, func_names[i]) == 0) {
                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", prefix, node->value);
                free(node->value);
                node->value = strdup(prefixed);
                break;
            }
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
        char prefixed[256];
        snprintf(prefixed, sizeof(prefixed), "%s_%s", prefix, node->value);
        free(node->value);
        node->value = strdup(prefixed);
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
    if (node->type == AST_BUILDER_FUNCTION && node->annotation) {
        for (int i = 0; i < func_count; i++) {
            if (strcmp(node->annotation, func_names[i]) == 0) {
                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", prefix, node->annotation);
                free(node->annotation);
                node->annotation = strdup(prefixed);
                break;
            }
        }
    }

    if (node->type == AST_IDENTIFIER && node->value) {
        // Only rename if this identifier matches a module constant AND is not
        // shadowed by a local variable or parameter in the enclosing function
        if (!name_in_list(node->value, local_names, local_count)) {
            int renamed = 0;
            for (int i = 0; i < const_count; i++) {
                if (strcmp(node->value, const_names[i]) == 0) {
                    char prefixed[256];
                    snprintf(prefixed, sizeof(prefixed), "%s_%s", prefix, node->value);
                    free(node->value);
                    node->value = strdup(prefixed);
                    renamed = 1;
                    break;
                }
            }
            // Also rewrite identifier-as-value references to module
            // functions (e.g. `my_handler` passed as a `ptr` argument
            // to a C extern that takes a function pointer). Without
            // this, the C compiler sees the unprefixed bare name and
            // fails with "undeclared identifier" — see #235. The
            // call-site rewrite above only catches AST_FUNCTION_CALL,
            // not value-position AST_IDENTIFIER.
            if (!renamed) {
                for (int i = 0; i < func_count; i++) {
                    if (strcmp(node->value, func_names[i]) == 0) {
                        char prefixed[256];
                        snprintf(prefixed, sizeof(prefixed), "%s_%s", prefix, node->value);
                        free(node->value);
                        node->value = strdup(prefixed);
                        break;
                    }
                }
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
    if (node->type == AST_CLOSURE) {
        const char* clo_locals[128];
        int clo_local_count = 0;
        /* The closure's OWN bindings go in first. The list is capped, and if
           a large enclosing scope filled it the parameters would be the names
           dropped — which is precisely the bug this branch exists to prevent.
           Enclosing names are appended after, and merely losing one of those
           costs a missed rename-suppression, not a wrong symbol. */
        collect_local_names(node, clo_locals, &clo_local_count, 128);
        for (int i = 0; i < local_count && clo_local_count < 128; i++) {
            if (!name_in_list(local_names[i], clo_locals, clo_local_count)) {
                clo_locals[clo_local_count++] = local_names[i];
            }
        }
        int kept_c = 0;
        for (int i = 0; i < clo_local_count; i++) {
            if (!name_is_module_global_var(prefix, clo_locals[i])) {
                clo_locals[kept_c++] = clo_locals[i];
            }
        }
        clo_local_count = kept_c;
        for (int i = 0; i < node->child_count; i++) {
            rename_intra_module_refs(node->children[i], prefix, func_names, func_count,
                                     const_names, const_count, clo_locals, clo_local_count);
        }
        return;
    }

    // When entering a function definition, collect its local names for shadowing checks.
    // Limit: 128 locals per function — excess names won't shadow module constants.
    if (node->type == AST_FUNCTION_DEFINITION) {
        const char* nested_locals[128];
        int nested_local_count = 0;
        collect_local_names(node, nested_locals, &nested_local_count, 128);
        /* #937: collect_local_names counts a bare `var = expr` write as a
         * local (it's shaped as an AST_VARIABLE_DECLARATION). For a module
         * `global_var` that's wrong — the write is to the shared global, and
         * leaving the name in `local_names` would shadow it out of the read
         * rename below (so reads-after-write stay bare and resolve to the
         * codegen's shadowing local). Drop global_var names so reads and the
         * write target both rename. A real local shadowing a `const` is NOT a
         * global_var, so it's correctly retained. */
        int kept = 0;
        for (int i = 0; i < nested_local_count; i++) {
            if (!name_is_module_global_var(prefix, nested_locals[i])) {
                nested_locals[kept++] = nested_locals[i];
            }
        }
        nested_local_count = kept;
        for (int i = 0; i < node->child_count; i++) {
            rename_intra_module_refs(node->children[i], prefix, func_names, func_count,
                                     const_names, const_count, nested_locals, nested_local_count);
        }
        return;
    }

    for (int i = 0; i < node->child_count; i++) {
        rename_intra_module_refs(node->children[i], prefix, func_names, func_count,
                                 const_names, const_count, local_names, local_count);
    }
}

// Is a function with the given (already-prefixed) name present in the program?
static int program_has_function(ASTNode* program, const char* prefixed_name) {
    if (!program || !prefixed_name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (existing && (existing->type == AST_FUNCTION_DEFINITION ||
            existing->type == AST_BUILDER_FUNCTION) &&
            existing->value && strcmp(existing->value, prefixed_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Is a constant with the given prefixed name already a top-level child of
// `program`? Mirrors program_has_function for the const-clone path — needed
// because the merge loop can now revisit a synthetic import (aether#1009), and
// re-cloning a module's consts without this guard emits a duplicate C
// definition (`redefinition of 'sha2_K256'`).
static int program_has_const(ASTNode* program, const char* prefixed_name) {
    if (!program || !prefixed_name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (existing && existing->type == AST_CONST_DECLARATION &&
            existing->value && strcmp(existing->value, prefixed_name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Is a fault member with the given (already-prefixed) C symbol name already
 * present in a merged AST_FAULT_DEFINITION? Dedup guard for a module imported
 * more than once — without it the member's `static const char` would be
 * emitted twice (duplicate C definition). */
static int program_has_fault_member(ASTNode* program, const char* prefixed_name) {
    if (!program || !prefixed_name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing || existing->type != AST_FAULT_DEFINITION) continue;
        for (int k = 0; k < existing->child_count; k++) {
            ASTNode* mem = existing->children[k];
            if (mem && mem->value && strcmp(mem->value, prefixed_name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

// Is a struct with the given name already a top-level child of `program`?
// Used by module_merge_into_program to dedup struct definitions when the
// consumer imports a module that exposes one. Imported structs share the
// consumer's struct namespace (no `<ns>_` prefix) — see the design rationale
// at the merge site.
static int program_has_struct(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_STRUCT_DEFINITION &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
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

/* #1006: an `actor X { ... }` already present in the program (the consumer's
 * own, or an earlier merge). Used to dedup cross-module actor merges. */
static int program_has_actor(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_ACTOR_DEFINITION &&
            unwrapped->value && strcmp(unwrapped->value, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* #1006: a `message X { ... }` already present in the program. Used to dedup
 * cross-module message merges. */
static int program_has_message(ASTNode* program, const char* name) {
    if (!program || !name) return 0;
    for (int m = 0; m < program->child_count; m++) {
        ASTNode* existing = program->children[m];
        if (!existing) continue;
        ASTNode* unwrapped = unwrap_export(existing);
        if (unwrapped && unwrapped->type == AST_MESSAGE_DEFINITION &&
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

static void apply_inherited_selective_imports(ASTNode* clone, ASTNode* mod_ast) {
    if (!clone || !mod_ast) return;

    for (int i = 0; i < mod_ast->child_count; i++) {
        ASTNode* imp = mod_ast->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;

        // #896: a glob import (`import M (*)`) brings every exported name of M
        // into the module's bare scope. Its selection list is NOT on the AST
        // (it's computed at typecheck time from M's exports), so it's handled
        // separately below — the selection is M's full export set rather than
        // the explicit selector children. A selective import needs its
        // identifier children; a glob has none.
        int is_glob = (imp->annotation &&
                       strcmp(imp->annotation, "glob_import") == 0);
        if (!is_glob) {
            if (imp->child_count == 0) continue;
            if (imp->children[0]->type != AST_IDENTIFIER) continue;
        }

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

        const char* sub_func_names[AETHER_MODULE_MAX_DECLS];
        int sub_func_count = collect_module_func_names(sub_mod->ast,
                                                       sub_func_names,
                                                       AETHER_MODULE_MAX_DECLS);
        const char* sub_const_names[AETHER_MODULE_MAX_DECLS];
        int sub_const_count = collect_module_const_names(sub_mod->ast,
                                                         sub_const_names,
                                                         AETHER_MODULE_MAX_DECLS);

        const char* sel_func_names[AETHER_MODULE_MAX_DECLS];
        int sel_func_count = 0;
        const char* sel_const_names[AETHER_MODULE_MAX_DECLS];
        int sel_const_count = 0;

        if (is_glob) {
            // #896: the glob selection is M's whole export surface. Take every
            // func/const M defines (skipping leading-underscore privates, the
            // same privacy rule the typecheck-time glob expansion applies) so
            // a bare `clean(...)` in M's merged body is rewritten to the
            // prefixed `fs_clean(...)` the transitive pass pulls in — exactly
            // what the selective and qualified forms already get.
            for (int k = 0; k < sub_func_count &&
                            sel_func_count < AETHER_MODULE_MAX_DECLS; k++) {
                if (sub_func_names[k] && sub_func_names[k][0] != '_') {
                    sel_func_names[sel_func_count++] = sub_func_names[k];
                }
            }
            for (int k = 0; k < sub_const_count &&
                            sel_const_count < AETHER_MODULE_MAX_DECLS; k++) {
                if (sub_const_names[k] && sub_const_names[k][0] != '_') {
                    sel_const_names[sel_const_count++] = sub_const_names[k];
                }
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
                    char prefixed[256];
                    snprintf(prefixed, sizeof(prefixed), "%s_%s", sub_ns, sel->value);
                    rename_aliased_import_refs(clone, sel->annotation + 13, prefixed,
                                               name_in_list(sel->value, sub_const_names,
                                                            sub_const_count));
                    continue;
                }
                if (name_in_list(sel->value, sub_func_names, sub_func_count) &&
                    sel_func_count < AETHER_MODULE_MAX_DECLS) {
                    sel_func_names[sel_func_count++] = sel->value;
                } else if (name_in_list(sel->value, sub_const_names, sub_const_count) &&
                           sel_const_count < AETHER_MODULE_MAX_DECLS) {
                    sel_const_names[sel_const_count++] = sel->value;
                }
            }
        }

        if (sel_func_count == 0 && sel_const_count == 0) continue;

        rename_intra_module_refs(clone, sub_ns,
                                 sel_func_names, sel_func_count,
                                 sel_const_names, sel_const_count,
                                 NULL, 0);
    }
}

// Walk a node looking for AST_FUNCTION_CALL targets that match a
// "<ns>_<name>" prefix where <name> is one of the module's own function
// names. Append unique matches into `out` (storing the bare name).
// Used to discover transitive intra-module callees in a cloned-and-
// renamed function body — see #171 P2.
static void collect_intra_module_callees(ASTNode* node, const char* ns,
                                          const char** mod_func_names, int mod_func_count,
                                          const char** out, int* out_count, int max) {
    if (!node || *out_count >= max) return;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        size_t ns_len = strlen(ns);
        if (strncmp(node->value, ns, ns_len) == 0 && node->value[ns_len] == '_') {
            const char* bare = node->value + ns_len + 1;
            for (int i = 0; i < mod_func_count; i++) {
                if (strcmp(bare, mod_func_names[i]) == 0) {
                    if (!name_in_list(bare, out, *out_count) && *out_count < max) {
                        out[(*out_count)++] = mod_func_names[i];
                    }
                    break;
                }
            }
        }
    }
    /* `builder name(...) with <factory>` carries the factory function
     * name in the builder's `annotation` field. After
     * rename_intra_module_refs has run, it's the already-prefixed
     * `<ns>_<factory>` form. Treat it as a callee dependency of the
     * builder so the transitive-pull-in loop above clones the
     * factory's body into the consumer TU. Without this, a
     * selectively-imported `builder win(...) with mkfac` from module
     * `smod` resolves `smod_win` correctly but leaves `smod_mkfac`
     * unresolved — bare `mkfac()` in the emitted C with no
     * declaration. Filed in aether/new_aevg_asks.md ASK 1. */
    if (node->type == AST_BUILDER_FUNCTION && node->annotation) {
        size_t ns_len = strlen(ns);
        if (strncmp(node->annotation, ns, ns_len) == 0 && node->annotation[ns_len] == '_') {
            const char* bare = node->annotation + ns_len + 1;
            for (int i = 0; i < mod_func_count; i++) {
                if (strcmp(bare, mod_func_names[i]) == 0) {
                    if (!name_in_list(bare, out, *out_count) && *out_count < max) {
                        out[(*out_count)++] = mod_func_names[i];
                    }
                    break;
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        collect_intra_module_callees(node->children[i], ns,
                                     mod_func_names, mod_func_count,
                                     out, out_count, max);
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
        if (imp->child_count > 0 && imp->children[0] &&
            imp->children[0]->type == AST_IDENTIFIER) continue;
        /* Dedup: skip if the program already carries a bare import of this
         * path (so two merged modules importing the same one inject once). */
        int already = 0;
        for (int p = 0; p < program->child_count; p++) {
            ASTNode* pc = program->children[p];
            if (pc && pc->type == AST_IMPORT_STATEMENT && pc->value &&
                strcmp(pc->value, imp->value) == 0 &&
                !(pc->child_count > 0 && pc->children[0] &&
                  pc->children[0]->type == AST_IDENTIFIER)) {
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

void module_merge_into_program(ASTNode* program) {
    if (!program || !global_module_registry) return;

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
        // AST just like user modules. collect_module_func_names only
        // collects AST_FUNCTION_DEFINITION nodes, so externs are untouched
        // by this path.

        AetherModule* mod = module_find(module_path);
        if (!mod || !mod->ast) continue;

        ASTNode* mod_ast = mod->ast;
        const char* ns = module_get_namespace(module_path);

        // Collect function and constant names for intra-module renaming
        const char* func_names[AETHER_MODULE_MAX_DECLS];
        int func_count = collect_module_func_names(mod_ast, func_names,
                                                   AETHER_MODULE_MAX_DECLS);
        const char* const_names[AETHER_MODULE_MAX_DECLS];
        int const_count = collect_module_const_names(mod_ast, const_names,
                                                     AETHER_MODULE_MAX_DECLS);

        // Check for selective import: if import has AST_IDENTIFIER children,
        // only merge functions/constants that appear in the selection list
        int has_selection = (child->child_count > 0 &&
                            child->children[0]->type == AST_IDENTIFIER);

        // #870: re-open the qualified-call surface for any module this
        // imported module bare-imports, so its merged bodies' `ns.fn(...)`
        // calls resolve even when the entry file imported the same module
        // selectively.
        inject_synthetic_bare_imports_from(program, mod_ast, &insert_idx);

        for (int j = 0; j < mod_ast->child_count; j++) {
            ASTNode* decl = unwrap_export(mod_ast->children[j]);

            if ((decl->type == AST_FUNCTION_DEFINITION || decl->type == AST_BUILDER_FUNCTION) && decl->value) {
                // Skip if not in selective import list. Note: even when
                // skipped here, the function may still be cloned later
                // by the transitive-pull-in pass below if a selectively
                // imported sibling calls it. See #171 P2.
                if (has_selection) {
                    int selected = 0;
                    for (int k = 0; k < child->child_count; k++) {
                        ASTNode* sel = child->children[k];
                        if (sel && sel->type == AST_IDENTIFIER &&
                            strcmp(sel->value, decl->value) == 0) {
                            selected = 1;
                            break;
                        }
                    }
                    if (!selected) continue;
                }

                // Build prefixed name: "double_it" -> "mymath_double_it"
                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, decl->value);

                // Collision with an extern declared in the same module.
                // See the comment on module_has_extern_named above for
                // why this check is necessary. Skipping the wrapper
                // makes qualified calls resolve directly to the extern.
                if (module_has_extern_named(mod_ast, prefixed)) continue;

                // Skip if already merged (e.g. from a prior non-selective import)
                if (program_has_function(program, prefixed)) continue;

                ASTNode* clone = clone_ast_node(decl);
                free(clone->value);
                clone->value = strdup(prefixed);
                // Mark as imported so codegen emits this function with the
                // C `static` storage class. Each translation unit that
                // imports the same module gets its own private copy, and
                // the linker no longer sees them as duplicate symbols
                // when several .o files are linked together (e.g. macOS
                // ld64, which does not support --allow-multiple-definition).
                clone->is_imported = 1;

                // Rename intra-module function calls and constant refs within the cloned body
                rename_intra_module_refs(clone, ns, func_names, func_count,
                                         const_names, const_count, NULL, 0);
                // Also rewrite bare-name calls to selectively-imported
                // names from M's own `import X (a, b)` statements — see
                // apply_inherited_selective_imports comment for why.
                apply_inherited_selective_imports(clone, mod_ast);

                insert_child_at(program, clone, insert_idx++);
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
                if (has_selection) {
                    int selected = 0;
                    for (int k = 0; k < child->child_count; k++) {
                        ASTNode* sel = child->children[k];
                        if (sel && sel->type == AST_IDENTIFIER &&
                            strcmp(sel->value, decl->value) == 0) {
                            selected = 1;
                            break;
                        }
                    }
                    if (!selected) continue;
                }

                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, decl->value);

                // Dedup — a module imported twice must not clone the
                // extern twice (duplicate registry entries / prototypes).
                int already = 0;
                for (int p = 0; p < program->child_count; p++) {
                    ASTNode* pc = program->children[p];
                    if (pc && pc->type == AST_EXTERN_FUNCTION && pc->value &&
                        strcmp(pc->value, prefixed) == 0) {
                        already = 1;
                        break;
                    }
                }
                if (already) continue;

                ASTNode* clone = clone_ast_node(decl);
                free(clone->value);
                clone->value = strdup(prefixed);
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_CONST_DECLARATION && decl->value) {
                // Skip if not in selective import list
                if (has_selection) {
                    int selected = 0;
                    for (int k = 0; k < child->child_count; k++) {
                        ASTNode* sel = child->children[k];
                        if (sel && sel->type == AST_IDENTIFIER &&
                            strcmp(sel->value, decl->value) == 0) {
                            selected = 1;
                            break;
                        }
                    }
                    if (!selected) continue;
                }

                // Skip if already merged (e.g. from a prior import of the same
                // module, or a synthetic revisit — aether#1009). Without this
                // the const is cloned twice → duplicate C definition.
                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, decl->value);
                if (program_has_const(program, prefixed)) continue;

                // Clone and rename constants too
                ASTNode* clone = clone_ast_node(decl);
                free(clone->value);
                clone->value = strdup(prefixed);

                // Rename references to other module constants in the value expression
                rename_intra_module_refs(clone, ns, func_names, func_count,
                                         const_names, const_count, NULL, 0);

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
                    if (!mem || !mem->value) continue;
                    /* Selective import: `import ns (Name)` filters members. */
                    if (has_selection) {
                        int selected = 0;
                        for (int k = 0; k < child->child_count; k++) {
                            ASTNode* sel = child->children[k];
                            if (sel && sel->type == AST_IDENTIFIER && sel->value &&
                                strcmp(sel->value, mem->value) == 0) {
                                selected = 1; break;
                            }
                        }
                        if (!selected) continue;
                    }
                    char sym[256];
                    snprintf(sym, sizeof(sym), "%s_%s", ns, mem->value);
                    if (program_has_fault_member(program, sym)) continue;
                    char qualified[512];
                    snprintf(qualified, sizeof(qualified), "%s.%s", ns, mem->value);
                    /* Rewrite the interned content (children[0], an
                     * AST_LITERAL) to the qualified name, then the symbol. */
                    if (mem->child_count > 0 && mem->children[0] &&
                        mem->children[0]->value) {
                        free(mem->children[0]->value);
                        mem->children[0]->value = strdup(qualified);
                    }
                    free(mem->value);
                    mem->value = strdup(sym);
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
                // consumer's program AST under their bare name (no
                // `<ns>_` prefix). Closes the cross-`import` struct
                // visibility gap that blocked the opaque-handle
                // pattern: a module exposing `new_slot() -> ptr` whose
                // body does `s = raw as *Slot` failed to typecheck once
                // imported, because the consumer's symbol table lacked
                // `Slot`.
                //
                // Bypasses the selective-import filter on purpose: a
                // function body that casts to `*Slot` cannot type-check
                // without the struct in scope, so even
                // `import handle (new_slot)` must pull in `Slot`.
                //
                // Dedup by name. If the consumer already has a struct
                // with the same name (its own, or an earlier merge),
                // we skip — name clashes between modules are out of
                // scope here; the C compiler will surface them downstream.
                if (program_has_struct(program, decl->value)) continue;

                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
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
                insert_child_at(program, clone, insert_idx++);
            } else if (decl->type == AST_MESSAGE_DEFINITION && decl->value) {
                // #1006: `message X { ... }` from an imported module enters the
                // consumer's program AST under its bare name (like structs), so
                // `actor ! X { ... }` sends and `receive { X(..) -> }` patterns
                // resolve, and codegen assigns X a runtime message-type id from
                // the (per-program) message registry. Bypasses the selective-
                // import filter on purpose: an imported actor's handlers cannot
                // type-check without their message types in scope. Dedup by name.
                if (program_has_message(program, decl->value)) continue;
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
                // Dedup by name.
                if (program_has_actor(program, decl->value)) continue;
                ASTNode* clone = clone_ast_node(decl);
                clone->is_imported = 1;
                // Unlike structs, an actor has a body: its receive handlers may
                // call module-local functions or read module constants, which
                // were cloned above under their `<ns>_` prefixed names. Rewrite
                // those references so they resolve, exactly as function bodies do.
                rename_intra_module_refs(clone, ns, func_names, func_count,
                                         const_names, const_count, NULL, 0);
                apply_inherited_selective_imports(clone, mod_ast);
                insert_child_at(program, clone, insert_idx++);
            }
            // Skip AST_MAIN_FUNCTION, AST_IMPORT_STATEMENT, etc.
        }
    }

    // Transitive pull-in: a selectively imported function may call
    // sibling helpers defined in the same module that weren't named in
    // the import list. The first-pass rename above rewrote those calls
    // to the prefixed `<ns>_<name>` form, but the helpers themselves
    // were skipped — leaving an unresolvable reference. Walk the merged
    // program until no new helpers appear (#171 P2).
    //
    // Visibility from outside the module is unchanged: the user's own
    // code still sees only the names it imported; the typechecker's
    // is_export_blocked path keeps unselected names off-limits at
    // qualified-call sites. This pass only ensures the symbol exists
    // in the merged AST so the cloned caller can link.
    int progressed;
    do {
        progressed = 0;
        for (int i = 0; i < orig_count; i++) {
            ASTNode* child = program->children[i];
            if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;

            // Only selective imports trigger transitive pull-in. Non-
            // selective imports already merged everything (they have no
            // selection list so the `if (!selected) continue;` guard
            // above never fired).
            int has_selection = (child->child_count > 0 &&
                                child->children[0]->type == AST_IDENTIFIER);
            if (!has_selection) continue;

            AetherModule* mod = module_find(child->value);
            if (!mod || !mod->ast) continue;
            ASTNode* mod_ast = mod->ast;
            const char* ns = module_get_namespace(child->value);

            const char* mod_func_names[AETHER_MODULE_MAX_DECLS];
            int mod_func_count = collect_module_func_names(mod_ast, mod_func_names,
                                                           AETHER_MODULE_MAX_DECLS);
            const char* mod_const_names[AETHER_MODULE_MAX_DECLS];
            int mod_const_count = collect_module_const_names(mod_ast, mod_const_names,
                                                             AETHER_MODULE_MAX_DECLS);

            // Collect bare names of intra-module callees referenced from
            // any function already merged from this module.
            const char* needed[AETHER_MODULE_MAX_DECLS];
            int needed_count = 0;
            for (int m = 0; m < program->child_count; m++) {
                ASTNode* top = program->children[m];
                if (!top || !top->is_imported) continue;
                if (top->type != AST_FUNCTION_DEFINITION &&
                    top->type != AST_BUILDER_FUNCTION) continue;
                if (!top->value) continue;
                size_t ns_len = strlen(ns);
                if (strncmp(top->value, ns, ns_len) != 0 || top->value[ns_len] != '_') continue;
                collect_intra_module_callees(top, ns, mod_func_names, mod_func_count,
                                             needed, &needed_count,
                                             AETHER_MODULE_MAX_DECLS);
            }

            for (int n = 0; n < needed_count; n++) {
                const char* bare = needed[n];
                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, bare);
                if (program_has_function(program, prefixed)) continue;
                if (module_has_extern_named(mod_ast, prefixed)) continue;

                // Locate the helper in the source module and clone it.
                for (int j = 0; j < mod_ast->child_count; j++) {
                    ASTNode* decl = unwrap_export(mod_ast->children[j]);
                    if (!decl || !decl->value) continue;
                    if ((decl->type != AST_FUNCTION_DEFINITION &&
                         decl->type != AST_BUILDER_FUNCTION)) continue;
                    if (strcmp(decl->value, bare) != 0) continue;

                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(prefixed);
                    clone->is_imported = 1;
                    rename_intra_module_refs(clone, ns, mod_func_names, mod_func_count,
                                             mod_const_names, mod_const_count, NULL, 0);
                    apply_inherited_selective_imports(clone, mod_ast);
                    insert_child_at(program, clone, insert_idx++);
                    progressed = 1;
                    break;
                }
            }
        }
    } while (progressed);

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
        // Collect direct imports as the BFS frontier.
        const char* visited[256];
        // #1097: was this module reached as a *transitive dependency* of some
        // other module (as opposed to only being a direct top-level import)?
        // A direct import merged only its selected subset in the main loop, so
        // a module that is ALSO transitively needed must have its remaining
        // exports merged here — otherwise a wrapper the top-level import
        // omitted (`poll2`) but a library uses stays un-instantiated and its
        // call site degrades to an undefined `<ns>_<name>`.
        int reached_transitively[256];
        int visited_count = 0;
        const char* queue[256];
        int q_head = 0, q_tail = 0;

        for (int i = 0; i < orig_count; i++) {
            ASTNode* child = program->children[i];
            if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
            if (visited_count >= 256) break;
            reached_transitively[visited_count] = 0;
            visited[visited_count++] = child->value;
            if (q_tail < 256) queue[q_tail++] = child->value;
        }

        // BFS: for each enqueued module, look at its `imports` list and
        // enqueue any not yet visited. A dep discovered here is marked
        // reached-transitively even if it was already seeded as a direct
        // import (#1097) — that's the union signal the merge below needs.
        while (q_head < q_tail) {
            const char* mod_path = queue[q_head++];
            AetherModule* mod = module_find(mod_path);
            if (!mod) continue;

            for (int i = 0; i < mod->import_count; i++) {
                const char* dep = mod->imports[i];
                if (!dep) continue;
                int seen = 0;
                for (int v = 0; v < visited_count; v++) {
                    if (strcmp(visited[v], dep) == 0) {
                        // Already visited — but now we know it's also a
                        // transitive dep, so record that (a direct selective
                        // import seeded it with the flag clear). #1097.
                        reached_transitively[v] = 1;
                        seen = 1;
                        break;
                    }
                }
                if (seen) continue;
                if (visited_count >= 256) break;
                reached_transitively[visited_count] = 1;
                visited[visited_count++] = dep;
                if (q_tail < 256) queue[q_tail++] = dep;
            }
        }

        // For each transitively-reachable module, merge its exported functions
        // and constants the same way the main loop merged direct imports.
        for (int v = 0; v < visited_count; v++) {
            const char* dep_path = visited[v];

            // Skip modules that are *only* a direct user import — the main
            // loop already merged them (its selective filter is the intended
            // user-facing scope). But a direct import that is ALSO a
            // transitive dependency of another merged module must fall
            // through: the main loop merged only its selected subset, and the
            // library needs the rest. The clone dedup guards below
            // (program_has_function / _const / _struct) make re-merging the
            // already-cloned subset a no-op, so this only adds the missing
            // transitively-used exports. #1097.
            int is_direct = 0;
            for (int i = 0; i < orig_count; i++) {
                ASTNode* child = program->children[i];
                if (child && child->type == AST_IMPORT_STATEMENT &&
                    child->value && strcmp(child->value, dep_path) == 0) {
                    is_direct = 1;
                    break;
                }
            }
            if (is_direct && !reached_transitively[v]) continue;

            AetherModule* dep_mod = module_find(dep_path);
            if (!dep_mod || !dep_mod->ast) continue;

            ASTNode* mod_ast = dep_mod->ast;
            const char* ns = module_get_namespace(dep_path);

            const char* func_names[AETHER_MODULE_MAX_DECLS];
            int func_count = collect_module_func_names(mod_ast, func_names,
                                                       AETHER_MODULE_MAX_DECLS);
            const char* const_names[AETHER_MODULE_MAX_DECLS];
            int const_count = collect_module_const_names(mod_ast, const_names,
                                                         AETHER_MODULE_MAX_DECLS);

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
            // #1097: a direct-but-also-transitive dep already carries a
            // user-written import for this path, so the namespace is already
            // registered — don't add a redundant synthetic node for it. Only
            // inject the synthetic import when the program has no import of
            // this path yet (the pure-transitive case #243 targeted).
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

            for (int j = 0; j < mod_ast->child_count; j++) {
                ASTNode* decl = unwrap_export(mod_ast->children[j]);
                if (!decl || !decl->value) continue;

                if (decl->type == AST_FUNCTION_DEFINITION ||
                    decl->type == AST_BUILDER_FUNCTION) {
                    char prefixed[256];
                    snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, decl->value);

                    if (module_has_extern_named(mod_ast, prefixed)) continue;
                    if (program_has_function(program, prefixed)) continue;

                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(prefixed);
                    clone->is_imported = 1;

                    rename_intra_module_refs(clone, ns, func_names, func_count,
                                             const_names, const_count, NULL, 0);
                    apply_inherited_selective_imports(clone, mod_ast);

                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_CONST_DECLARATION) {
                    char prefixed[256];
                    snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, decl->value);

                    // Dedup against existing CONSTS (not functions — the old
                    // program_has_function guard here never matched a const, so
                    // a revisited module re-cloned it: aether#1009 exposed this
                    // as `redefinition of 'sha2_K256'`).
                    if (program_has_const(program, prefixed)) continue;

                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(prefixed);

                    rename_intra_module_refs(clone, ns, func_names, func_count,
                                             const_names, const_count, NULL, 0);

                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_STRUCT_DEFINITION) {
                    // Same shape as the direct-import struct merge above,
                    // for transitively-reachable modules pulled in via the
                    // BFS. A merged function body that casts to `*T` needs
                    // T in scope regardless of which import edge brought
                    // the function in.
                    if (program_has_struct(program, decl->value)) continue;

                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_DISTINCT_TYPE_DEF && decl->value) {
                    // #908: transitive sibling of the distinct-def merge above.
                    if (program_has_distinct(program, decl->value)) continue;
                    ASTNode* clone = clone_ast_node(decl);
                    clone->is_imported = 1;
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
                    insert_child_at(program, clone, insert_idx++);
                }
            }
        }
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
        if (import_node->child_count == 0 ||
            import_node->children[0]->type != AST_IDENTIFIER) continue;

        const char* module_path = import_node->value;
        AetherModule* mod = module_find(module_path);
        if (!mod || !mod->ast) continue;
        const char* ns = module_get_namespace(module_path);

        const char* sel_func_names[AETHER_MODULE_MAX_DECLS];
        int sel_func_count = 0;
        const char* sel_const_names[AETHER_MODULE_MAX_DECLS];
        int sel_const_count = 0;

        const char* mod_func_names[AETHER_MODULE_MAX_DECLS];
        int mod_func_count = collect_module_func_names(mod->ast, mod_func_names,
                                                       AETHER_MODULE_MAX_DECLS);
        const char* mod_const_names[AETHER_MODULE_MAX_DECLS];
        int mod_const_count = collect_module_const_names(mod->ast, mod_const_names,
                                                         AETHER_MODULE_MAX_DECLS);

        const char* alias_from[AETHER_MODULE_MAX_DECLS];
        const char* alias_to_name[AETHER_MODULE_MAX_DECLS];
        int alias_is_const[AETHER_MODULE_MAX_DECLS];
        int alias_count = 0;

        for (int k = 0; k < import_node->child_count; k++) {
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
                if (alias_count < AETHER_MODULE_MAX_DECLS &&
                    (name_in_list(sel->value, mod_func_names, mod_func_count) ||
                     name_in_list(sel->value, mod_const_names, mod_const_count))) {
                    alias_from[alias_count] = sel->annotation + 13;
                    alias_to_name[alias_count] = sel->value;
                    alias_is_const[alias_count] =
                        name_in_list(sel->value, mod_const_names, mod_const_count);
                    alias_count++;
                }
                continue;
            }
            if (name_in_list(sel->value, mod_func_names, mod_func_count) &&
                sel_func_count < AETHER_MODULE_MAX_DECLS) {
                sel_func_names[sel_func_count++] = sel->value;
            } else if (name_in_list(sel->value, mod_const_names, mod_const_count) &&
                       sel_const_count < AETHER_MODULE_MAX_DECLS) {
                sel_const_names[sel_const_count++] = sel->value;
            }
        }
        if (sel_func_count == 0 && sel_const_count == 0 && alias_count == 0) continue;

        for (int m = 0; m < program->child_count; m++) {
            ASTNode* top = program->children[m];
            if (!top) continue;
            if (top->type == AST_IMPORT_STATEMENT) continue;
            if (top->is_imported) continue;
            rename_intra_module_refs(top, ns, sel_func_names, sel_func_count,
                                     sel_const_names, sel_const_count, NULL, 0);
            for (int a = 0; a < alias_count; a++) {
                char prefixed[256];
                snprintf(prefixed, sizeof(prefixed), "%s_%s", ns, alias_to_name[a]);
                rename_aliased_import_refs(top, alias_from[a], prefixed,
                                           alias_is_const[a]);
            }
        }
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
            char prefixed[256];
            snprintf(prefixed, sizeof(prefixed), "%s_%s", ons, sym);
            if (program_has_function(program, prefixed)) continue;

            const char* of_names[AETHER_MODULE_MAX_DECLS];
            int of_count = collect_module_func_names(origin->ast, of_names,
                                                     AETHER_MODULE_MAX_DECLS);
            const char* oc_names[AETHER_MODULE_MAX_DECLS];
            int oc_count = collect_module_const_names(origin->ast, oc_names,
                                                      AETHER_MODULE_MAX_DECLS);

            for (int j = 0; j < origin->ast->child_count; j++) {
                ASTNode* decl = unwrap_export(origin->ast->children[j]);
                if (!decl || !decl->value || strcmp(decl->value, sym) != 0) continue;

                if (decl->type == AST_FUNCTION_DEFINITION ||
                    decl->type == AST_BUILDER_FUNCTION) {
                    if (module_has_extern_named(origin->ast, prefixed)) break;
                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(prefixed);
                    clone->is_imported = 1;
                    rename_intra_module_refs(clone, ons, of_names, of_count,
                                             oc_names, oc_count, NULL, 0);
                    apply_inherited_selective_imports(clone, origin->ast);
                    insert_child_at(program, clone, insert_idx++);
                } else if (decl->type == AST_CONST_DECLARATION) {
                    // The block-level guard above checks functions only; a
                    // const needs its own dedup or a revisit re-clones it.
                    if (program_has_const(program, prefixed)) break;
                    ASTNode* clone = clone_ast_node(decl);
                    free(clone->value);
                    clone->value = strdup(prefixed);
                    rename_intra_module_refs(clone, ons, of_names, of_count,
                                             oc_names, oc_count, NULL, 0);
                    insert_child_at(program, clone, insert_idx++);
                }
                break;
            }
        }
    }
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
// at the AST level skips that wasted gcc work entirely AND skips the
// equivalent typecheck work, since the typechecker walks every top-
// level decl in the program AST.
//
// Algorithm: classic mark-and-sweep over the call graph.
//   1. Seed the reachable set from main + actor handlers + exports +
//      every non-imported (user-written) function and builder.
//   2. Walk each seed, collecting AST_FUNCTION_CALL targets and bare
//      AST_IDENTIFIER references that name a top-level function.
//   3. For each newly-discovered name, find its definition in the
//      program AST and walk it the same way. Repeat until fixed point.
//   4. Sweep: drop any AST_FUNCTION_DEFINITION / AST_BUILDER_FUNCTION
//      whose `is_imported` flag is set and whose name is NOT in the
//      reachable set. Constants stay: they're cheap, and pruning them
//      would need an additional reachability pass keyed on identifier
//      references, which isn't worth it for the size of a constant.
//
// Qualified call sites carry dotted names (`os.argv0`); codegen rewrites
// dot-to-underscore at emission. We normalise the same way when adding
// to the set so it matches the prefixed function-definition names that
// `module_merge_into_program` produced (`os_argv0`).
// ----------------------------------------------------------------------------

typedef struct {
    char** names;
    int count;
    int capacity;
} NameSet;

static int nameset_contains(const NameSet* s, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) return 1;
    }
    return 0;
}

static int nameset_add(NameSet* s, const char* name) {
    if (!name) return 0;
    char buf[256];
    const char* key = name;
    if (strchr(name, '.')) {
        size_t n = strlen(name);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        for (size_t i = 0; i < n; i++) {
            char c = name[i];
            buf[i] = (c == '.') ? '_' : c;
        }
        buf[n] = '\0';
        key = buf;
    }
    if (nameset_contains(s, key)) return 0;
    if (s->count >= s->capacity) {
        int new_cap = s->capacity ? s->capacity * 2 : 64;
        char** nn = realloc(s->names, sizeof(char*) * new_cap);
        if (!nn) return 0;
        s->names = nn;
        s->capacity = new_cap;
    }
    s->names[s->count++] = strdup(key);
    return 1;
}

static void nameset_free(NameSet* s) {
    for (int i = 0; i < s->count; i++) free(s->names[i]);
    free(s->names);
    s->names = NULL;
    s->count = s->capacity = 0;
}

/* #934 UFCS reachability: a `recv.method(...)` UFCS call is rewritten (at
 * typecheck, AFTER this prune) to an imported `mod.method(recv, ...)` when a
 * module exports a `method` whose first param matches typeof(recv). At prune
 * time the receiver type isn't known, so over-approximate: for a bare method
 * name, seed `mod.method` for EVERY module that exports it. Sound (keeps a
 * few extra bodies); the typechecker still resolves the single real target. */
static void prune_seed_ufcs_method(const char* method, NameSet* seen,
                                   NameSet* worklist) {
    if (!method || !global_module_registry) return;
    for (int mi = 0; mi < global_module_registry->module_count; mi++) {
        AetherModule* m = global_module_registry->modules[mi];
        if (!m || !m->name) continue;
        if (!module_is_exported(m, method)) continue;
        char q[256];
        snprintf(q, sizeof(q), "%s.%s", m->name, method);
        if (nameset_add(seen, q)) nameset_add(worklist, q);
    }
}

static void prune_collect_calls(ASTNode* node, NameSet* seen, NameSet* worklist) {
    if (!node) return;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        if (nameset_add(seen, node->value)) {
            nameset_add(worklist, node->value);
        }
        /* #924 re-export: a `hub.fn(...)` call is cloned under the DEFINING
         * module's name (`<origin>_fn`). Seed the origin-qualified form so
         * the reachability sweep doesn't prune the re-exported body. */
        char* dot = strchr(node->value, '.');
        if (dot && dot != node->value && !strchr(dot + 1, '.')) {
            char hubname[256];
            size_t hl = (size_t)(dot - node->value);
            if (hl < sizeof(hubname)) {
                memcpy(hubname, node->value, hl);
                hubname[hl] = '\0';
                AetherModule* hub = module_find(hubname);
                AetherModule* origin = module_resolve_reexport(hub, dot + 1);
                if (origin && origin->name) {
                    char oq[256];
                    snprintf(oq, sizeof(oq), "%s.%s", origin->name, dot + 1);
                    if (nameset_add(seen, oq)) nameset_add(worklist, oq);
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
    if (node->type == AST_IDENTIFIER && node->value) {
        if (nameset_add(seen, node->value)) {
            nameset_add(worklist, node->value);
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
        char qualified[256];
        snprintf(qualified, sizeof(qualified), "%s.%s",
                 node->children[0]->value, node->value);
        if (nameset_add(seen, qualified)) {
            nameset_add(worklist, qualified);
        }
        /* #924 re-export: `hub.X` is cloned under the DEFINING module's
         * name (`<origin>_X`), not `hub_X`. Seed the origin form too so the
         * reachability sweep keeps the re-exported definition. */
        AetherModule* hub = module_find(node->children[0]->value);
        AetherModule* origin = module_resolve_reexport(hub, node->value);
        if (origin && origin->name) {
            char oq[256];
            snprintf(oq, sizeof(oq), "%s.%s", origin->name, node->value);
            if (nameset_add(seen, oq)) nameset_add(worklist, oq);
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
            nameset_add(worklist, node->annotation);
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        prune_collect_calls(node->children[i], seen, worklist);
    }
}

static ASTNode* prune_find_function(ASTNode* program, const char* name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c || !c->value) continue;
        if ((c->type == AST_FUNCTION_DEFINITION || c->type == AST_BUILDER_FUNCTION) &&
            strcmp(c->value, name) == 0) {
            return c;
        }
    }
    return NULL;
}

void module_prune_unreachable(ASTNode* program) {
    if (!program) return;

    NameSet reachable = {0};
    NameSet worklist = {0};

    // Seed: main, non-imported user functions, actor decls, exports.
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        if (!c) continue;
        switch (c->type) {
            case AST_MAIN_FUNCTION:
            case AST_ACTOR_DEFINITION:
            case AST_EXPORT_STATEMENT:
                prune_collect_calls(c, &reachable, &worklist);
                break;
            case AST_FUNCTION_DEFINITION:
            case AST_BUILDER_FUNCTION:
                if (!c->is_imported) {
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
        ASTNode* fn = prune_find_function(program, name);
        if (fn) prune_collect_calls(fn, &reachable, &worklist);

        size_t name_len = strlen(name);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!c || !c->value || !c->is_imported) continue;
            if (c->type != AST_FUNCTION_DEFINITION && c->type != AST_BUILDER_FUNCTION) continue;
            size_t cv_len = strlen(c->value);
            if (cv_len > name_len + 1 &&
                c->value[cv_len - name_len - 1] == '_' &&
                strcmp(c->value + cv_len - name_len, name) == 0) {
                if (nameset_add(&reachable, c->value)) {
                    nameset_add(&worklist, c->value);
                }
            }
        }
        free(name);
    }
    free(worklist.names);

    // Sweep: drop imported functions/builders that the closure never
    // reached. Compaction is in-place; freeing the dead AST sub-trees
    // releases the memory the typechecker would otherwise traverse.
    int kept = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        int drop = 0;
        if (c && c->is_imported && c->value &&
            (c->type == AST_FUNCTION_DEFINITION || c->type == AST_BUILDER_FUNCTION) &&
            !nameset_contains(&reachable, c->value)) {
            drop = 1;
        }
        if (drop) {
            free_ast_node(c);
        } else {
            program->children[kept++] = c;
        }
    }
    program->child_count = kept;

    nameset_free(&reachable);
}

