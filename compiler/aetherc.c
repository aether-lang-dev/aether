/*
 * Aether Programming Language Compiler
 * Copyright (c) 2025 Aether Programming Language Contributors
 * 
 * This file is part of Aether.
 * Licensed under the MIT License. See LICENSE file in the project root.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#  if defined(_MSC_VER) && !defined(getpid)
#    define getpid _getpid
#  endif
#else
#include <unistd.h>
#endif
#include "parser/tokens.h"
#include "ast.h"
#include "parser/parser.h"
#include "analysis/typechecker.h"
#include "analysis/derive.h"
#include "codegen/optimizer.h"
#include "codegen/codegen.h"
#include "aether_defines.h"
#include "aether_error.h"
#include "aether_module.h"
#include "../lsp/aether_lsp.h"

// Compiler limits
#define MAX_TOKENS 50000

// Version is set by Makefile from VERSION file
#ifndef AETHER_VERSION
#define AETHER_VERSION "0.0.0-dev"
#endif

// Constants for better maintainability
#define DEFAULT_MAX_ACTORS 1000
#define DEFAULT_WORKER_THREADS 4

// Global flags
static bool verbose_mode = false;
static bool dump_ast_mode = false;
static bool emit_c_mode = false;
static const char* csrc_header_path = NULL;  // #996 --emit-header=<path>
static const char* csrc_catalog_path = NULL; // #996 --emit-catalog-json=<path>
static bool check_only_mode = false;
static bool preempt_mode = false;
// Issue #348 — suppress runtime emission of `requires` / `ensures`
// contract checks. Default is OFF (contracts checked at runtime).
// `--no-contracts` is the analog of C's `-DNDEBUG`: zero per-call
// cost, intended for release builds where the contracts have been
// validated upstream.
static bool no_contracts_mode = false;
static const char* emit_header_path = NULL;

// --emit=<exe|lib|both> — which artifact(s) to produce.
// exe (default): emit `int main(int, char**)`, no aether_* alias stubs.
// lib          : omit main(), emit aether_<name> alias stubs for every top-level
//                Aether function, and refuse to link capability-heavy stdlib modules.
// both         : emit both — executable and library symbols live in one .c file.
static bool emit_exe = true;
static bool emit_lib = false;

// --emit-main=<func> — with --emit=lib, emit a thin main(argc,argv)
// shim that calls the named Aether function. Closes the exe/lib
// symmetry: one .c ships as both a loadable lib AND a binary. Issue
// #268.3. NULL when not set; the codegen ignores the shim when the
// flag is absent or when --emit=lib isn't active.
static const char* emit_main_target = NULL;

// --with=<capability>[,<capability>...] — capability opt-ins for
// --emit=lib. Default is capability-empty (every capability-gated
// stdlib import is rejected); a project that IS the host — linking
// the emitted .c into its own binary rather than embedding it as a
// user script — opts into the subset it needs with a comma-separated
// list. Flag is a no-op without --emit=lib.
static bool with_fs = false;
static bool with_net = false;
static bool with_os = false;

// --emit-namespace-manifest: walk a manifest.ae's AST, extract the
// namespace/input/event/bindings calls, and write a JSON description
// to stdout. Used by `ae build --namespace <dir>` to learn about the
// namespace before codegen so it can synthesize the discovery struct.
// Implies --check (no codegen output).
static bool emit_namespace_manifest = false;

// --emit-namespace-describe: like --emit-namespace-manifest but writes
// a C source file containing a static const AetherNamespaceManifest
// and the aether_describe() definition. The C file is then linked
// into the namespace .so so the host can call aether_describe() at
// runtime. Output path is the second positional arg (the input is
// the manifest.ae).
static bool emit_namespace_describe = false;

// --list-functions: walk the AST, print one line per top-level Aether
// function definition: `<name>|<return_type>|<param_name>:<param_type>,...`
// Used by the namespace pipeline's per-language SDK generators to learn
// what aether_<name>() exports the .so will contain.
static bool list_functions_mode = false;

// --diagnose=ownership: print the per-function string-ownership
// verdicts the codegen would use at the wrapper terminator
// (codegen_stmt.c:1611-1631) — without running codegen. Helps
// downstreams localise the "heap-string handed to a collection then
// variable reassigned" UAF shape that the heap-string-tracker fix
// can unmask.
static bool diagnose_ownership_mode = false;

// --audit-mem: print every raw std.mem offset access (mem.get_*/set_*)
// with the byte width its accessor name implies, so a port author can
// check each read/write width against the C field's actual type. The
// dominant idiom for opaque C structs in large ports is hand-probed
// offset access (`mem.get_long(p, OFF)`), where the read width is chosen
// by the function name, not the field type — a single wrong choice reads
// adjacent bytes (issue #868: a uint32 field read with get_long produced
// a garbage 94 TB length and crashed). The width-exact accessors already
// exist; this surfaces the call sites so the choice is auditable. Off by
// default — opt-in, report-and-exit (no codegen). Issue #868 option 1.
static bool audit_mem_mode = false;

// Byte width a std.mem raw-offset accessor reads/writes, keyed by the
// name after "mem.". Returns 0 for non-width-bearing ops (copy / move /
// compare / set_bulk / ptr<->long) and unknown names.
static int mem_accessor_width(const char* fn) {
    static const struct { const char* name; int width; } t[] = {
        {"get_byte",1},{"set_byte",1},{"get_byte_sz",1},{"set_byte_sz",1},
        {"get_int8",1},{"set_int8",1},{"get_uint8",1},{"set_uint8",1},
        {"get_int16",2},{"set_int16",2},{"get_uint16",2},{"set_uint16",2},
        {"get_u16_le",2},{"get_u16_be",2},{"set_u16_le",2},{"set_u16_be",2},
        {"get_int",4},{"set_int",4},{"get_uint32",4},{"set_uint32",4},
        {"get_u32_le",4},{"get_u32_be",4},{"set_u32_le",4},{"set_u32_be",4},
        {"get_float32",4},{"set_float32",4},
        {"get_long",8},{"set_long",8},{"get_u64_le",8},{"get_u64_be",8},
        {"set_u64_le",8},{"set_u64_be",8},{"get_float64",8},{"set_float64",8},
        {"get_ptr",8},{"set_ptr",8},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strcmp(fn, t[i].name) == 0) return t[i].width;
    return 0;
}

// Recursively report every raw std.mem offset access under `node`.
// Returns the number of accesses reported.
static int audit_mem_walk(ASTNode* node, FILE* out) {
    if (!node) return 0;
    int n = 0;
    /* The audit runs post-typecheck, where a qualified `mem.get_long`
     * call has been resolved to its namespace-prefixed binding
     * `mem_get_long` (the same rewrite --emit=ast observes). Match that
     * resolved form; also accept the as-written `mem.` form defensively. */
    if (node->type == AST_FUNCTION_CALL && node->value &&
        (strncmp(node->value, "mem_", 4) == 0 ||
         strncmp(node->value, "mem.", 4) == 0)) {
        const char* acc = node->value + 4;
        int w = mem_accessor_width(acc);
        if (w > 0) {
            const char* kind = (strncmp(acc, "set", 3) == 0)
                ? "writes" : "reads";
            const char* file = node->source_file ? node->source_file : "<input>";
            char offbuf[128];
            offbuf[0] = '\0';
            if (node->child_count >= 2 && node->children[1] &&
                (node->children[1]->type == AST_IDENTIFIER ||
                 node->children[1]->type == AST_LITERAL) &&
                node->children[1]->value) {
                snprintf(offbuf, sizeof(offbuf), " at offset %s",
                         node->children[1]->value);
            }
            fprintf(out,
                "%s:%d:%d: %s %s %d byte%s%s, verify the width matches the C field's type\n",
                file, node->line, node->column, node->value, kind,
                w, w == 1 ? "" : "s", offbuf);
            n++;
        }
    }
    for (int i = 0; i < node->child_count; i++)
        n += audit_mem_walk(node->children[i], out);
    return n;
}

// --emit=ast: post-typecheck AST emit as JSON to stdout. A third walker
// beside --list-functions and --diagnose=ownership, runs at the same
// post-typecheck / pre-codegen point so node->value carries the
// resolved (namespace-prefixed) binding rather than the as-written
// spelling (`os.system` → `os_system`, alias-rewrites resolved).
//
// Consumer use case: aeb's supply-chain veto walks the emitted JSON
// to apply operator-owned deny rules (`deny extern`, `deny exec`,
// `deny net`, `deny import`). The compiler itself contains zero
// veto policy — that lives in aeb. See aeb's lib/veto and
// veto-enhancements.md (cross-repo design note).
//
// Filter set (intentionally small, additive later):
//   - import_statement   (module, alias, selected[], glob)
//   - extern_function    (name, variadic)
//   - function_call      (callee resolved post-merge, or indirect:true)
//
// Exit codes are conventional (no inversion): 0 = emitted OK,
// non-zero = parse/typecheck/IO failure. See the post-typecheck
// short-circuit further down for the explicit return-0 path.
static bool emit_ast_mode = false;
// --emit=inspect: operator-facing summary of what a .ae declares
// (imports, capabilities, exports/entry, declarations) to stdout, then
// exit. No codegen. Drives `ae inspect` (issue #473).
static bool emit_inspect_mode = false;
// --emit=effects: derived per-function effect/purity analysis as JSON to
// stdout, then exit. No codegen. Peer of --emit=ast / inspect. Whole-program
// transitive capability reachability (fs/net/os) + purity per function,
// computed from the call graph (NOT author @no_* tags), fail-closed on
// externs. Consumed by external auditors (aeb's supply-chain veto). Issue #889.
static bool emit_effects_mode = false;

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define access _access
    #define F_OK 0
    #define PATH_SEP '\\'
#else
    #include <unistd.h>
    #define PATH_SEP '/'
#endif

// Helper to check file existence
int compiler_file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

// Derive header filename from output path (output.c -> output.h)
static char* derive_header_path(const char* output_path) {
    size_t len = strlen(output_path);
    char* header_path = malloc(len + 1);
    strcpy(header_path, output_path);

    // Replace .c with .h, or append .h if no .c extension
    if (len > 2 && header_path[len-2] == '.' && header_path[len-1] == 'c') {
        header_path[len-1] = 'h';
    } else {
        header_path = aether_xrealloc(header_path, len + 3);
        strcat(header_path, ".h");
    }
    return header_path;
}

// Print a summary line if any errors were recorded.
//
// Also surface the `ae help <script>` hint — the typer's per-error
// messages are precise but terse, the operator-friendly companion
// is one shell command away, and the user shouldn't have to read
// the docs to learn it exists. Suppressed when AETHER_NO_HELP_HINT
// is set so build pipelines that machine-parse stderr aren't
// surprised by the extra line.
static void report_compilation_failure(const char* input_path) {
    int n = aether_error_count();
    if (n <= 0) return;
    fprintf(stderr, "aborting: %d error(s) found\n", n);
    if (getenv("AETHER_NO_HELP_HINT")) return;
    if (input_path && input_path[0]) {
        fprintf(stderr,
                "hint: run `ae help %s` for actionable suggestions "
                "(Levenshtein matches, YAML/HCL→call-form, missing imports). "
                "Suppress with AETHER_NO_HELP_HINT=1.\n",
                input_path);
    }
}

