#ifndef AETHER_MODULE_H
#define AETHER_MODULE_H

#include "ast.h"
#include "aether_lib_path.h"  /* AETHER_LIB_DIRS_MAX, AETHER_LIB_PATH_SEP_CHAR */

// Module system for Aether
// Supports: import/export, package management

// Module structure
typedef struct {
    char* name;           // Module name (e.g., "game.player")
    char* file_path;      // Path to source file
    ASTNode* ast;         // Parsed AST
    char** exports;       // Exported symbols
    int export_count;
    char** imports;       // Imported modules
    int import_count;
    // #2209: the namespace this module's merged symbols are prefixed with
    // (`<ns>_<name>`). The last path segment when no other loaded module
    // shares it, else the full path with dots as underscores, so two
    // modules ending in `vk` never share one namespace. A shipped module
    // (`std.*`, `contrib.*`) always keeps its last segment: its C symbols
    // are compiled under that prefix and cannot move (#2190). Assigned by
    // module_assign_namespaces once every module is loaded; NULL before.
    char* ns;
} AetherModule;

// Module registry
typedef struct {
    AetherModule** modules;
    int module_count;
    int module_capacity;
    char source_dir[2048];  // Source file directory for relative resolution
    // Lib-search path: an ordered list of directories (PATH-style),
    // searched left-to-right; first hit wins. Issue #413. Default is
    // a single entry, `"lib"`, populated at registry init. Each entry
    // is a fixed 256-byte buffer; the count tracks how many slots
    // are live.
    char lib_dirs[AETHER_LIB_DIRS_MAX][256];
    int  lib_dir_count;
} ModuleRegistry;

// Global module registry
extern ModuleRegistry* global_module_registry;

// Module management
void module_registry_init(void);
void module_registry_shutdown(void);

AetherModule* module_create(const char* name, const char* file_path);
void module_free(AetherModule* module);

// Module registration
void module_register(AetherModule* module);
AetherModule* module_find(const char* name);

/* #2209: per-module namespaces. `module_assign_namespaces` gives every
 * registered module its `ns` (see AetherModule.ns); module_orchestrate
 * calls it once all imports are loaded. `module_namespace_of` is the
 * namespace a module path's merged symbols carry: the assigned `ns` for a
 * registered module, the last path segment for anything else (the
 * pre-#2209 rule, so an unregistered path still prefixes as before).
 * `module_find_by_namespace` finds a module by its full name, its `ns`, or
 * — last — its bare last segment (the first such match), which is what a
 * qualified `ns.name` use carries. */
void module_assign_namespaces(void);
const char* module_namespace_of(const char* module_path);
AetherModule* module_find_by_namespace(const char* ns);

// Import/export handling
void module_add_export(AetherModule* module, const char* symbol);
void module_add_import(AetherModule* module, const char* module_name);
int module_is_exported(AetherModule* module, const char* symbol);
/* #2172: exported as `name` or as the prefixed `<leaf>_name` a qualified
 * call resolves to. What export enforcement asks. */
int module_exports_symbol(AetherModule* module, const char* name);
/* #924: if `module` re-exports `symbol` (lists it in `exports` but imports
 * it from elsewhere), return the module that actually defines it (resolved
 * transitively). NULL when `module` itself is the definer. */
AetherModule* module_resolve_reexport(AetherModule* module, const char* symbol);

// Dependency graph
typedef struct DependencyNode {
    char* module_name;
    struct DependencyNode** dependencies;
    int dependency_count;
    int visited;  // For circular detection
    int in_stack; // For circular detection
} DependencyNode;

typedef struct {
    DependencyNode** nodes;
    int node_count;
} DependencyGraph;

DependencyGraph* dependency_graph_create(void);
void dependency_graph_free(DependencyGraph* graph);
DependencyNode* dependency_graph_add_node(DependencyGraph* graph, const char* module_name);
void dependency_graph_add_edge(DependencyGraph* graph, const char* from, const char* to);
int dependency_graph_has_cycle(DependencyGraph* graph);
DependencyNode* dependency_graph_find_node(DependencyGraph* graph, const char* module_name);

// Package manifest (aether.toml)
typedef struct {
    char* package_name;
    char* version;
    char* author;
    char** dependencies;
    int dependency_count;
} PackageManifest;

PackageManifest* package_manifest_load(const char* path);
void package_manifest_free(PackageManifest* manifest);

// Module orchestration — call between parsing and type checking
// Set the source file directory so module resolution can search lib/ relative to it.
void module_set_source_dir(const char* source_path);

