/* Trusted names in `sandbox.enforce(perms, foo, bar) { block }`.
 * See sandbox_trust.h for the model. */
#include "sandbox_trust.h"
#include "typechecker.h"
#include "../aether_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const ASTNode* call; int site; } MarkedCall;

static MarkedCall* g_marks = NULL;
static int g_mark_count = 0, g_mark_cap = 0;
static const ASTNode** g_sites = NULL;   /* g_sites[site - 1] = the enforce call */
static int g_site_count = 0, g_site_cap = 0;

static void mark_call(const ASTNode* call, int site) {
    for (int i = 0; i < g_mark_count; i++) {
        if (g_marks[i].call == call) { g_marks[i].site = site; return; }
    }
    if (g_mark_count == g_mark_cap) {
        g_mark_cap = g_mark_cap ? g_mark_cap * 2 : 32;
        g_marks = realloc(g_marks, (size_t)g_mark_cap * sizeof(MarkedCall));
    }
    g_marks[g_mark_count].call = call;
    g_marks[g_mark_count].site = site;
    g_mark_count++;
}

static int add_site(const ASTNode* enforce_call) {
    if (g_site_count == g_site_cap) {
        g_site_cap = g_site_cap ? g_site_cap * 2 : 16;
        g_sites = realloc(g_sites, (size_t)g_site_cap * sizeof(ASTNode*));
    }
    g_sites[g_site_count++] = enforce_call;
    return g_site_count;
}

int sandbox_trust_site_of_call(const ASTNode* call) {
    for (int i = 0; i < g_mark_count; i++) {
        if (g_marks[i].call == call) return g_marks[i].site;
    }
    return 0;
}

int sandbox_trust_site_of_enforce(const ASTNode* call) {
    for (int i = 0; i < g_site_count; i++) {
        if (g_sites[i] == call) return i + 1;
    }
    return 0;
}

int sandbox_trust_call_count(void) { return g_mark_count; }

const ASTNode* sandbox_trust_call_at(int i) {
    return (i >= 0 && i < g_mark_count) ? g_marks[i].call : NULL;
}

/* ---- the program's view of names ------------------------------------- */

static ASTNode* g_program = NULL;

/* The top-level function definition named `name`, looking through
 * `export` wrappers, or NULL. */
static ASTNode* find_function(const char* name) {
    if (!g_program || !name) return NULL;
    for (int i = 0; i < g_program->child_count; i++) {
        ASTNode* c = g_program->children[i];
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
        if (!c || !c->value) continue;
        if ((c->type == AST_FUNCTION_DEFINITION || c->type == AST_BUILDER_FUNCTION) &&
            strcmp(c->value, name) == 0) {
            return c;
        }
    }
    return NULL;
}

/* 1 when the program declares a C extern called `name` (merged module
 * externs carry their namespace: `os_getenv`). */
static int is_extern_named(const char* name) {
    for (int i = 0; g_program && name && i < g_program->child_count; i++) {
        ASTNode* c = g_program->children[i];
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
        if (c && c->type == AST_EXTERN_FUNCTION && c->value && strcmp(c->value, name) == 0) return 1;
    }
    return 0;
}

/* 1 when the module loaded under namespace `ns` declares `name` (as written
 * in it, unprefixed) as a C extern. A module's externs stay in its own AST. */
static int module_declares_extern(const char* ns, const char* name) {
    AetherModule* m = module_find_by_namespace(ns);
    if (!m || !m->ast || !name) return 0;
    for (int i = 0; i < m->ast->child_count; i++) {
        ASTNode* c = m->ast->children[i];
        if (c && c->type == AST_EXPORT_STATEMENT && c->child_count > 0) c = c->children[0];
        if (c && c->type == AST_EXTERN_FUNCTION && c->value && strcmp(c->value, name) == 0) return 1;
    }
    return 0;
}

/* 1 when `name` is selectively imported into the program (`import m (name)`);
 * *is_extern is set when that module declares it as a C extern. */
static int is_selected_import(const char* name, int* is_extern) {
    if (is_extern) *is_extern = 0;
    for (int i = 0; g_program && i < g_program->child_count; i++) {
        ASTNode* imp = g_program->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        for (int k = 0; k < imp->child_count; k++) {
            ASTNode* s = imp->children[k];
            if (s && s->type == AST_IDENTIFIER && s->value &&
                !(s->annotation && strcmp(s->annotation, "module_alias") == 0) &&
                strcmp(s->value, name) == 0) {
                if (is_extern) {
                    *is_extern = module_declares_extern(module_namespace_of(imp->value), name);
                }
                return 1;
            }
        }
    }
    return 0;
}

/* The namespace an import written as `name` stands for, or NULL. Handles
 * `import db`, `import a.b.db` (written `db`) and `import x as db`. */