// JSON helpers for --emit-namespace-manifest. Quote a string literal,
// handling backslash and double-quote. Newlines/tabs are unlikely in
// manifest fields but escape them for safety.
static void json_emit_str(FILE* out, const char* s) {
    if (!s) { fputs("null", out); return; }
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') { fputs("\\n", out); }
        else if (c == '\t') { fputs("\\t", out); }
        else if (c < 0x20) { fprintf(out, "\\u%04x", c); }
        else { fputc(c, out); }
    }
    fputc('"', out);
}

// Captured manifest used by both the JSON emitter and the C-describe
// emitter. Borrowed pointers into the parsed AST's literal storage —
// valid for the lifetime of the parse.
typedef struct {
    const char* ns_name;
    const char* java_pkg;
    const char* java_class;
    const char* py_module;
    const char* rb_module;
    const char* go_package;
    struct { const char* name; const char* type_sig; } inputs[64];
    int input_count;
    struct { const char* name; const char* carries; } events[64];
    int event_count;
} ExtractedManifest;

// Walk a parsed program AST and capture every top-level manifest
// builder call (namespace/input/event/bindings/java/python/go) found
// in the body of any top-level function. The function ordering inside
// the AST preserves the user's declaration order — inputs and events
// appear in the generated SDK in the order the manifest declared them.
//
// We deliberately do NOT execute the manifest at compile time; we
// inspect the AST. That sidesteps the need to dlopen the just-built
// library inside `aetherc` and matches how documented contract
// generators (gRPC's protoc-gen-*, openapi-generator, etc.) work:
// description is structural, not behavioural.
/* Recursively walk an AST node looking for manifest-builder calls.
 * Handles both the flat form (`setup() { describe("x") input(...) }`)
 * and the nested form (`abi() { describe("x") { input(...) bindings()
 * { java(...) } } }`).
 *
 * In the nested form, an `AST_FUNCTION_CALL` like `describe("trading")
 * { ... }` has the trailing block as a child node (typically the last
 * child, of type AST_CLOSURE with value "trailing", or AST_BLOCK).
 * We recurse into anything that looks like a child block. */
static void extract_manifest_walk(ExtractedManifest* m, ASTNode* node) {
    if (!node) return;

    /* If this is an AST_FUNCTION_CALL, capture builder-call args. */
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* name = node->value;
        #define ARG_STR(n) (node->child_count > (n) && node->children[(n)] \
                            && node->children[(n)]->type == AST_LITERAL \
                            ? node->children[(n)]->value : NULL)
        if (strcmp(name, "describe") == 0) {
            if (ARG_STR(0)) m->ns_name = ARG_STR(0);
        } else if (strcmp(name, "input") == 0) {
            if (m->input_count < 64 && ARG_STR(0) && ARG_STR(1)) {
                m->inputs[m->input_count].name = ARG_STR(0);
                m->inputs[m->input_count].type_sig = ARG_STR(1);
                m->input_count++;
            }
        } else if (strcmp(name, "event") == 0) {
            if (m->event_count < 64 && ARG_STR(0) && ARG_STR(1)) {
                m->events[m->event_count].name = ARG_STR(0);
                m->events[m->event_count].carries = ARG_STR(1);
                m->event_count++;
            }
        } else if (strcmp(name, "java") == 0) {
            if (ARG_STR(0)) m->java_pkg = ARG_STR(0);
            if (ARG_STR(1)) m->java_class = ARG_STR(1);
        } else if (strcmp(name, "python") == 0) {
            if (ARG_STR(0)) m->py_module = ARG_STR(0);
        } else if (strcmp(name, "ruby") == 0) {
            if (ARG_STR(0)) m->rb_module = ARG_STR(0);
        } else if (strcmp(name, "go") == 0) {
            if (ARG_STR(0)) m->go_package = ARG_STR(0);
        }
        #undef ARG_STR
    }

    /* Recurse into all children — picks up nested trailing blocks
     * (the trailing block of `describe("x") { ... }` is a child of
     * the call node) and the ordinary statements of any function body. */
    for (int i = 0; i < node->child_count; i++) {
        extract_manifest_walk(m, node->children[i]);
    }
}