// Set the lib search path for module resolution. The argument may be
// a single directory (`"lib"`, `".aeb"`) or a PATH-style list (e.g.
// `"./lib:~/aether-libs"` on POSIX, `"./lib;C:/aether-libs"` on
// Windows). Each entry is appended to the search list in order; the
// list is RESET on every call so passing a single path overrides any
// existing entries. Use `module_add_lib_dir` to append without
// clearing. Default is the single entry `"lib"`. Issue #413.
void module_set_lib_dir(const char* lib_dir);

// Append a single directory to the lib search path. Used by `ae run
// --lib a --lib b` to compose paths from repeated flags. Silently
// no-ops on NULL or empty input and on overflow past
// AETHER_LIB_DIRS_MAX. Issue #413.
void module_add_lib_dir(const char* dir);

// Append a separator-string of directories ("a:b:c" POSIX,
// "a;b;c" Windows) to the lib search path. Each segment is routed
// through `module_add_lib_dir` so normalisation, dedup, and the
// cap behave identically to repeated flags. Single source of truth
// for separator parsing — both `module_set_lib_dir` and aetherc's
// `--lib` handler call this. Issue #413.
void module_add_lib_dirs(const char* spec);

// Orchestrate all module loading: scan imports, resolve, parse, cache, detect cycles.
// Returns 1 on success, 0 on circular dependency error.
int module_orchestrate(ASTNode* program);

// Parse a single module file into an AST. Saves/restores lexer state.
ASTNode* module_parse_file(const char* file_path);

/* #1746: parses the module at `source_path` and encodes it as a compiled
 * module artifact recording `source_rel` (its install-relative path, e.g.
 * std/cryptography/md2/module.ae). Returns the malloc'd artifact bytes, or
 * NULL with *err set when the module does not parse cleanly. */
char* module_build_artifact(const char* source_path, const char* source_rel,
                            size_t* out_len, const char** err);

// --- Dependency recording (issue #1882, depfile cache key) --------------------
// The resolver records every path it PROBES (whether the file was there or not)
// and every file it PARSES, so `aetherc --emit-deps` can write an exact
// invalidation manifest. The cache key on a warm run hashes that manifest
// instead of walking whole directory trees. Recording the MISSES matters: a
// module dropped in at a path an earlier resolver `Try` probed-and-missed
// shadows the one that resolved, and only a recorded miss makes that
// insertion bust the cache (Nic's requirement — see
// docs/notes/import-closure-cache-key.md).
//
// Off by default; module_dep_recording_enable() turns it on for a build that
// asked for --emit-deps. module_probe() is the wrapper the resolver's access()
// checks go through: it records `path` (found or not) and returns 1 if present.
void module_dep_recording_enable(void);
int  module_probe(const char* path);            // access(F_OK)==0, and record
void module_dep_record_read(const char* path);  // a file whose CONTENTS matter
// Write the manifest: two sections, "read <path>" (content-hashed on reuse) and
// "absent <path>" (a newly-present file busts the cache). Returns 0 on success.
int  module_dep_write(const char* out_path);

// #2125 `@source("lanes.c")`: the path of one directive, resolved against the
// directory of the .ae file it was written in (an absolute path is kept as
// written). Returns malloc'd memory; caller frees.
char* module_resolve_source_directive(ASTNode* directive);
// Every top-level @source of one AST: the file must exist (reported at the
// directive, since the C compiler's "No such file" would name neither the
// module nor the line) and is recorded as a `read` dependency, so a warm
// cached build is invalidated when the C changes. Returns 1 when all resolve.
int module_check_source_directives(ASTNode* ast);

// Resolve module name to file path. Returns malloc'd path or NULL. Caller frees.
char* module_resolve_stdlib_path(const char* module_name);  // "fs" -> path
char* module_resolve_contrib_path(const char* module_name); // "sqlite" -> path
char* module_resolve_local_path(const char* module_path);   // "mypackage.utils" -> path

// Merge pure Aether module functions into the main program AST.
// Call after module_orchestrate() and before typecheck_program().
void module_merge_into_program(ASTNode* program);

// Tree-shake the merged program AST: remove imported function and
// builder definitions that no user-reachable code transitively calls.
// Roots: main(), actor handlers, exports, and any non-imported user
// function/builder. Closure: every function-call target named anywhere
// in those roots' bodies, recursively through merged code.
//
// Run AFTER module_merge_into_program (so the closure can see merged
// helpers) and BEFORE typecheck_program (so the typechecker doesn't
// burn time walking dead bodies). Reduces both aetherc typecheck time
// and the size of the C output gcc has to compile, on programs that
// only use a slice of large stdlib modules.
void module_prune_unreachable(ASTNode* program);

#endif // AETHER_MODULE_H