static const char* module_namespace_written(const char* name) {
    for (int i = 0; g_program && i < g_program->child_count; i++) {
        ASTNode* imp = g_program->children[i];
        if (!imp || imp->type != AST_IMPORT_STATEMENT || !imp->value) continue;
        const char* written = NULL;
        for (int k = 0; k < imp->child_count; k++) {
            ASTNode* s = imp->children[k];
            if (s && s->type == AST_IDENTIFIER && s->value && s->annotation &&
                strcmp(s->annotation, "module_alias") == 0) {
                written = s->value;
            }
        }
        if (!written) {
            const char* dot = strrchr(imp->value, '.');
            written = dot ? dot + 1 : imp->value;
        }
        if (strcmp(written, name) == 0) return module_namespace_of(imp->value);
    }
    if (module_find_by_namespace(name)) return name;
    return NULL;
}

/* 1 when `call` is std.sandbox's enforce: `<ns>.enforce`, where <ns> is the
 * namespace std.sandbox was loaded under, or a bare `enforce` the program
 * imported from it selectively. */
static int is_enforce_call(const ASTNode* call, const char* sandbox_ns) {
    if (!call || call->type != AST_FUNCTION_CALL || !call->value || !sandbox_ns) return 0;
    size_t n = strlen(sandbox_ns);
    if (strncmp(call->value, sandbox_ns, n) == 0 && strcmp(call->value + n, ".enforce") == 0) {
        return 1;
    }
    if (strcmp(call->value, "enforce") == 0 && !find_function("enforce")) {
        for (int i = 0; g_program && i < g_program->child_count; i++) {
            ASTNode* imp = g_program->children[i];
            if (imp && imp->type == AST_IMPORT_STATEMENT && imp->value &&
                strcmp(imp->value, "std.sandbox") == 0) {
                for (int k = 0; k < imp->child_count; k++) {
                    ASTNode* s = imp->children[k];
                    if (s && s->type == AST_IDENTIFIER && s->value &&
                        (strcmp(s->value, "enforce") == 0 || strcmp(s->value, "*") == 0)) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* ---- one enforce site --------------------------------------------------- */

typedef struct {
    const char* name;      /* as written in the call */
    const char* module_ns; /* non-NULL: a module, matching "<ns>.x" calls */
    int line, column;
    int called;            /* seen as a callee in the block */
} TrustedName;

static int g_errors = 0;

static void trust_error(const char* msg, int line, int column) {
    type_error(msg, line, column);
    g_errors++;
}

static void mark_block(ASTNode* node, TrustedName* names, int count, int site) {
    if (!node) return;
    if (node->type == AST_FUNCTION_CALL && node->value) {
        for (int i = 0; i < count; i++) {
            int hit = 0;
            if (names[i].module_ns) {
                size_t n = strlen(names[i].module_ns);
                hit = strncmp(node->value, names[i].module_ns, n) == 0 && node->value[n] == '.';
            } else {
                hit = strcmp(node->value, names[i].name) == 0;
            }
            if (hit) {
                /* Only an Aether function gets a wrapper: an extern's C
                 * prototype is the header's, and reproducing it for a
                 * wrapper risks a quiet mismatch. Say so here rather than
                 * leave the call silently sandboxed. */
                char mangled[512];
                snprintf(mangled, sizeof mangled, "%s", node->value);
                for (char* q = mangled; *q; q++) {
                    if (*q == '.') *q = '_';
                }
                if (is_extern_named(mangled) || is_extern_named(node->value) ||
                    (names[i].module_ns &&
                     (module_declares_extern(names[i].module_ns,
                                             node->value + strlen(names[i].module_ns) + 1) ||
                      module_declares_extern(names[i].module_ns, mangled)))) {
                    char msg[640];
                    snprintf(msg, sizeof msg,
                             "sandbox.enforce trusts '%s', but '%s' is a C extern, and only Aether "
                             "functions can be trusted: call it from an Aether function and "
                             "trust that function instead",
                             names[i].name, node->value);
                    trust_error(msg, node->line, node->column);
                    names[i].called = 1;
                    continue;
                }
                mark_call(node, site);
                names[i].called = 1;
            }
        }
    } else if (node->type == AST_IDENTIFIER && node->value) {
        for (int i = 0; i < count; i++) {
            if (!names[i].module_ns && strcmp(node->value, names[i].name) == 0) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "'%s' is trusted by this sandbox.enforce, so inside its block it can "
                         "only be called, not used as a value: a value passed on would leave "
                         "the block and run without the exemption, or with it somewhere it was "
                         "not granted. Call it here, or drop it from the trusted names",
                         node->value);
                trust_error(msg, node->line, node->column);
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        mark_block(node->children[i], names, count, site);
    }
}

static void process_enforce(ASTNode* call) {
    /* children: perms, trusted names..., the block */
    int n = call->child_count;
    if (n <= 2) return;
    ASTNode* block = call->children[n - 1];
    if (!block || block->type != AST_CLOSURE) {
        trust_error("sandbox.enforce with trusted names needs its block written at the call "
                    "(`sandbox.enforce(perms, foo) callback { ... }`): the names are matched "
                    "against the calls in that block",
                    call->line, call->column);
        /* Reduce to enforce(perms, block) anyway, so the arity check does
         * not add a second, less useful error about the same call. */
        for (int i = 1; i < n - 1; i++) free_ast_node(call->children[i]);
        call->children[1] = block;
        call->child_count = 2;
        return;
    }
    int count = n - 2;
    TrustedName* names = calloc((size_t)count, sizeof(TrustedName));
    int ok = 1;
    for (int i = 0; i < count; i++) {
        ASTNode* a = call->children[1 + i];
        names[i].line = a ? a->line : call->line;
        names[i].column = a ? a->column : call->column;
        if (!a || a->type != AST_IDENTIFIER || !a->value) {
            trust_error("a trusted name in sandbox.enforce must be the name of a function or an "
                        "imported module, written directly (`sandbox.enforce(perms, audit_log, db)`)",
                        names[i].line, names[i].column);
            ok = 0;
            continue;
        }
        names[i].name = a->value;
        ASTNode* fn = find_function(a->value);
        if (fn) {
            if (fn->type == AST_BUILDER_FUNCTION) {
                char msg[384];
                snprintf(msg, sizeof msg,
                         "sandbox.enforce cannot trust '%s': it is a builder function, which "
                         "takes a trailing block. Trust a plain function that calls it",
                         a->value);
                trust_error(msg, a->line, a->column);
                ok = 0;
            }
            for (int k = 0; k < fn->child_count; k++) {
                ASTNode* p = fn->children[k];
                if (p && (p->type == AST_PATTERN_LIST || p->type == AST_PATTERN_CONS ||
                          p->type == AST_PATTERN_LITERAL || p->type == AST_PATTERN_STRUCT)) {
                    char msg[384];
                    snprintf(msg, sizeof msg,
                             "sandbox.enforce cannot trust '%s': it matches on its parameters "
                             "(a multi-clause function). Trust a plain function that calls it",
                             a->value);
                    trust_error(msg, a->line, a->column);
                    ok = 0;
                    break;
                }
            }
            continue;
        }
        const char* ns = module_namespace_written(a->value);
        if (ns) {
            names[i].module_ns = ns;
            continue;
        }
        int sel_extern = 0;
        if (is_selected_import(a->value, &sel_extern)) {
            if (sel_extern) {
                char emsg[384];
                snprintf(emsg, sizeof emsg,
                         "sandbox.enforce cannot trust '%s': it is a C extern, and only Aether "
                         "functions can be trusted. Call it from an Aether function and trust "
                         "that function instead",
                         a->value);
                trust_error(emsg, a->line, a->column);
                ok = 0;
            }
            continue;
        }
        if (is_extern_named(a->value)) {
            char emsg[384];
            snprintf(emsg, sizeof emsg,
                     "sandbox.enforce cannot trust '%s': it is a C extern, and only Aether "
                     "functions can be trusted. Call it from an Aether function and trust "
                     "that function instead",
                     a->value);
            trust_error(emsg, a->line, a->column);
            ok = 0;
            continue;
        }
        char msg[384];
        snprintf(msg, sizeof msg,
                 "sandbox.enforce: '%s' is not a function or an imported module, so it cannot "
                 "be trusted",
                 a->value);
        trust_error(msg, a->line, a->column);
        ok = 0;
    }
    if (ok) {
        int site = add_site(call);
        mark_block(block, names, count, site);
        for (int i = 0; i < count; i++) {
            if (names[i].called) continue;
            char msg[384];
            snprintf(msg, sizeof msg,
                     "sandbox.enforce trusts '%s', but the block never calls %s: drop the name, "
                     "or check its spelling (a trusted name only covers calls written in the block)",
                     names[i].name, names[i].module_ns ? "into that module" : "it");
            trust_error(msg, names[i].line, names[i].column);
        }
    }
    free(names);
    /* Drop the names: what remains type-checks as enforce(perms, block). The
     * name nodes are no longer referenced by anything but the old slots. */
    for (int i = 1; i < n - 1; i++) free_ast_node(call->children[i]);
    call->children[1] = block;
    call->child_count = 2;
}

static void walk(ASTNode* node, const char* sandbox_ns) {
    if (!node) return;
    /* Children first is NOT what we want: an outer enforce must mark its
     * block before an inner one re-marks the calls lexically closest to it. */
    if (is_enforce_call(node, sandbox_ns)) process_enforce(node);
    for (int i = 0; i < node->child_count; i++) walk(node->children[i], sandbox_ns);
}

int sandbox_trust_pass(ASTNode* program) {
    g_program = program;
    g_errors = 0;
    g_mark_count = 0;
    g_site_count = 0;
    if (!program) return 0;
    if (!module_find("std.sandbox")) return 0;
    const char* ns = module_namespace_of("std.sandbox");
    walk(program, ns);
    return g_errors;
}