static void extract_manifest(ExtractedManifest* m, ASTNode* program) {
    memset(m, 0, sizeof(*m));
    if (!program) return;

    /* Walk every top-level function (`abi`, `setup`, `main`, whatever).
     * Skip externs and the import statements themselves — we only
     * want to find calls inside function bodies. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (!fn || (fn->type != AST_FUNCTION_DEFINITION
                 && fn->type != AST_BUILDER_FUNCTION
                 && fn->type != AST_MAIN_FUNCTION)) continue;
        extract_manifest_walk(m, fn);
    }
}

static void emit_manifest_json(FILE* out, ASTNode* program) {
    if (!out) return;
    ExtractedManifest m;
    extract_manifest(&m, program);

    fputs("{\n  \"namespace\": ", out); json_emit_str(out, m.ns_name); fputs(",\n", out);

    fputs("  \"inputs\": [", out);
    for (int i = 0; i < m.input_count; i++) {
        if (i > 0) fputs(", ", out);
        fputs("{\"name\": ", out); json_emit_str(out, m.inputs[i].name);
        fputs(", \"type\": ",     out); json_emit_str(out, m.inputs[i].type_sig);
        fputs("}", out);
    }
    fputs("],\n", out);

    fputs("  \"events\": [", out);
    for (int i = 0; i < m.event_count; i++) {
        if (i > 0) fputs(", ", out);
        fputs("{\"name\": ",    out); json_emit_str(out, m.events[i].name);
        fputs(", \"carries\": ", out); json_emit_str(out, m.events[i].carries);
        fputs("}", out);
    }
    fputs("],\n", out);

    fputs("  \"bindings\": {\n", out);
    fputs("    \"java\": {\"package\": ", out); json_emit_str(out, m.java_pkg);
    fputs(", \"class\": ", out); json_emit_str(out, m.java_class); fputs("},\n", out);
    fputs("    \"python\": {\"module\": ", out); json_emit_str(out, m.py_module); fputs("},\n", out);
    fputs("    \"ruby\": {\"module\": ", out); json_emit_str(out, m.rb_module); fputs("},\n", out);
    fputs("    \"go\": {\"package\": ", out); json_emit_str(out, m.go_package); fputs("}\n", out);
    fputs("  }\n", out);
    fputs("}\n", out);
}

// C-quote a string for embedding in a static initializer. Same escape
// rules as JSON above but for C string literals.
static void c_emit_str(FILE* out, const char* s) {
    if (!s) { fputs("NULL", out); return; }
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') { fputs("\\n", out); }
        else if (c == '\t') { fputs("\\t", out); }
        else if (c < 0x20) { fprintf(out, "\\x%02x\"\"", c); }
        else { fputc(c, out); }
    }
    fputc('"', out);
}

// Walk a parsed program AST and emit one line per top-level function:
//   <name>|<return_aether_type>|<param_name>:<param_aether_type>,...
//
// Each parameter is `name:type`; multiple parameters comma-separated;
// no parameters → empty string after the second |. Return type is the
// Aether type spelling we infer from the function definition's
// node_type (or "void" if unset). Used by the namespace pipeline to
// generate per-language SDKs that map Aether function signatures to
// host-language method signatures.
//
// We skip functions whose name starts with `_` (treated as private)
// and the synthesized main() that the namespace pipeline injects.
static const char* aether_type_spelling(Type* t) {
    if (!t) return "void";
    if (t->c_alias) return t->c_alias;
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_INT64:  return "long";
        case TYPE_UINT64: return "ulong";
        case TYPE_UINT32: return "uint32";
        case TYPE_UINT16: return "uint16";
        case TYPE_UINT8:  return "uint8";
        case TYPE_DURATION: return "Duration";
        case TYPE_FLOAT:  return "float";
        case TYPE_LONGDOUBLE: return "longdouble";
        case TYPE_BOOL:   return "bool";
        case TYPE_STRING: return "string";
        case TYPE_PTR:    return "ptr";
        case TYPE_VOID:   return "void";
        default:          return "any";
    }
}

static void emit_function_list(FILE* out, ASTNode* program) {
    if (!out || !program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (!fn || (fn->type != AST_FUNCTION_DEFINITION
                 && fn->type != AST_BUILDER_FUNCTION)) continue;
        if (!fn->value) continue;
        if (fn->value[0] == '_') continue;
        if (fn->is_imported) continue;

        fprintf(out, "%s|%s|", fn->value, aether_type_spelling(fn->node_type));

        int param_count = 0;
        for (int j = 0; j < fn->child_count; j++) {
            ASTNode* c = fn->children[j];
            if (!c) continue;
            if (c->type == AST_GUARD_CLAUSE) continue;
            if (c->type == AST_BLOCK) continue;
            if (c->type != AST_VARIABLE_DECLARATION
             && c->type != AST_PATTERN_VARIABLE) continue;
            if (param_count > 0) fputc(',', out);
            fprintf(out, "%s:%s",
                c->value ? c->value : "_unnamed",
                aether_type_spelling(c->node_type));
            param_count++;
        }
        fputc('\n', out);
    }
}

// Helper: write a JSON-escaped string literal (including the surrounding
// quotes) for an arbitrary C string. NULL is emitted as a JSON null
// literal (no quotes). Used by emit_ast_json — the strings we surface
// (file paths, identifier names) are 7-bit printable in practice, but
// handle the general case so a future caller passing weird input
// doesn't break the schema.
static void emit_json_string(FILE* out, const char* s) {
    if (!s) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') fputs("\\\"", out);
        else if (c == '\\') fputs("\\\\", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc((int)c, out);
    }
    fputc('"', out);
}

// Walk one AST node, emit a JSON object on stdout if it falls in the
// veto filter set, then recurse into children. The `first` flag tracks
// whether the next emitted object needs a leading comma — passed by
// pointer so siblings + descendants share the comma cursor.
//
// `alias_map` / `alias_count` carry the program's import aliases
// (`import std.os as o` → "o" → "std.os") so qualified call sites
// can be canonicalised at emit time. See `resolve_callee_name`.
typedef struct { const char* alias; const char* module; } EmitAlias;
static void emit_ast_node_recursive(FILE* out, ASTNode* node, int* first,
                                     const EmitAlias* alias_map, int alias_count);

// Given a parsed `prefix.suffix` callee spelling, return a malloc'd
// resolved name `<module>_<suffix>` if `prefix` is a known module
// alias, otherwise NULL. Caller must free.
static char* resolve_callee_name(const char* callee,
                                  const EmitAlias* alias_map, int alias_count) {
    if (!callee) return NULL;
    const char* dot = strchr(callee, '.');
    if (!dot) return NULL;

    size_t prefix_len = (size_t)(dot - callee);
    char prefix_buf[128];
    if (prefix_len >= sizeof(prefix_buf)) return NULL;
    memcpy(prefix_buf, callee, prefix_len);
    prefix_buf[prefix_len] = '\0';
    const char* suffix = dot + 1;

    // Try the alias map first (`import X as Y` rewrites Y → X).
    const char* module = NULL;
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(alias_map[i].alias, prefix_buf) == 0) {
            module = alias_map[i].module;
            break;
        }
    }
    // If no alias, the prefix IS the module name (`import std.os`
    // gives `os.system(...)` where prefix=`os` and the module path
    // is `std.os`; the symbol's namespace is the last dotted
    // component of the path, which for `std.os` is `os`). So the
    // callee already canonicalises to `os_system` via prefix_<suffix>.
    if (!module) module = prefix_buf;

    // Module is dotted (`std.os`) — the symbol's namespace is the
    // last segment after the final `.`. Match the rule used at
    // typechecker.c:543: `snprintf(name, "%s_%s", prefix, suffix)`
    // where prefix is the *short* namespace.
    const char* short_ns = strrchr(module, '.');
    short_ns = short_ns ? short_ns + 1 : module;

    size_t out_len = strlen(short_ns) + 1 + strlen(suffix) + 1;
    char* resolved = malloc(out_len);
    if (!resolved) return NULL;
    snprintf(resolved, out_len, "%s_%s", short_ns, suffix);
    return resolved;
}

// Walk the program's top-level import statements and populate an
// alias map. Caller owns the array; the const char* entries borrow
// from AST nodes (don't free them).
static int collect_import_aliases(ASTNode* program, EmitAlias* out, int max) {
    if (!program) return 0;
    int count = 0;
    for (int i = 0; i < program->child_count && count < max; i++) {
        ASTNode* imp = program->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        // Look for an AST_IDENTIFIER child with annotation == "module_alias".
        for (int j = 0; j < imp->child_count; j++) {
            ASTNode* c = imp->children[j];
            if (!c || c->type != AST_IDENTIFIER || !c->value) continue;
            if (c->annotation && strcmp(c->annotation, "module_alias") == 0) {
                out[count].alias = c->value;
                out[count].module = imp->value;
                count++;
                break;
            }
        }
    }
    return count;
}

// --emit=ast: write the post-typecheck AST as JSON to `out`. Emits
// `{"nodes":[ {...}, {...}, ... ]}` with one object per node in the
// filter set (import_statement, extern_function, function_call).
// Other node kinds are walked through but not emitted — we recurse so
// nested calls inside function bodies are reached.
//
// `node->value` on AST_FUNCTION_CALL carries the resolved binding
// (post-merge, post-alias-resolution) — e.g. `os_system` for what the
// user wrote as `o.system(...)` after `import std.os as o`. That's the
// load-bearing field for aeb's veto: it defeats alias-spelling tricks
// a regex would miss.
static void emit_ast_json(FILE* out, ASTNode* program) {
    if (!out || !program) return;
    // Collect import aliases up-front so qualified call sites can be
    // canonicalised — `o.system` post `import std.os as o` becomes
    // `os_system` on the wire (which is what aeb's veto rules match
    // against). Cap at 128 — pathological programs with more aliases
    // get the tail dropped; aeb sees the as-written spelling for
    // those and can decide.
    EmitAlias aliases[128];
    int alias_count = collect_import_aliases(program, aliases, 128);

    fputs("{\"nodes\":[", out);
    int first = 1;
    emit_ast_node_recursive(out, program, &first, aliases, alias_count);
    fputs("]}\n", out);
}

// Returns 1 if the call node's `value` looks like a function-pointer
// indirect call rather than a static target. Today the parser emits
// AST_FUNCTION_CALL with `value == NULL` for some indirect-call shapes
// (the callee expression is the first child rather than baked into
// value). aeb fail-closes on these per the design doc.
static int call_is_indirect(ASTNode* call) {
    if (!call) return 0;
    if (!call->value || call->value[0] == '\0') return 1;
    return 0;
}

// Returns 1 if `arg` is an argument-position child of a function call
// that aeb would consider "captureable" — positional or named args.
// The trailing closure / block that the parser attaches as a final
// FUNCTION_CALL child (DSL builder bodies, `f(x) { ... }`) is NOT an
// argument in the veto sense; the block IS the function body and the
// real args precede it. Filtering here keeps the JSON `args` array
// matching what the user wrote in parens.
static int arg_is_capturable(ASTNode* arg) {
    if (!arg) return 0;
    if (arg->type == AST_BLOCK) return 0;
    if (arg->type == AST_CLOSURE) return 0;
    return 1;
}

// If `arg` resolves to a primitive literal (string / int / float /
// bool — including a named-arg wrapper around one), write the
// JSON-encoded literal payload to `out` and return 1. Otherwise
// return 0 (caller emits `{"computed":true}`).
//
// Detection: post-optimize_ast, every primitive literal in the
// program is an AST_LITERAL node with `node->value` carrying the
// source text. Strings keep their content verbatim (without
// surrounding quotes — those were stripped by the lexer); numbers
// keep their decimal text; booleans surface as "true"/"false".
// AST_NAMED_ARG wraps the value as its single child, so we unwrap.
//
// Edge cases intentionally NOT promoted to literals (caller emits
// `{"computed":true}`): identifier references (resolved variable
// vs constant is ambiguous post-fold for our purposes — aeb's
// "never allow a computed coordinate" rule applies anyway),
// string interpolations (`"${a}${b}"` — those are
// AST_STRING_INTERP not AST_LITERAL), array literals, struct
// literals, function calls, member access. This deliberately
// matches aeb's "literal vs not — the entire distinction" design
// per veto-emit-ast-args.md.
static int emit_literal_arg_payload(FILE* out, ASTNode* arg) {
    if (!arg) return 0;

    // Unwrap a named-arg: emit only the value side.
    if (arg->type == AST_NAMED_ARG && arg->child_count > 0) {
        return emit_literal_arg_payload(out, arg->children[0]);
    }

    if (arg->type != AST_LITERAL || !arg->value) return 0;
    if (!arg->node_type) {
        // Unknown type — be conservative and call it computed.
        return 0;
    }

    switch (arg->node_type->kind) {
        case TYPE_STRING:
        case TYPE_INT:
        case TYPE_INT64:
        case TYPE_FLOAT:
        case TYPE_LONGDOUBLE:
        case TYPE_BOOL:
            fputs("{\"literal\":", out);
            emit_json_string(out, arg->value);
            fputc('}', out);
            return 1;
        default:
            return 0;
    }
}

static void emit_ast_node_recursive(FILE* out, ASTNode* node, int* first,
                                     const EmitAlias* alias_map, int alias_count) {
    if (!node) return;

    int matched = 0;
    if (node->type == AST_IMPORT_STATEMENT) matched = 1;
    else if (node->type == AST_EXTERN_FUNCTION) matched = 1;
    else if (node->type == AST_FUNCTION_CALL) matched = 1;

    if (matched) {
        if (!*first) fputc(',', out);
        *first = 0;
        fputc('{', out);

        // kind
        fputs("\"kind\":", out);
        if (node->type == AST_IMPORT_STATEMENT) {
            emit_json_string(out, "import_statement");
        } else if (node->type == AST_EXTERN_FUNCTION) {
            emit_json_string(out, "extern_function");
        } else {
            emit_json_string(out, "function_call");
        }

        // file (origin) — NULL for synthetic compiler-emitted nodes;
        // aeb treats those as trusted-origin per veto-enhancements.md.
        fputs(",\"file\":", out);
        emit_json_string(out, node->source_file);

        // line — 1-based, matches the user's editor.
        fprintf(out, ",\"line\":%d", node->line);

        if (node->type == AST_IMPORT_STATEMENT) {
            // module — the import path (`std.os`).
            fputs(",\"module\":", out);
            emit_json_string(out, node->value);

            // alias / selected[] / glob — inspect child AST_IDENTIFIER
            // nodes. `import X as Y` carries one child with
            // annotation="module_alias". Glob imports carry
            // annotation="glob_import" on the *statement* (per
            // aether_module.c:1488). Selective imports
            // (`import X (a, b)`) carry one AST_IDENTIFIER per name,
            // un-annotated.
            int is_glob = (node->annotation
                           && strcmp(node->annotation, "glob_import") == 0);
            if (is_glob) {
                fputs(",\"glob\":true", out);
            }
            const char* alias = NULL;
            int sel_count = 0;
            for (int i = 0; i < node->child_count; i++) {
                ASTNode* c = node->children[i];
                if (!c || c->type != AST_IDENTIFIER || !c->value) continue;
                if (c->annotation
                    && strcmp(c->annotation, "module_alias") == 0) {
                    alias = c->value;
                } else if (!is_glob) {
                    sel_count++;
                }
            }
            if (alias) {
                fputs(",\"alias\":", out);
                emit_json_string(out, alias);
            }
            if (sel_count > 0) {
                fputs(",\"selected\":[", out);
                int sel_first = 1;
                for (int i = 0; i < node->child_count; i++) {
                    ASTNode* c = node->children[i];
                    if (!c || c->type != AST_IDENTIFIER || !c->value) continue;
                    if (c->annotation
                        && strcmp(c->annotation, "module_alias") == 0) continue;
                    if (!sel_first) fputc(',', out);
                    sel_first = 0;
                    emit_json_string(out, c->value);
                }
                fputc(']', out);
            }
        } else if (node->type == AST_EXTERN_FUNCTION) {
            // name — the extern's C symbol.
            fputs(",\"name\":", out);
            emit_json_string(out, node->value);

            // variadic: parser stamps a `varargs` marker on
            // `extern foo(..., ...)` declarations (compiler/parser/
            // parser.c, the `TOKEN_DOTDOTDOT` path).
            if (annotation_has_marker(node->annotation, "varargs")) {
                fputs(",\"variadic\":true", out);
            }
        } else {
            // AST_FUNCTION_CALL
            if (call_is_indirect(node)) {
                // No static target — aeb fail-closes on this per
                // veto-enhancements.md (the design doc explicitly
                // distinguishes "couldn't resolve" from "resolved
                // to benign").
                fputs(",\"indirect\":true", out);
            } else {
                // Canonicalise the callee. For qualified calls
                // (`os.system`, `o.system`) the rewriter resolves
                // the prefix against the program's import-alias
                // map and produces the post-merge symbol
                // (`os_system`) — defeating alias-spelling tricks
                // a regex would miss. Bare-name calls
                // (intra-module helpers, top-level user
                // functions) flow through unchanged.
                char* resolved = resolve_callee_name(node->value,
                                                     alias_map, alias_count);
                fputs(",\"callee\":", out);
                emit_json_string(out, resolved ? resolved : node->value);
                free(resolved);
            }

            // Emit args[]: one object per positional/named arg, in
            // source order. Literal args (string/int/float/bool) get
            // {"literal":"<value>"}; anything else gets {"computed":
            // true}. Trailing closures / DSL bodies are NOT args and
            // are filtered out via arg_is_capturable. Per
            // veto-emit-ast-args.md sub-ask 1: the literal-vs-not
            // distinction is the entire requirement; we don't expose
            // computed-expression provenance — aeb fail-closes on
            // any `{"computed":true}` regardless of contents.
            //
            // Always emit the array (even when empty) so aeb's
            // policy can rely on `obj["args"]` being a defined
            // shape; an absent field would require null-coalescing
            // logic.
            fputs(",\"args\":[", out);
            int arg_first = 1;
            for (int ai = 0; ai < node->child_count; ai++) {
                ASTNode* arg = node->children[ai];
                if (!arg_is_capturable(arg)) continue;
                if (!arg_first) fputc(',', out);
                arg_first = 0;
                if (!emit_literal_arg_payload(out, arg)) {
                    fputs("{\"computed\":true}", out);
                }
            }
            fputc(']', out);
        }

        fputc('}', out);
    }

    // Recurse into children regardless of whether this node matched —
    // calls live inside function bodies, which themselves aren't
    // emitted but contain the AST_FUNCTION_CALL nodes we care about.
    for (int i = 0; i < node->child_count; i++) {
        emit_ast_node_recursive(out, node->children[i], first,
                                alias_map, alias_count);
    }
}

// ---- `ae inspect` (--emit=inspect, issue #473) -------------------
//
// An operator-facing view of what a .ae script declares — imports
// (resolved + capability-flagged), capability posture, exports / entry
// point, top-level declarations, and the builder-DSL vocabulary it
// defines. Reads the post-typecheck AST so signatures carry resolved
// types. No codegen, no output file (peer of --emit=ast); prints to
// stdout and exits. Out of scope (per the issue): full type dumps
// (that's aether_describe) and raw AST printing (that's --emit=ast).

// Aether-facing spelling of a resolved type, for signature display.
static const char* inspect_type_name(Type* t) {
    if (!t) return "?";
    if (t->c_alias) return t->c_alias;
    switch (t->kind) {
        case TYPE_INT:       return "int";
        case TYPE_INT64:     return "int64";
        case TYPE_UINT64:    return "uint64";
        case TYPE_UINT32:    return "uint32";
        case TYPE_UINT16:    return "uint16";
        case TYPE_UINT8:     return "uint8";
        case TYPE_DURATION:  return "Duration";
        case TYPE_FLOAT:     return "float";
        case TYPE_LONGDOUBLE: return "longdouble";
        case TYPE_BOOL:      return "bool";
        case TYPE_BYTE:      return "byte";
        case TYPE_STRING:    return "string";
        case TYPE_PTR:       return "ptr";
        case TYPE_VOID:      return "void";
        case TYPE_ACTOR_REF: return "actor";
        case TYPE_ARRAY:     return "array";
        case TYPE_STRUCT:    return t->struct_name ? t->struct_name : "struct";
        case TYPE_TUPLE:     return "tuple";
        default:             return "?";
    }
}

// Render a function/extern return type, expanding a tuple into
// "(T1, T2, ...)". Writes into `buf`.
static void inspect_return_type(Type* t, char* buf, size_t cap) {
    if (t && t->kind == TYPE_TUPLE && t->tuple_count > 0) {
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, cap - pos, "(");
        for (int i = 0; i < t->tuple_count && pos < cap; i++) {
            pos += (size_t)snprintf(buf + pos, cap - pos, "%s%s",
                                    i ? ", " : "",
                                    inspect_type_name(t->tuple_types[i]));
        }
        snprintf(buf + pos, cap - pos, ")");
    } else {
        snprintf(buf, cap, "%s", inspect_type_name(t));
    }
}

// True for the AST node kinds that represent a function/extern parameter.
static int inspect_is_param(ASTNode* n) {
    if (!n) return 0;
    return n->type == AST_VARIABLE_DECLARATION ||
           n->type == AST_PATTERN_VARIABLE ||
           n->type == AST_IDENTIFIER;
}

// Print a function's parameter list: `(name: type, ...)`.
static void inspect_print_params(FILE* out, ASTNode* fn) {
    fputc('(', out);
    int n = 0;
    for (int i = 0; i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (!p || p->type == AST_GUARD_CLAUSE || p->type == AST_BLOCK) continue;
        if (!inspect_is_param(p)) continue;
        if (n++) fputs(", ", out);
        if (p->value) fprintf(out, "%s: ", p->value);
        fputs(inspect_type_name(p->node_type), out);
    }
    fputc(')', out);
}

// Classify an import path into its source bucket.
static const char* inspect_import_origin(const char* mod) {
    if (!mod) return "local";
    if (strncmp(mod, "std.", 4) == 0)     return "stdlib";
    if (strncmp(mod, "contrib.", 8) == 0) return "contrib";
    return "local";
}

// The --with capability a gated module needs, or NULL if ungated.
static const char* inspect_module_capability(const char* mod) {
    if (!mod) return NULL;
    if (strcmp(mod, "std.fs") == 0)   return "fs";
    if (strcmp(mod, "std.net") == 0)  return "net";
    if (strcmp(mod, "std.http") == 0) return "net";
    if (strcmp(mod, "std.tcp") == 0)  return "net";
    if (strcmp(mod, "std.os") == 0)   return "os";
    return NULL;
}

// True iff `c` is a declaration from THIS file (not merged in from an
// imported module). `is_imported` covers most merged nodes, but some —
// e.g. an imported module's top-level constants — aren't flagged, so we
// also confirm the node's stamped origin matches the file under
// inspection. A NULL source_file is a synthetic node the parser invented
// for this file (e.g. main's scaffolding) — treat as local.
static int inspect_is_local(ASTNode* c, const char* path) {
    if (!c || c->is_imported) return 0;
    if (c->source_file && path && strcmp(c->source_file, path) != 0) return 0;
    return 1;
}

static void emit_inspect_report(FILE* out, ASTNode* program, const char* path) {
    if (!out || !program) return;

    // First pass: tallies + artifact shape.
    int n_import = 0, n_fn = 0, n_actor = 0, n_struct = 0, n_const = 0,
        n_extern = 0, n_builder = 0, n_export = 0, has_main = 0;
    int need_fs = 0, need_net = 0, need_os = 0;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* c = program->children[i];
        // Skip declarations merged in from imported modules — inspect
        // reports what THIS file declares, not its dependencies' internals.
        if (!inspect_is_local(c, path)) continue;
        switch (c->type) {
            case AST_IMPORT_STATEMENT: {
                n_import++;
                const char* cap = inspect_module_capability(c->value);
                if (cap) {
                    if (strcmp(cap, "fs") == 0) need_fs = 1;
                    else if (strcmp(cap, "net") == 0) need_net = 1;
                    else if (strcmp(cap, "os") == 0) need_os = 1;
                }
                break;
            }
            case AST_MAIN_FUNCTION:
                has_main = 1;
                break;
            case AST_FUNCTION_DEFINITION:
                n_fn++;
                if (c->value && strcmp(c->value, "main") == 0) has_main = 1;
                break;
            case AST_BUILDER_FUNCTION:   n_builder++; break;
            case AST_ACTOR_DEFINITION:   n_actor++;  break;
            case AST_STRUCT_DEFINITION:  n_struct++; break;
            case AST_CONST_DECLARATION:  n_const++;  break;
            case AST_EXTERN_FUNCTION:    n_extern++; break;
            case AST_EXPORTS_LIST:       n_export += c->child_count; break;
            default: break;
        }
    }

    fprintf(out, "inspect: %s\n", path ? path : "<stdin>");

    // Artifact shape: a module if it has an exports() list, otherwise an
    // executable if it defines main(), otherwise a plain source unit.
    if (n_export > 0)
        fprintf(out, "  artifact:  library / module (%d export%s)\n",
                n_export, n_export == 1 ? "" : "s");
    else if (has_main)
        fprintf(out, "  artifact:  executable (entry: main)\n");
    else
        fprintf(out, "  artifact:  source unit (no main, no exports)\n");

    // Capability posture: what the gated imports require, and whether the
    // current --with flags would grant it.
    if (need_fs || need_net || need_os) {
        fprintf(out, "  capabilities required (gated imports):");
        if (need_fs)  fprintf(out, " fs%s",  with_fs  ? "(granted)" : "");
        if (need_net) fprintf(out, " net%s", with_net ? "(granted)" : "");
        if (need_os)  fprintf(out, " os%s",  with_os  ? "(granted)" : "");
        fputc('\n', out);
        if ((need_fs && !with_fs) || (need_net && !with_net) || (need_os && !with_os))
            fprintf(out, "             (pass --with=<cap,...> to grant under --emit=lib)\n");
    } else {
        fprintf(out, "  capabilities required: none (capability-empty)\n");
    }

    // Imports.
    if (n_import) {
        fprintf(out, "  imports (%d):\n", n_import);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!inspect_is_local(c, path) || c->type != AST_IMPORT_STATEMENT || !c->value) continue;
            const char* cap = inspect_module_capability(c->value);
            fprintf(out, "    %-28s [%s%s%s]\n", c->value,
                    inspect_import_origin(c->value),
                    cap ? ", capability: " : "", cap ? cap : "");
        }
    }

    // Exports (module ABI surface).
    if (n_export) {
        fprintf(out, "  exports (%d):\n", n_export);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!inspect_is_local(c, path) || c->type != AST_EXPORTS_LIST) continue;
            for (int j = 0; j < c->child_count; j++) {
                ASTNode* e = c->children[j];
                if (e && e->value) fprintf(out, "    %s\n", e->value);
            }
        }
    }

    // Declarations.
    if (n_fn) {
        fprintf(out, "  functions (%d):\n", n_fn);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!inspect_is_local(c, path) || c->type != AST_FUNCTION_DEFINITION || !c->value) continue;
            char ret[256];
            inspect_return_type(c->node_type, ret, sizeof(ret));
            fprintf(out, "    %s", c->value);
            inspect_print_params(out, c);
            fprintf(out, " -> %s\n", ret);
        }
    }
    if (n_builder) {
        fprintf(out, "  builder-DSL functions (%d):\n", n_builder);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (!inspect_is_local(c, path) || c->type != AST_BUILDER_FUNCTION || !c->value) continue;
            fprintf(out, "    %s", c->value);
            inspect_print_params(out, c);
            fputc('\n', out);
        }
    }
    if (n_actor) {
        fprintf(out, "  actors (%d):", n_actor);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (inspect_is_local(c, path) && c->type == AST_ACTOR_DEFINITION && c->value)
                fprintf(out, " %s", c->value);
        }
        fputc('\n', out);
    }
    if (n_struct) {
        fprintf(out, "  structs (%d):", n_struct);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (inspect_is_local(c, path) && c->type == AST_STRUCT_DEFINITION && c->value)
                fprintf(out, " %s", c->value);
        }
        fputc('\n', out);
    }
    if (n_const) {
        fprintf(out, "  constants (%d):", n_const);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* c = program->children[i];
            if (inspect_is_local(c, path) && c->type == AST_CONST_DECLARATION && c->value)
                fprintf(out, " %s", c->value);
        }
        fputc('\n', out);
    }
    if (n_extern)
        fprintf(out, "  extern C declarations: %d\n", n_extern);
}

// Emit a self-contained C source file declaring a static const
// AetherNamespaceManifest populated from the parsed manifest AST,
// plus an aether_describe() exported function. This file is linked
// into the namespace .so so the host can introspect at runtime.
static void emit_describe_c(FILE* out, ASTNode* program) {
    if (!out) return;
    ExtractedManifest m;
    extract_manifest(&m, program);

    /* The describe stub is intentionally self-contained — it doesn't
     * #include aether_host.h so that it can be linked into a .so even
     * when the host's include path isn't known to aetherc. We define
     * a *layout-compatible* struct under a different name and cast at
     * the boundary. The host #includes aether_host.h and sees the
     * canonical names. */
    fputs("/* Auto-generated by aetherc --emit-namespace-describe. DO NOT EDIT. */\n", out);
    fputs("#include <stdint.h>\n", out);
    fputs("#include <stddef.h>\n\n", out);
    fputs("struct AetherInputDecl { const char* name; const char* type_signature; };\n", out);
    fputs("struct AetherEventDecl { const char* name; const char* carries_type; };\n", out);
    fputs("struct AetherJavaBinding   { const char* package_name; const char* class_name; };\n", out);
    fputs("struct AetherPythonBinding { const char* module_name; };\n", out);
    fputs("struct AetherRubyBinding   { const char* module_name; };\n", out);
    fputs("struct AetherGoBinding     { const char* package_name; };\n", out);
    fputs("struct AetherNamespaceManifest {\n", out);
    fputs("    const char* namespace_name;\n", out);
    fputs("    int input_count;\n", out);
    fputs("    struct AetherInputDecl inputs[64];\n", out);
    fputs("    int event_count;\n", out);
    fputs("    struct AetherEventDecl events[64];\n", out);
    fputs("    struct AetherJavaBinding   java;\n", out);
    fputs("    struct AetherPythonBinding python;\n", out);
    fputs("    struct AetherRubyBinding   ruby;\n", out);
    fputs("    struct AetherGoBinding     go;\n", out);
    fputs("};\n\n", out);

    fputs("static const struct AetherNamespaceManifest g_aether_describe = {\n", out);
    fputs("    .namespace_name = ", out); c_emit_str(out, m.ns_name); fputs(",\n", out);
    fprintf(out, "    .input_count = %d,\n", m.input_count);
    fputs("    .inputs = {\n", out);
    for (int i = 0; i < m.input_count; i++) {
        fputs("        { ", out); c_emit_str(out, m.inputs[i].name);
        fputs(", ", out); c_emit_str(out, m.inputs[i].type_sig);
        fputs(" },\n", out);
    }
    fputs("    },\n", out);
    fprintf(out, "    .event_count = %d,\n", m.event_count);
    fputs("    .events = {\n", out);
    for (int i = 0; i < m.event_count; i++) {
        fputs("        { ", out); c_emit_str(out, m.events[i].name);
        fputs(", ", out); c_emit_str(out, m.events[i].carries);
        fputs(" },\n", out);
    }
    fputs("    },\n", out);
    fputs("    .java = { ", out); c_emit_str(out, m.java_pkg);
    fputs(", ", out); c_emit_str(out, m.java_class); fputs(" },\n", out);
    fputs("    .python = { ", out); c_emit_str(out, m.py_module); fputs(" },\n", out);
    fputs("    .ruby = { ", out);   c_emit_str(out, m.rb_module); fputs(" },\n", out);
    fputs("    .go = { ", out); c_emit_str(out, m.go_package); fputs(" },\n", out);
    fputs("};\n\n", out);

    /* aether_describe() is the runtime discovery entry point. Hosts
     * cast the returned pointer to AetherNamespaceManifest* (declared
     * canonically in runtime/aether_host.h). */
    fputs("const struct AetherNamespaceManifest* aether_describe(void) {\n", out);
    fputs("    return &g_aether_describe;\n", out);
    fputs("}\n", out);
}

