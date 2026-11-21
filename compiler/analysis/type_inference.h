#ifndef TYPE_INFERENCE_H
#define TYPE_INFERENCE_H

#include "../ast.h"
#include "../aether_strmap.h"
#include "typechecker.h"

// Type constraint: "node N must have type T"
typedef struct TypeConstraint {
    ASTNode* node;
    Type* required_type;
    const char* reason;  // For error messages
    int line;
    int column;
    int resolved;  // 1 if constraint is satisfied
} TypeConstraint;

// Inference context
typedef struct InferenceContext {
    TypeConstraint* constraints;
    int constraint_count;
    int constraint_capacity;
    SymbolTable* symbols;
    int iteration_count;
    ASTNode* scope_owner;   // the function / main / closure body being walked (#2124)
    /* #2173: the id of the function walk in progress (0 outside one). Every
     * symbol the walk adds is stamped with it; see rebindable_symbol. */
    unsigned walk_id;
    /* The DSL blocks being walked, innermost last (#333, #2766): the trailing
     * block of a member-access call resolves its bare calls through the
     * call's receiver (`<receiver>_<name>`), as the checker's scope for it
     * does, and binds names of its own, the symbols added above `dsl_heads`
     * since it was entered. */
#define INFER_MAX_DSL_BLOCKS 32
    char* dsl_receivers[INFER_MAX_DSL_BLOCKS];
    Symbol* dsl_heads[INFER_MAX_DSL_BLOCKS];
    int dsl_depth;
    /* #3064: the joins of the names bound again in each body walked, one
     * HoistJoins per function, main or closure (by its node), built on the
     * first name bound again there and kept for the walk. */
    StrMap hoist_joins;
    /* The program's module-level `var`s, by name (values unused): a bare
     * binding of one in a function writes the global rather than declaring
     * a local (see bind_untyped_walk_local). Built once by infer_all_types
     * and shared by each pass's context; not owned here. */
    const StrMap* module_vars;
} InferenceContext;

// Main API
int infer_all_types(ASTNode* program, SymbolTable* table);

// Internal functions
InferenceContext* create_inference_context(SymbolTable* table, const StrMap* module_vars);
void free_inference_context(InferenceContext* ctx);

void collect_constraints(ASTNode* node, InferenceContext* ctx);
void collect_literal_constraints(ASTNode* node, InferenceContext* ctx);
void collect_function_constraints(ASTNode* node, InferenceContext* ctx);
void collect_expression_constraints(ASTNode* node, InferenceContext* ctx);

int solve_constraints(InferenceContext* ctx);
void propagate_known_types(InferenceContext* ctx);
int has_unresolved_types(InferenceContext* ctx);

void add_constraint(InferenceContext* ctx, ASTNode* node, Type* type, const char* reason);
void report_ambiguous_types(InferenceContext* ctx);

// Utility
int is_type_inferrable(Type* type);
Type* infer_from_literal(const char* value);
Type* infer_from_binary_op(Type* left, Type* right, const char* operator);

#endif // TYPE_INFERENCE_H