// Compile aether source to C
/* CRITICAL: codegen writes through fprintf, which reports nothing on
 * failure. Without checking the stream before closing it, a full disk or
 * I/O error yields a truncated .c file and a success exit code: with
 * --emit=c the user gets silent corruption, and on the exe path the
 * truncated C reaches the C compiler as a cascade of syntax errors that
 * hides the real cause. Returns 1 when the file was written intact. */
static int close_generated_file(FILE* f, const char* path) {
    if (!f) return 1;
    int failed = ferror(f) != 0;
    if (fclose(f) != 0) failed = 1;
    if (failed) {
        fprintf(stderr, "Error: failed writing '%s' (disk full or I/O error)\n",
                path ? path : "generated output");
        return 0;
    }
    return 1;
}

int compile_source(const char* input_path, const char* output_path) {
    // Read input file
    FILE *input = fopen(input_path, "r");
    if (!input) {
        perror("Error opening input file");
        return 0;
    }
    
    fseek(input, 0, SEEK_END);
    long file_size = ftell(input);
    fseek(input, 0, SEEK_SET);

    /* CRITICAL: ftell reports -1 on an unseekable stream (a FIFO, a
     * character device). Falling through would malloc(0) and then hand
     * fread a size_t-widened SIZE_MAX, writing past a zero-byte buffer. */
    if (file_size < 0) {
        fprintf(stderr, "Error: '%s' is not a seekable file\n", input_path);
        fclose(input);
        return 0;
    }

    char *source = malloc((size_t)file_size + 1);
    if (!source) {
        perror("Memory allocation error");
        fclose(input);
        return 0;
    }
    
    size_t bytes_read = fread(source, 1, file_size, input);
    fclose(input);
    if (bytes_read == 0 && file_size > 0) {
        fprintf(stderr, "Error: Failed to read file\n");
        free(source);
        return 0;
    }
    // On Windows text mode, bytes_read may be less than file_size due to line ending conversion
    // Null-terminate at actual bytes read
    source[bytes_read] = '\0';
    
    if (verbose_mode) printf("Compiling %s...\n", input_path);

    aether_error_init(input_path, source);

    // Step 1: Lexical Analysis
    if (verbose_mode) {
        printf("[Phase 1/5] Lexical Analysis...\n");
    }
    
    lexer_init(source);
    
    Token* tokens[MAX_TOKENS];
    int token_count = 0;
    
    while (token_count < MAX_TOKENS - 1) {
        Token* token = next_token();
        tokens[token_count] = token;
        token_count++;
        
        if (token->type == TOKEN_EOF) {
            break;
        }
        
        if (token->type == TOKEN_ERROR) {
            aether_error_with_code(token->value, token->line, token->column,
                                   AETHER_ERR_SYNTAX);
            // Cleanup tokens
            for (int i = 0; i < token_count; i++) {
                free_token(tokens[i]);
            }
            free(source);
            return 0;
        }
    }
    
    // Check for token overflow (file too large)
    if (token_count >= MAX_TOKENS - 1 && tokens[token_count - 1]->type != TOKEN_EOF) {
        fprintf(stderr, "error: source file exceeds maximum token limit (%d tokens)\n", MAX_TOKENS);
        fprintf(stderr, "  help: split into multiple files using imports\n");
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free(source);
        return 0;
    }

    if (verbose_mode) printf("Generated %d tokens\n", token_count);

    // Step 2: Parsing
    if (verbose_mode) printf("Step 2: Parsing...\n");
    Parser* parser = create_parser(tokens, token_count);
    ASTNode* program = parse_program(parser);

    // Stamp every parsed node with its source path so codegen can
    // emit `#line N "path"` directives. Imported-module nodes get
    // their own path stamped by module_parse_file before clone, so
    // re-stamping the merged tree later is a no-op for them.
    if (program) ast_stamp_source_file(program, input_path);

    if (!program) {
        report_compilation_failure(input_path);
        // Cleanup
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }

    /* Parse can complete and return a partial program even when the
     * parser recorded errors (e.g. reserved-keyword-as-function-name
     * cases that we skip + diagnose rather than abort). Don't proceed
     * to codegen in that case — the partial AST will silently drop
     * the offending decl and downstream compilation produces a
     * binary that calls a nonexistent C function. */
    if (aether_error_count() > 0) {
        report_compilation_failure(input_path);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 0;
    }

    if (verbose_mode) printf("Parse successful\n");

    // --dump-ast: print the AST and exit (no codegen)
    if (dump_ast_mode) {
        print_ast(program, 0);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 1;  // success
    }

    // --emit-namespace-manifest: walk the AST for std.host builder calls
    // and emit JSON to stdout. Used by `ae build --namespace`. No codegen.
    if (emit_namespace_manifest) {
        emit_manifest_json(stdout, program);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 1;  // success
    }

    // --emit-namespace-describe: walk the AST and emit a self-contained
    // C file with the embedded AetherNamespaceManifest + aether_describe()
    // stub. Output path is `output_path` (the second positional argument).
    if (emit_namespace_describe) {
        FILE* out = fopen(output_path, "w");
        if (!out) {
            perror("Error opening output file");
            free_ast_node(program);
            for (int i = 0; i < token_count; i++) free_token(tokens[i]);
            free_parser(parser);
            free(source);
            return 0;
        }
        emit_describe_c(out, program);
        fclose(out);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;
    }

    // Step 2.5: Module Orchestration
    if (verbose_mode) printf("[Phase 2.5/5] Module resolution...\n");
    module_set_source_dir(input_path);
    if (!module_orchestrate(program)) {
        report_compilation_failure(input_path);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }
    if (verbose_mode) printf("Module resolution successful\n");

    /* #953: surface parse/annotation errors from IMPORTED modules. The
     * entry file's own parse errors are gated above (the aether_error_count
     * check after parse_program), but module_orchestrate parses every
     * reachable import and returns success even when one carries an error —
     * it recovers per-module so it can report as many as possible. Because
     * the parser's error recovery can drop the offending construct (e.g. an
     * invalid `@` annotation lowering to a bare `return x`), the merged AST
     * then type-checks clean and codegen produces a working binary from
     * source that does NOT compile — `build` silently disagreeing with
     * `check`. Re-check the global error count here, after orchestration,
     * so both paths fail (non-zero, no binary) on any imported-module error. */
    if (aether_error_count() > 0) {
        report_compilation_failure(input_path);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 0;
    }

    // Step 2.6: Merge pure Aether module functions into program AST
    module_merge_into_program(program);

    // Step 2.65: Tree-shake imported functions the program never calls.
    // Reduces typecheck and gcc compile time on programs that only use
    // a slice of large stdlib modules. Must run after merge (so the
    // closure can see merged helpers) and before typecheck (so dead
    // bodies don't slow it down). See module_prune_unreachable.
    module_prune_unreachable(program);

    // Step 2.67: `@derive(...)` synthesizer (#338).
    // Walks AST_STRUCT_DEFINITION nodes carrying a `derive:<list>`
    // annotation, synthesizes the helper function definitions
    // (T_eq today; format / clone / hash in follow-up commits),
    // and inserts them as siblings into the program. Runs AFTER
    // module_prune_unreachable so synthesized functions don't get
    // pruned as "unused" before the call sites that need them
    // type-check, and BEFORE typecheck_program so synthesized
    // bodies type-check normally.
    if (derive_synthesize_pass(program) != 0) {
        module_registry_shutdown();
        free_ast_node(program);
        for (int k = 0; k < token_count; k++) free_token(tokens[k]);
        free_parser(parser);
        free(source);
        return 0;
    }

    // Step 2.68: compile-time `when` / static-if resolution (#483).
    // Evaluates each `when` condition (target.os / target.arch / const bool)
    // at compile time and prunes the AST to the selected arm BEFORE type
    // checking and codegen run. The unselected arm — including any platform-
    // specific extern it declares — is freed here, so it is never resolved,
    // type-checked, or emitted. Runs after module merge so `when`s in merged
    // module bodies are resolved too, and BEFORE the --emit=lib import gate
    // and typecheck so both see the post-prune program. A non-const `when`
    // condition is a hard error (returns non-zero) — we never guess an arm.
    if (resolve_when_statements(program) != 0) {
        report_compilation_failure(input_path);
        module_registry_shutdown();
        free_ast_node(program);
        for (int k = 0; k < token_count; k++) free_token(tokens[k]);
        free_parser(parser);
        free(source);
        return 0;
    }

    // Step 2.7: --emit=lib capability-empty check.
    // In lib mode the output is consumed by another process (Java host,
    // Python script, etc.) that owns network/filesystem/process access.
    // An embedded Aether script that opens sockets or writes files is
    // a capability escalation — fail the build and point the user to the
    // documented pattern (host does I/O, script returns data).
    //
    // Projects that ARE the host (compile .ae + handwritten C into one
    // binary) opt into specific capabilities with --with=fs / --with=net
    // / --with=os. The gate stays default-deny; --with is the explicit
    // acknowledgement that the host owns and audits the surface it's
    // enabling. Keeping the capability categories coarse (three buckets)
    // matches the three banned-import groupings below.
    if (emit_lib) {
        // Each entry is (module_name, granted_flag, capability_name).
        // When granted_flag is non-zero, that module is allowed; otherwise
        // it's rejected with a message that names the --with flag needed.
        struct { const char* module; bool granted; const char* cap; } gated[] = {
            { "std.fs",   with_fs,  "fs"  },
            { "std.net",  with_net, "net" },
            { "std.http", with_net, "net" },
            { "std.tcp",  with_net, "net" },
            { "std.os",   with_os,  "os"  },
        };
        int num_gated = (int)(sizeof(gated) / sizeof(gated[0]));
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* child = program->children[i];
            if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
            for (int g = 0; g < num_gated; g++) {
                if (strcmp(child->value, gated[g].module) != 0) continue;
                if (gated[g].granted) break;  // opted in; allow.
                fprintf(stderr,
                    "Error: --emit=lib rejects 'import %s' without --with=%s.\n"
                    "\n"
                    "       The library ABI is capability-empty by default so an\n"
                    "       embedded Aether script can't escalate beyond what its\n"
                    "       host grants. Pass --with=%s if the binary linking this\n"
                    "       library is itself the capability owner (i.e. you're\n"
                    "       writing systems code that compiles .ae and handwritten\n"
                    "       C into one executable). Multiple capabilities can be\n"
                    "       comma-separated: --with=%s,os\n",
                    gated[g].module, gated[g].cap, gated[g].cap, gated[g].cap);
                module_registry_shutdown();
                free_ast_node(program);
                for (int k = 0; k < token_count; k++) free_token(tokens[k]);
                free_parser(parser);
                free(source);
                return 0;
            }
        }
    }

    // Step 3: Type Checking
    if (verbose_mode) printf("Step 3: Type checking...\n");
    if (!typecheck_program(program)) {
        report_compilation_failure(input_path);
        // Cleanup
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }
    
    if (verbose_mode) printf("Type checking successful\n");

    // --list-functions: post-typecheck so node_type is populated.
    if (list_functions_mode) {
        emit_function_list(stdout, program);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;
    }

    // --diagnose=ownership: print per-function string-ownership
    // verdicts and exit. Post-typecheck so node_type is populated for
    // the user-fn structural-escape recursion.
    if (diagnose_ownership_mode) {
        codegen_diagnose_ownership(program, stdout);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;
    }

    // --audit-mem: report every raw std.mem offset access and its implied
    // width, then exit (no codegen). Post-typecheck so qualified calls are
    // resolved to their `mem.<fn>` form and node->source_file is stamped.
    if (audit_mem_mode) {
        int total = audit_mem_walk(program, stdout);
        fprintf(stdout, "audit-mem: %d raw std.mem offset access%s found\n",
                total, total == 1 ? "" : "es");
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 0;
    }

    // --check: stop after typecheck + type inference, no codegen
    if (check_only_mode) {
        int warnings = aether_warning_count();
        if (warnings > 0) {
            fprintf(stderr, "OK: %d warning(s)\n", warnings);
        } else {
            fprintf(stderr, "OK: no errors\n");
        }
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 1;  // success
    }

    // Step 3.5: Optimization (AST-level passes: constant folding, dead code, tail calls)
    if (verbose_mode) printf("Step 3.5: Optimizing...\n");
    program = optimize_ast(program);

    // --emit=ast: write the post-typecheck, post-merge, post-fold
    // program AST as JSON to stdout (filter set: import_statement,
    // extern_function, function_call). Conventional exit codes —
    // return 0 here means process exit 0 ("AST emitted OK"); anything
    // other than 0 means parse/typecheck/IO failure further up. aeb's
    // supply-chain veto consumes this; the compiler holds no policy.
    // See veto-enhancements.md and veto-emit-ast-args.md.
    //
    // Emit runs AFTER `optimize_ast` so const-foldable args (numeric
    // arithmetic — `port(1 + 2)` → `port(3)`) arrive as literals on
    // the wire. String-literal concatenation isn't a binary `+` in
    // Aether (the parser rejects `string + string`), so the
    // "computed concat becomes a literal" case only fires for
    // numeric folds today; future fold passes (e.g. a
    // const-`string.concat(lit, lit)` fold) would get picked up
    // automatically.
    if (emit_inspect_mode) {
        emit_inspect_report(stdout, program, input_path);
        fflush(stdout);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;  // process exit 0 (see --emit=ast note below)
    }

    // --emit=effects: derived per-function capability reachability + purity
    // as JSON to stdout (issue #889). Peer of --emit=ast / inspect — runs
    // after typecheck/merge (so the call graph is whole-program), bypasses
    // codegen. An external auditor reads this instead of re-walking the AST.
    if (emit_effects_mode) {
        emit_effects_json(stdout, program);
        fflush(stdout);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;  // process exit 0 (see --emit=ast note below)
    }

    if (emit_ast_mode) {
        emit_ast_json(stdout, program);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        // Return value here is `compile_source`'s internal "non-zero =
        // success" convention; main() translates that to process
        // exit 0 (see the --check / --dump-ast handlers at the bottom
        // of main()). Externally observable: process exit 0 = AST
        // emitted OK, non-zero = parse / typecheck / IO failure.
        return 1;
    }

    // Step 4: Code Generation
    if (verbose_mode) printf("Step 4: Generating C code...\n");
    FILE *output = fopen(output_path, "w");
    if (!output) {
        perror("Error opening output file");
        // Cleanup
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }
    
    // Open header file if --emit-header was specified
    FILE* header_output = NULL;
    char* header_path = NULL;
    if (emit_header_path) {
        if (strcmp(emit_header_path, "auto") == 0) {
            header_path = derive_header_path(output_path);
        } else {
            header_path = strdup(emit_header_path);
        }
        header_output = fopen(header_path, "w");
        if (!header_output) {
            fprintf(stderr, "Warning: Could not open header file %s\n", header_path);
        }
    }

    CodeGenerator* codegen;
    if (header_output) {
        codegen = create_code_generator_with_header(output, header_output, header_path);
        if (verbose_mode) printf("Also generating header: %s\n", header_path);
    } else {
        codegen = create_code_generator(output);
    }
    if (preempt_mode) codegen->preempt_loops = 1;
    if (no_contracts_mode) codegen->no_contracts = 1;
    codegen->emit_exe = emit_exe ? 1 : 0;
    codegen->emit_lib = emit_lib ? 1 : 0;
    // #996 --emit=csrc: open the catalog header if requested. Distinct from the
    // pre-existing --header (header_output) path above.
    FILE* csrc_header = NULL;
    if (csrc_header_path) {
        csrc_header = fopen(csrc_header_path, "w");
        if (!csrc_header) {
            fprintf(stderr, "Error: cannot open --emit-header path '%s'\n", csrc_header_path);
            fclose(output);
            return 1;
        }
        codegen->csrc_header_file = csrc_header;
    }
    // #996 --emit=csrc: also emit the machine-readable JSON catalog when asked,
    // recording the granted --with capabilities in it as provenance. cap_buf is
    // function-scoped so it outlives the generate_program call below.
    FILE* csrc_catalog = NULL;
    char csrc_cap_buf[64];
    if (csrc_catalog_path) {
        csrc_catalog = fopen(csrc_catalog_path, "w");
        if (!csrc_catalog) {
            fprintf(stderr, "Error: cannot open --emit-catalog-json path '%s'\n", csrc_catalog_path);
            if (csrc_header) fclose(csrc_header);
            fclose(output);
            return 1;
        }
        codegen->csrc_catalog_file = csrc_catalog;
        int cp = 0;
        csrc_cap_buf[0] = '\0';
        if (with_fs)  cp += snprintf(csrc_cap_buf + cp, sizeof(csrc_cap_buf) - cp, "%sfs",  cp ? "," : "");
        if (with_net) cp += snprintf(csrc_cap_buf + cp, sizeof(csrc_cap_buf) - cp, "%snet", cp ? "," : "");
        if (with_os)  cp += snprintf(csrc_cap_buf + cp, sizeof(csrc_cap_buf) - cp, "%sos",  cp ? "," : "");
        codegen->csrc_capabilities = csrc_cap_buf;
    }
    codegen->emit_main_target = emit_main_target;  // NULL when not requested
    // Source path so codegen can expand `__FILE__` literally (#265).
    codegen->source_file = input_path;
    int errors_before_codegen = aether_error_count();
    generate_program(codegen, program);
    int write_ok = close_generated_file(output, output_path);
    if (!close_generated_file(csrc_header, csrc_header_path))   write_ok = 0;
    if (!close_generated_file(csrc_catalog, csrc_catalog_path)) write_ok = 0;
    if (!close_generated_file(header_output, header_path))      write_ok = 0;
    if (!write_ok) {
        /* compile_source reports failure as 0; every caller tests
         * !compile_source(...). Returning 1 here would print the error and
         * then hand the truncated output to the C compiler anyway. */
        if (header_path) free(header_path);
        return 0;
    }
    if (header_path) {
        free(header_path);
    }

    // If codegen reported new errors (e.g. L4 closure/state validation),
    // abort with a non-zero exit so callers see the compile failure rather
    // than a half-written output file. Only count errors reported by
    // codegen itself — parse-phase errors are handled separately to avoid
    // regressing legacy tests that silently tolerate parser noise.
    if (aether_error_count() > errors_before_codegen) {
        report_compilation_failure(input_path);
        // Remove the partial output to avoid downstream build steps
        // picking up an incomplete file.
        remove(output_path);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free_code_generator(codegen);
        free(source);
        return 0;
    }

    if (verbose_mode) {
        printf("Code generation successful\n");
        // Print all optimization stats here — series/linear loop collapse happens during codegen,
        // so stats must be printed after generate_program(), not before it.
        print_optimization_stats();
    }

    // Cleanup
    module_registry_shutdown();
    free_ast_node(program);
    for (int i = 0; i < token_count; i++) {
        free_token(tokens[i]);
    }
    free_parser(parser);
    free_code_generator(codegen);
    free(source);

    return 1;
}

// ---------------------------------------------------------------------------
// --concat-ae: source-to-source merge of multiple .ae files into one
// synthetic .ae. Issue #268.1.
//
// Behaviour, line-based:
//   1. Walk each input file. For every line whose first non-whitespace
//      token is `import`, accumulate it into a deduped list (first
//      occurrence wins, order preserved). All other lines are body.
//   2. Count `main(` definitions across all files. Reject with a clear
//      error if more than one (the merge target is one whole-program
//      TU; two `main()`s would collide at link time).
//   3. Emit the synthetic file as: (banner) → (deduped imports) →
//      (each input's body, prefixed with a `// ---- <path> ----` marker
//      so diagnostics still trace back to the original source).
//
// Pure source-to-source — no parser involvement, matching the issue's
// "no `--emit=lib` involvement" contract. Edge cases the line-based
// approach skips intentionally:
//   - Multi-line `import { ... }` blocks (Aether doesn't have these
//     today; if added, this function will need to switch to lexer-
//     level tokenisation).
//   - The string literal `"import "` or the comment `// import …`
//     starting at column 0 — both would be (incorrectly) picked up
//     as imports. The simpler check is correct for the canonical
//     downstream use case (aetherBuild's generated `.build.ae` files,
//     which have imports at the file head and never inside strings).
// ---------------------------------------------------------------------------

static char* concat_ae_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

// Trim trailing CR / spaces / tabs from a length-bounded substring.
static size_t concat_ae_trim_trailing(const char* s, size_t len) {
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
        len--;
    }
    return len;
}

static int concat_ae_imports_contains(char** imports, int count, const char* line, size_t len) {
    for (int i = 0; i < count; i++) {
        if (strncmp(imports[i], line, len) == 0 && imports[i][len] == '\0') return 1;
    }
    return 0;
}

static int do_concat_ae(char** inputs, int input_count, const char* output_path) {
    char** imports        = NULL;
    int    imports_count  = 0;
    int    imports_cap    = 0;
    int    main_count     = 0;
    char** bodies         = malloc(sizeof(char*) * (size_t)input_count);
    if (!bodies) {
        fprintf(stderr, "Error: --concat-ae: out of memory\n");
        return 1;
    }
    for (int i = 0; i < input_count; i++) bodies[i] = NULL;

    for (int f = 0; f < input_count; f++) {
        char* source = concat_ae_read_file(inputs[f]);
        if (!source) {
            fprintf(stderr, "Error: --concat-ae: cannot read '%s'\n", inputs[f]);
            for (int j = 0; j < f; j++) free(bodies[j]);
            free(bodies);
            for (int i = 0; i < imports_count; i++) free(imports[i]);
            free(imports);
            return 1;
        }

        size_t cap = strlen(source) + 64;
        char* body = malloc(cap);
        if (!body) {
            fprintf(stderr, "Error: --concat-ae: out of memory\n");
            free(source);
            for (int j = 0; j < f; j++) free(bodies[j]);
            free(bodies);
            for (int i = 0; i < imports_count; i++) free(imports[i]);
            free(imports);
            return 1;
        }
        size_t pos = 0;

        char* p = source;
        while (*p) {
            char* line_start = p;
            while (*p && *p != '\n') p++;
            size_t raw_len = (size_t)(p - line_start);

            // Trim leading whitespace for classification only — body
            // lines stay verbatim so indent/style is preserved.
            const char* trimmed = line_start;
            size_t trim_len = raw_len;
            while (trim_len > 0 && (*trimmed == ' ' || *trimmed == '\t')) {
                trimmed++;
                trim_len--;
            }
            trim_len = concat_ae_trim_trailing(trimmed, trim_len);

            int is_import = (trim_len >= 7 && strncmp(trimmed, "import ", 7) == 0);
            if (is_import) {
                if (!concat_ae_imports_contains(imports, imports_count, trimmed, trim_len)) {
                    if (imports_count >= imports_cap) {
                        imports_cap = imports_cap ? imports_cap * 2 : 16;
                        char** grown = realloc(imports, sizeof(char*) * (size_t)imports_cap);
                        if (!grown) {
                            fprintf(stderr, "Error: --concat-ae: out of memory\n");
                            free(body);
                            free(source);
                            for (int j = 0; j < f; j++) free(bodies[j]);
                            free(bodies);
                            for (int i = 0; i < imports_count; i++) free(imports[i]);
                            free(imports);
                            return 1;
                        }
                        imports = grown;
                    }
                    char* dup = malloc(trim_len + 1);
                    memcpy(dup, trimmed, trim_len);
                    dup[trim_len] = '\0';
                    imports[imports_count++] = dup;
                }
            } else {
                // Verbatim copy. Detect `main(` definitions before
                // appending so we can fail early on duplicates.
                if (trim_len >= 5 &&
                    strncmp(trimmed, "main", 4) == 0) {
                    const char* m = trimmed + 4;
                    while (*m == ' ' || *m == '\t') m++;
                    if (*m == '(') main_count++;
                }
                if (pos + raw_len + 2 > cap) {
                    cap = (pos + raw_len + 64) * 2;
                    char* grown = realloc(body, cap);
                    if (!grown) {
                        fprintf(stderr, "Error: --concat-ae: out of memory\n");
                        free(body);
                        free(source);
                        for (int j = 0; j < f; j++) free(bodies[j]);
                        free(bodies);
                        for (int i = 0; i < imports_count; i++) free(imports[i]);
                        free(imports);
                        return 1;
                    }
                    body = grown;
                }
                memcpy(body + pos, line_start, raw_len);
                pos += raw_len;
                body[pos++] = '\n';
            }

            if (*p == '\n') p++;
        }
        body[pos] = '\0';
        bodies[f] = body;
        free(source);
    }

    if (main_count > 1) {
        fprintf(stderr,
                "Error: --concat-ae: %d main() definitions across input files; at most one is allowed.\n",
                main_count);
        for (int i = 0; i < input_count; i++) free(bodies[i]);
        free(bodies);
        for (int i = 0; i < imports_count; i++) free(imports[i]);
        free(imports);
        return 1;
    }

    FILE* out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "Error: --concat-ae: cannot write '%s'\n", output_path);
        for (int i = 0; i < input_count; i++) free(bodies[i]);
        free(bodies);
        for (int i = 0; i < imports_count; i++) free(imports[i]);
        free(imports);
        return 1;
    }

    fprintf(out, "// Generated by aetherc --concat-ae from %d file(s).\n", input_count);
    fprintf(out, "// Imports deduped; bodies concatenated verbatim.\n\n");
    for (int i = 0; i < imports_count; i++) {
        fprintf(out, "%s\n", imports[i]);
    }
    if (imports_count > 0) fprintf(out, "\n");
    for (int b = 0; b < input_count; b++) {
        fprintf(out, "// ---- %s ----\n", inputs[b]);
        fputs(bodies[b], out);
        fprintf(out, "\n");
    }
    fclose(out);

    for (int i = 0; i < input_count; i++) free(bodies[i]);
    free(bodies);
    for (int i = 0; i < imports_count; i++) free(imports[i]);
    free(imports);
    return 0;
}

// Parse the `--concat-ae <files...> -o <out>` tail and dispatch.
static int run_concat_ae(int argc, char** argv, int start) {
    const char* output = NULL;
    char** inputs = NULL;
    int input_count = 0;
    int input_cap = 0;

    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --concat-ae: -o requires an output path\n");
                free(inputs);
                return 1;
            }
            output = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: --concat-ae: unknown flag '%s'\n", argv[i]);
            free(inputs);
            return 1;
        } else {
            if (input_count >= input_cap) {
                input_cap = input_cap ? input_cap * 2 : 8;
                char** grown = realloc(inputs, sizeof(char*) * (size_t)input_cap);
                if (!grown) {
                    fprintf(stderr, "Error: --concat-ae: out of memory\n");
                    free(inputs);
                    return 1;
                }
                inputs = grown;
            }
            inputs[input_count++] = argv[i];
        }
    }

    if (!output) {
        fprintf(stderr, "Error: --concat-ae requires -o <output.ae>\n");
        free(inputs);
        return 1;
    }
    if (input_count == 0) {
        fprintf(stderr, "Error: --concat-ae requires at least one input file\n");
        free(inputs);
        return 1;
    }

    int rc = do_concat_ae(inputs, input_count, output);
    free(inputs);
    return rc;
}

void print_help(const char* program_name) {
    printf("Aether Compiler v%s\n\n", AETHER_VERSION);
    printf("Usage:\n");
    printf("  %s <input.ae> <output.c>         Compile Aether to C\n", program_name);
    printf("  %s lsp                           Run the language server on stdio\n", program_name);
    printf("  %s --concat-ae <files...> -o <out.ae>\n", program_name);
    printf("                                   Discover-and-dedupe source merge, emits one\n");
    printf("                                   synthetic .ae with each file's content, imports\n");
    printf("                                   deduped, accepting at most one main()\n");
    printf("\n");
    printf("Options:\n");
    printf("  --version, -v                    Show version information\n");
    printf("  --verbose                        Show detailed compilation phases and timing\n");
    printf("  --emit-c                         Print generated C code to stdout\n");
    printf("  --emit-header [path]             Generate C header for embedding (default: auto)\n");
    printf("  --emit=<exe|lib|both|csrc>       Output artifact (exe default; lib → .so/.dylib; csrc → portable .c + catalog .h + .catalog.json)\n");
    printf("  --emit=ast|inspect|effects       Analysis to stdout, no codegen: AST JSON / declaration\n");
    printf("                                   summary / derived per-function effect+purity JSON (#889)\n");
    printf("  --emit-main=<func>               With --emit=lib: also emit a thin main(argc,argv) shim\n");
    printf("                                   that calls <func>(). Closes the exe/lib symmetry.\n");
    printf("  --emit-namespace-manifest        Print the manifest JSON for a manifest.ae and exit\n");
    printf("  --check                          Type-check only (no code generation)\n");
    printf("  -D SYMBOL                        Define a build symbol for `when defined(SYMBOL)`\n");
    printf("                                   and select(). Present or absent; no value.\n");
    printf("  --no-contracts                   Skip runtime emission of `requires`/`ensures` checks (#348)\n");
    printf("  --dump-ast                       Print AST and exit (no code generation)\n");
    printf("  --diagnose=ownership             Print string-ownership verdicts and exit (no code generation)\n");
    printf("  --audit-mem                      List every raw std.mem offset access + implied width, then exit\n");
    printf("  --emit=ast                       Print post-typecheck AST as JSON to stdout and exit (no code generation).\n");
    printf("  --emit=inspect                   Print an operator-facing summary of what the script declares and exit (drives `ae inspect`).\n");
    printf("                                   Filter set: import_statement, extern_function, function_call.\n");
    printf("                                   Used by aeb's supply-chain veto. Compiler holds no policy.\n");
    printf("  --help, -h                       Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s hello.ae hello.c              Compile to C\n", program_name);
    printf("  %s run hello.ae                  Quick run\n", program_name);
    printf("  %s lsp                           Run the embedded language server\n", program_name);
    printf("  %s --concat-ae a.ae b.ae -o all.ae   Merge sources for whole-program build\n", program_name);
    printf("  %s --verbose hello.ae hello.c    Compile with timing info\n", program_name);
    printf("  %s --emit-header hello.ae hello.c  Generate hello.h for C embedding\n", program_name);
}

int main(int argc, char *argv[]) {
    // Subcommand dispatch — checked BEFORE the flag loop so flags
    // can't shadow it. `aetherc lsp` runs the embedded language
    // server on stdio (same code path as the standalone aether-lsp
    // binary, which is now a thin alias). Issue #327.
    if (argc >= 2 && strcmp(argv[1], "lsp") == 0) {
        LSPServer* server = lsp_server_create();
        if (!server) {
            fprintf(stderr, "Error: failed to create LSP server\n");
            return 1;
        }
        lsp_server_run(server);
        lsp_server_free(server);
        return 0;
    }

    // --concat-ae: same priority as `lsp` — break out before the
    // generic flag loop because the rest of argv is a flag-shaped
    // list of inputs the loop would mis-classify. Issue #268.1.
    if (argc >= 2 && strcmp(argv[1], "--concat-ae") == 0) {
        return run_concat_ae(argc, argv, 2);
    }

    // Parse flags
    int arg_offset = 1;
    while (arg_offset < argc && argv[arg_offset][0] == '-') {
        if (strcmp(argv[arg_offset], "--version") == 0 || strcmp(argv[arg_offset], "-v") == 0) {
            printf("Aether Compiler v%s\n", AETHER_VERSION);
            return 0;
        } else if (strcmp(argv[arg_offset], "--help") == 0 || strcmp(argv[arg_offset], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strncmp(argv[arg_offset], "-D", 2) == 0) {
            /* -D NAME or -DNAME: a build symbol `when defined(NAME)` tests.
             * A `=value` suffix is rejected rather than silently ignored,
             * because a symbol here is present or absent and accepting the
             * spelling would imply it carries a value. */
            const char* name = argv[arg_offset] + 2;
            if (*name == '\0') {
                if (arg_offset + 1 >= argc) {
                    fprintf(stderr, "Error: -D needs a symbol name\n");
                    return 1;
                }
                name = argv[++arg_offset];
            }
            if (strchr(name, '=')) {
                fprintf(stderr, "Error: -D takes a bare name; build symbols are "
                                "present or absent, they do not carry values "
                                "(got '%s')\n", name);
                return 1;
            }
            if (!aether_define_is_valid_name(name)) {
                fprintf(stderr, "Error: -D takes an identifier: letters, digits "
                                "and underscore, not starting with a digit "
                                "(got '%s')\n", name);
                if (strchr(name, ' ') || strchr(name, '\t')) {
                    fprintf(stderr, "  A name with a space means the whole flag "
                                    "arrived as one argument, which is what "
                                    "\"-D NAME\" quoted together produces.\n");
                }
                return 1;
            }
            if (!aether_define_add(name)) {
                fprintf(stderr, "Error: too many -D symbols to add '%s'\n", name);
                return 1;
            }
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--verbose") == 0) {
            verbose_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-c") == 0) {
            emit_c_mode = true;
            arg_offset++;
        } else if (strncmp(argv[arg_offset], "--emit-catalog-header=", 22) == 0) {
            // #996: with --emit=csrc, also write the catalog's public C
            // prototypes to this path (the distributable .h).
            csrc_header_path = argv[arg_offset] + 22;
            arg_offset++;
        } else if (strncmp(argv[arg_offset], "--emit-catalog-json=", 20) == 0) {
            // #996: with --emit=csrc, also write the aether_lib_meta catalog as
            // machine-readable JSON to this path (functions/closures/constants +
            // capability provenance) so binding generators can consume it.
            csrc_catalog_path = argv[arg_offset] + 20;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--dump-ast") == 0) {
            dump_ast_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--check") == 0) {
            check_only_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--preempt") == 0) {
            preempt_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--no-contracts") == 0) {
            no_contracts_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--diagnose=ownership") == 0) {
            diagnose_ownership_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--audit-mem") == 0) {
            audit_mem_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-header") == 0) {
            // Check for optional explicit path argument (must end in .h)
            if (arg_offset + 1 < argc && argv[arg_offset + 1][0] != '-') {
                const char* next_arg = argv[arg_offset + 1];
                size_t len = strlen(next_arg);
                if (len > 2 && next_arg[len-2] == '.' && next_arg[len-1] == 'h') {
                    // Explicit .h path provided
                    emit_header_path = next_arg;
                    arg_offset += 2;
                } else {
                    // Next arg is probably the input file, not a header path
                    emit_header_path = "auto";
                    arg_offset++;
                }
            } else {
                emit_header_path = "auto";  // Auto-derive from output filename
                arg_offset++;
            }
        } else if (strcmp(argv[arg_offset], "--lib") == 0) {
            if (arg_offset + 1 >= argc) {
                fprintf(stderr, "--lib requires an argument\n");
                return 1;
            }
            /* First --lib resets the chain (overriding any
             * AETHER_LIB_DIR env-var seed); subsequent --lib flags
             * append. This lets the `ae` CLI emit one `--lib <X>`
             * per resolved entry without needing platform-fragile
             * separator-string quoting at the shell layer (cmd.exe +
             * MSYS2 mangle `;` inside quoted args). A human passing
             * `--lib "a:b"` still works because both entry points
             * route through the same separator parser. Issue #413. */
            static int seen_lib_flag = 0;
            if (!seen_lib_flag) {
                module_set_lib_dir(argv[arg_offset + 1]);
                seen_lib_flag = 1;
            } else {
                module_add_lib_dirs(argv[arg_offset + 1]);
            }
            arg_offset += 2;
        } else if (strncmp(argv[arg_offset], "--emit=", 7) == 0) {
            const char* val = argv[arg_offset] + 7;
            if (strcmp(val, "exe") == 0) {
                emit_exe = true;
                emit_lib = false;
            } else if (strcmp(val, "lib") == 0) {
                emit_exe = false;
                emit_lib = true;
            } else if (strcmp(val, "csrc") == 0) {
                // #996: --emit=csrc — produce the portable generated C + a
                // catalog header, for distribution as source (compile-on-
                // install / WASM / static-link) rather than a host .so. Codegen
                // is identical to --emit=lib (same aether_<name> catalog); the
                // header is written when --emit-header=<path> is also passed
                // (the `ae` driver supplies it), and the driver skips gcc.
                emit_exe = false;
                emit_lib = true;
            } else if (strcmp(val, "both") == 0) {
                emit_exe = true;
                emit_lib = true;
            } else if (strcmp(val, "ast") == 0) {
                // --emit=ast: post-typecheck AST → JSON on stdout. A
                // peer of --check / --dump-ast / --diagnose=ownership
                // (no codegen, no output file). Distinct from
                // --emit=exe / lib / both which control codegen
                // targets — ast bypasses codegen entirely. See
                // veto-enhancements.md for the design.
                emit_ast_mode = true;
            } else if (strcmp(val, "inspect") == 0) {
                // --emit=inspect: operator-facing declaration summary on
                // stdout (issue #473). Like --emit=ast, bypasses codegen.
                emit_inspect_mode = true;
            } else if (strcmp(val, "effects") == 0) {
                // --emit=effects: derived per-function effect/purity JSON on
                // stdout (issue #889). Like --emit=ast, bypasses codegen.
                emit_effects_mode = true;
            } else {
                fprintf(stderr, "Error: --emit must be one of: exe, lib, both, ast, inspect, effects (got '%s')\n", val);
                return 1;
            }
            arg_offset++;
        } else if (strncmp(argv[arg_offset], "--emit-main=", 12) == 0) {
            // --emit-main=<func>: a follow-up to --emit=lib. The codegen
            // emits a thin main() shim calling the named function, so a
            // single .c can ship as a lib AND as an exe. Issue #268.3.
            const char* val = argv[arg_offset] + 12;
            if (!*val) {
                fprintf(stderr, "Error: --emit-main= requires a function name\n");
                return 1;
            }
            emit_main_target = val;
            arg_offset++;
        } else if (strncmp(argv[arg_offset], "--with=", 7) == 0) {
            // Comma-separated capability opt-ins for --emit=lib. Unknown
            // tokens are a hard error rather than silently ignored — a
            // typo in a capability name should fail the build, not leave
            // the user wondering why their import still gets rejected.
            const char* list = argv[arg_offset] + 7;
            const char* p = list;
            while (*p) {
                const char* start = p;
                while (*p && *p != ',') p++;
                size_t len = (size_t)(p - start);
                if (len == 2 && strncmp(start, "fs", 2) == 0) {
                    with_fs = true;
                } else if (len == 3 && strncmp(start, "net", 3) == 0) {
                    with_net = true;
                } else if (len == 2 && strncmp(start, "os", 2) == 0) {
                    with_os = true;
                } else if ((len == 11 && strncmp(start, "first-party", 11) == 0) ||
                           (len == 3  && strncmp(start, "all", 3) == 0)) {
                    // "I am the host, every capability is granted."
                    // Equivalent to fs,net,os but expresses intent
                    // (systems code, not sandboxed plugin) rather
                    // than enumerating buckets. See docs/emit-lib.md
                    // §"Opting in" for when this is appropriate.
                    with_fs = true;
                    with_net = true;
                    with_os = true;
                } else {
                    fprintf(stderr,
                        "Error: --with= got unknown capability '%.*s'. "
                        "Known: fs, net, os, first-party (alias: all).\n",
                        (int)len, start);
                    return 1;
                }
                if (*p == ',') p++;
            }
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-namespace-manifest") == 0) {
            emit_namespace_manifest = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-namespace-describe") == 0) {
            emit_namespace_describe = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--list-functions") == 0) {
            list_functions_mode = true;
            arg_offset++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg_offset]);
            fprintf(stderr, "Use --help for usage information\n");
            return 1;
        }
    }
    
    if (argc - arg_offset < 1) {
        print_help(argv[0]);
        return 1;
    }

    if (strcmp(argv[arg_offset], "run") == 0) {
        fprintf(stderr,
                "aetherc does not run programs: it is the compiler front end.\n"
                "Use the `ae` toolchain driver instead:\n"
                "\n"
                "    ae run %s\n"
                "\n"
                "`ae` resolves the stdlib, runtime and link flags for your\n"
                "platform; aetherc only translates Aether to C (--emit=c).\n",
                (argc - arg_offset >= 2) ? argv[arg_offset + 1] : "<input.ae>");
        return 1;
    }

    // --dump-ast only needs the input file
    if (dump_ast_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // --diagnose=ownership only needs the input file. The branch in
    // compile_source emits to stdout and short-circuits before codegen.
    if (diagnose_ownership_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // --emit-c: compile and print generated C to stdout
    if (emit_c_mode) {
        // Compile to a temp file, then cat to stdout
        char tmp_path[256];
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/aether_emit_%d.c", (int)getpid());
        if (!compile_source(argv[arg_offset], tmp_path)) {
            return 1;
        }
        FILE* f = fopen(tmp_path, "r");
        if (f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                fwrite(buf, 1, n, stdout);
            }
            fclose(f);
        }
        remove(tmp_path);
        return 0;
    }

    // --check: type-check only, no output file needed
    if (check_only_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // --emit=ast: post-typecheck AST → JSON on stdout. Same input-only
    // shape as --check / --dump-ast / --diagnose=ownership: the file
    // is read but no .c is written; the short-circuit inside
    // compile_source returns before codegen.
    if (emit_ast_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // --emit=inspect: operator-facing declaration summary on stdout
    // (issue #473). Input-only, like --check / --emit=ast — no .c is
    // written; the short-circuit inside compile_source returns before
    // codegen, so the "/dev/null" output is never opened.
    if (emit_inspect_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // Default mode: Compile to C (original behavior)
    if (argc - arg_offset < 2) {
        fprintf(stderr, "Usage: %s <input.ae> <output.c>\n", argv[0]);
        fprintf(stderr, "Use --help for more information\n");
        return 1;
    }

    /* The output path is positional, so `aetherc in.ae -o out.c` would
     * silently write a file literally named "-o" and leave out.c untouched.
     * A path that begins with a dash is a misplaced flag, not a filename. */
    if (argv[arg_offset + 1][0] == '-' && argv[arg_offset + 1][1] != '\0') {
        fprintf(stderr, "Error: '%s' is not an output path.\n", argv[arg_offset + 1]);
        fprintf(stderr, "aetherc takes the output as a positional argument:\n");
        fprintf(stderr, "    %s %s %s\n", argv[0], argv[arg_offset],
                (argc - arg_offset >= 3) ? argv[arg_offset + 2] : "<output.c>");
        return 1;
    }

    if (!compile_source(argv[arg_offset], argv[arg_offset + 1])) {
        return 1;
    }
    
    if (verbose_mode) printf("Output written to %s\n", argv[arg_offset + 1]);
    return 0;
}
