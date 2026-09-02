#include "type_inference.h"
#include "../aether_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>

#define MAX_INFERENCE_ITERATIONS 100

// Create inference context
InferenceContext* create_inference_context(SymbolTable* table) {
    InferenceContext* ctx = (InferenceContext*)malloc(sizeof(InferenceContext));
    ctx->constraints = NULL;
    ctx->constraint_count = 0;
    ctx->constraint_capacity = 0;
    ctx->symbols = table;
    ctx->iteration_count = 0;
    return ctx;
}

void free_inference_context(InferenceContext* ctx) {
    if (!ctx) return;
    
    for (int i = 0; i < ctx->constraint_count; i++) {
        if (ctx->constraints[i].required_type) {
            free_type(ctx->constraints[i].required_type);
        }
    }
    
    if (ctx->constraints) {
        free(ctx->constraints);
    }
    
    free(ctx);
}

// Add constraint
void add_constraint(InferenceContext* ctx, ASTNode* node, Type* type, const char* reason) {
    if (!ctx || !node || !type) return;
    
    // Grow array if needed
    if (ctx->constraint_count >= ctx->constraint_capacity) {
        int new_capacity = ctx->constraint_capacity == 0 ? 16 : ctx->constraint_capacity * 2;
        TypeConstraint* new_constraints = (TypeConstraint*)realloc(ctx->constraints, 
                                                     new_capacity * sizeof(TypeConstraint));
        if (!new_constraints) return;
        ctx->constraints = new_constraints;
        ctx->constraint_capacity = new_capacity;
    }
    
    TypeConstraint* constraint = &ctx->constraints[ctx->constraint_count++];
    constraint->node = node;
    constraint->required_type = clone_type(type);
    constraint->reason = reason;
    constraint->line = node->line;
    constraint->column = node->column;
    constraint->resolved = 0;
}

// Check if type needs inference
int is_type_inferrable(Type* type) {
    return type && type->kind == TYPE_UNKNOWN;
}

// Pick the narrowest integer kind that can hold a parsed magnitude.
// signed_input=1 means the source literal was a plain decimal that the
// caller would interpret as signed; signed_input=0 means hex/oct/bin
// which Aether treats as bit patterns (a 0xFFFF...FFFF literal is the
// caller's u64 max, not -1).
static TypeKind pick_integer_kind(unsigned long long magnitude, int signed_input) {
    if (signed_input) {
        if (magnitude <= (unsigned long long)INT_MAX) return TYPE_INT;
        if (magnitude <= (unsigned long long)LLONG_MAX) return TYPE_INT64;
        return TYPE_UINT64;
    }
    if (magnitude <= (unsigned long long)INT_MAX) return TYPE_INT;
    if (magnitude <= (unsigned long long)LLONG_MAX) return TYPE_INT64;
    return TYPE_UINT64;
}

static int duration_unit_ns(const char* unit, long long* out) {
    if (!unit || !out) return 0;
    if (strcmp(unit, "ns") == 0) { *out = 1LL; return 1; }
    if (strcmp(unit, "us") == 0) { *out = 1000LL; return 1; }
    if (strcmp(unit, "ms") == 0) { *out = 1000000LL; return 1; }
    if (strcmp(unit, "s") == 0)  { *out = 1000000000LL; return 1; }
    if (strcmp(unit, "m") == 0)  { *out = 60LL * 1000000000LL; return 1; }
    if (strcmp(unit, "h") == 0)  { *out = 60LL * 60LL * 1000000000LL; return 1; }
    if (strcmp(unit, "d") == 0)  { *out = 24LL * 60LL * 60LL * 1000000000LL; return 1; }
    return 0;
}

static int is_duration_literal_text(const char* value) {
    if (!value || !isdigit((unsigned char)value[0])) return 0;
    const char* p = value;
    int saw_unit = 0;
    while (*p) {
        int saw_digit = 0;
        while (isdigit((unsigned char)*p) || *p == '.') {
            if (isdigit((unsigned char)*p)) saw_digit = 1;
            p++;
        }
        if (!saw_digit) return 0;
        char unit[3] = {0, 0, 0};
        if ((p[0] == 'n' || p[0] == 'u' || p[0] == 'm') && p[1] == 's') {
            unit[0] = p[0]; unit[1] = p[1]; p += 2;
        } else if (*p == 's' || *p == 'm' || *p == 'h' || *p == 'd') {
            unit[0] = *p; p++;
        } else {
            return 0;
        }
        long long ns = 0;
        if (!duration_unit_ns(unit, &ns)) return 0;
        saw_unit = 1;
    }
    return saw_unit;
}

// Infer type from literal value
Type* infer_from_literal(const char* value) {
    if (!value) return create_type(TYPE_UNKNOWN);

    if (is_duration_literal_text(value)) {
        return create_type(TYPE_DURATION);
    }

    // Prefixed forms (0x / 0o / 0b) are pure-integer bit patterns.
    // Pick the narrowest integer kind that holds the value so that
    // e.g. 0xB5026F5AA96619E9 doesn't silently truncate to TYPE_INT
    // and then to int32 in the emitted C.
    if (value[0] == '0' && value[1] && value[2]) {
        int base = 0;
        const char* digits = NULL;
        if (value[1] == 'x' || value[1] == 'X') { base = 16; digits = value; }
        else if (value[1] == 'o' || value[1] == 'O') { base = 8;  digits = value + 2; }
        else if (value[1] == 'b' || value[1] == 'B') { base = 2;  digits = value + 2; }
        if (base != 0) {
            errno = 0;
            unsigned long long mag = strtoull(digits, NULL, base);
            if (errno != 0) {
                /* Out-of-range literal — fall through to the digit
                 * loop, which will classify it as not-a-number and
                 * leave the typechecker to surface a diagnostic. */
            } else {
                return create_type(pick_integer_kind(mag, 0));
            }
        }
    }

    // Check if it's a number. Start is_number = 0 so the empty
    // buffer doesn't decay to TYPE_INT — require at least one
    // digit to flip it on. See bug #2 in
    // tests/integration/multi_return_destructure_chain/ for the cluster.
    int is_float = 0;
    int is_number = 0;

    for (const char* p = value; *p; p++) {
        if (*p == '.') {
            is_float = 1;
        } else if (isdigit((unsigned char)*p)) {
            is_number = 1;
        } else if (*p != '-' && *p != '+') {
            is_number = 0;
            break;
        }
    }

    if (is_number) {
        if (is_float) return create_type(TYPE_FLOAT);
        /* A plain decimal literal that exceeds INT_MAX would otherwise
         * be stamped TYPE_INT and silently truncate in codegen. Reparse
         * with strtoull (handles optional leading +/-) and pick the
         * narrowest integer kind that holds it. */
        errno = 0;
        const char* p = value;
        int negative = 0;
        if (*p == '-') { negative = 1; p++; }
        else if (*p == '+') { p++; }
        unsigned long long mag = strtoull(p, NULL, 10);
        if (errno != 0) {
            return create_type(TYPE_INT64);
        }
        TypeKind kind = pick_integer_kind(mag, 1);
        /* A negative value larger in magnitude than INT_MAX still fits
         * in int64; never widen a negative-decimal to UINT64. */
        if (negative && kind == TYPE_UINT64) kind = TYPE_INT64;
        return create_type(kind);
    }
    
    // Check if it's a string literal (starts with quote)
    if (value[0] == '"' || value[0] == '\'') {
        return create_type(TYPE_STRING);
    }
    
    // Check for boolean
    if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        return create_type(TYPE_BOOL);
    }
    
    return create_type(TYPE_UNKNOWN);
}

// Infer type from binary operation
Type* infer_from_binary_op(Type* left, Type* right, const char* operator) {
    if (!left || !right) return create_type(TYPE_UNKNOWN);
    
    // Arithmetic operators: +, -, *, /, %
    if (strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0 ||
        strcmp(operator, "*") == 0 || strcmp(operator, "/") == 0 ||
        strcmp(operator, "%") == 0) {
        if ((strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0) &&
            left->kind == TYPE_DURATION && right->kind == TYPE_DURATION) {
            return create_type(TYPE_DURATION);
        }
        if ((strcmp(operator, "*") == 0 || strcmp(operator, "/") == 0) &&
            left->kind == TYPE_DURATION &&
            (right->kind == TYPE_INT || right->kind == TYPE_INT64 ||
             right->kind == TYPE_UINT64 || right->kind == TYPE_FLOAT)) {
            return create_type(TYPE_DURATION);
        }
        if (strcmp(operator, "*") == 0 &&
            right->kind == TYPE_DURATION &&
            (left->kind == TYPE_INT || left->kind == TYPE_INT64 ||
             left->kind == TYPE_UINT64 || left->kind == TYPE_FLOAT)) {
            return create_type(TYPE_DURATION);
        }
        if (strcmp(operator, "/") == 0 &&
            left->kind == TYPE_DURATION && right->kind == TYPE_DURATION) {
            return create_type(TYPE_FLOAT);
        }
        // If either is int64 (long), promote to int64
        if (left->kind == TYPE_INT64 || right->kind == TYPE_INT64) {
            return create_type(TYPE_INT64);
        }
        // ptr arithmetic for + and -: result is ptr (real pointer
        // offsetting). ptr - ptr would be ptrdiff_t, currently lowered
        // as int — leave as-is until a use case surfaces. The previous
        // version of this rule said `ptr +-*/ int → int` which silently
        // truncated 64-bit pointers when stored in an inferred-type
        // local variable; high-address heap allocations on Linux x86_64
        // (typically ~0x55_5555_5555_0000+) would get clobbered. Real
        // C ptr arithmetic — used by mquickjs port — needs ptr-result.
        if (strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0) {
            if ((left->kind == TYPE_PTR && right->kind == TYPE_INT) ||
                (left->kind == TYPE_PTR && right->kind == TYPE_INT64)) {
                return create_type(TYPE_PTR);
            }
            if (left->kind == TYPE_INT && right->kind == TYPE_PTR) {
                return create_type(TYPE_PTR);
            }
            // ptr - ptr: caller doing pointer-difference arithmetic
            if (left->kind == TYPE_PTR && right->kind == TYPE_PTR &&
                strcmp(operator, "-") == 0) {
                return create_type(TYPE_INT64);
            }
        }
        // For *, /, % on ptr/int: keep the legacy "ptr-as-int" behavior
        // (some Aether code uses ptr to box int values; multiplying makes
        // no sense for real pointer arithmetic). Result is int.
        if ((left->kind == TYPE_PTR && right->kind == TYPE_INT) ||
            (left->kind == TYPE_INT && right->kind == TYPE_PTR)) {
            return create_type(TYPE_INT);
        }
        // If both are int, result is int
        if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
            return create_type(TYPE_INT);
        }
        // #749: longdouble is the widest numeric — wins over float/int.
        if (left->kind == TYPE_LONGDOUBLE || right->kind == TYPE_LONGDOUBLE) {
            return create_type(TYPE_LONGDOUBLE);
        }
        // If either is float, result is float
        if (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT) {
            return create_type(TYPE_FLOAT);
        }
        // String concatenation for +
        if (strcmp(operator, "+") == 0 && 
            (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
            return create_type(TYPE_STRING);
        }
    }
    
    // Comparison operators: ==, !=, <, <=, >, >=
    if (strcmp(operator, "==") == 0 || strcmp(operator, "!=") == 0 ||
        strcmp(operator, "<") == 0 || strcmp(operator, "<=") == 0 ||
        strcmp(operator, ">") == 0 || strcmp(operator, ">=") == 0) {
        return create_type(TYPE_BOOL);
    }
    
    // Logical operators: &&, ||
    if (strcmp(operator, "&&") == 0 || strcmp(operator, "||") == 0) {
        return create_type(TYPE_BOOL);
    }
    
    return create_type(TYPE_UNKNOWN);
}

// Collect constraints from literals
void collect_literal_constraints(ASTNode* node, InferenceContext* ctx) {
    if (!node || node->type != AST_LITERAL) return;
    
    if (node->node_type && is_type_inferrable(node->node_type)) {
        Type* inferred = infer_from_literal(node->value);
        if (inferred->kind != TYPE_UNKNOWN) {
            add_constraint(ctx, node, inferred, "literal type inference");
            free_type(node->node_type);
            node->node_type = clone_type(inferred);
        }
        free_type(inferred);
    }
}

// Collect constraints from expressions
void collect_expression_constraints(ASTNode* node, InferenceContext* ctx) {
    if (!node) return;
    
    switch (node->type) {
        case AST_BINARY_EXPRESSION:
            if (node->child_count >= 2) {
                collect_constraints(node->children[0], ctx);
                collect_constraints(node->children[1], ctx);
                
                Type* left_type = node->children[0]->node_type;
                Type* right_type = node->children[1]->node_type;
                
                if (left_type && right_type && node->value) {
                    Type* result_type = infer_from_binary_op(left_type, right_type, node->value);
                    if (result_type->kind != TYPE_UNKNOWN) {
                        if (node->node_type) free_type(node->node_type);
                        node->node_type = result_type;
                    } else {
                        free_type(result_type);
                    }
                }
            }
            break;
            
        case AST_COMPOUND_ASSIGNMENT:
            // children[0] = operator, children[1] = RHS expression
            if (node->child_count >= 2) {
                collect_constraints(node->children[1], ctx);
                // Infer type from existing variable
                if (node->value && ctx->symbols) {
                    Symbol* sym = lookup_symbol(ctx->symbols, node->value);
                    if (sym && sym->type && sym->type->kind != TYPE_UNKNOWN) {
                        if (!node->node_type || node->node_type->kind == TYPE_UNKNOWN) {
                            if (node->node_type) free_type(node->node_type);
                            node->node_type = clone_type(sym->type);
                        }
                    }
                }
            }
            break;

        case AST_VARIABLE_DECLARATION:
        case AST_STATE_DECLARATION:
            // Always process initializer if present (even with explicit types)
            if (node->child_count > 0) {
                collect_constraints(node->children[0], ctx);

                // If declaration type is unknown, infer it from initializer
                if (is_type_inferrable(node->node_type)) {
                    Type* init_type = node->children[0]->node_type;
                    if (init_type && init_type->kind != TYPE_UNKNOWN) {
                        /* #892: a NAMED array initializer decays to a pointer
                         * (C array-to-pointer decay), so `ids = static_ids`
                         * (static_ids: byte[N]) infers `ids` as a plain `ptr`
                         * and a later `ids = heap` stays legal. Mirror the
                         * decay in the typechecker (infer path) so codegen
                         * emits `void* ids`, not an array declarator. An array
                         * LITERAL is not an identifier, so it still binds an
                         * array. */
                        if (init_type->kind == TYPE_ARRAY &&
                            node->children[0]->type == AST_IDENTIFIER) {
                            free_type(node->node_type);
                            node->node_type = create_type(TYPE_PTR);
                        } else {
                            free_type(node->node_type);
                            node->node_type = clone_type(init_type);
                            add_constraint(ctx, node, init_type, "variable initialization");
                        }
                    }
                }
            }

            // Add variable to symbol table for later lookups (member access, etc.)
            if (node->value && node->node_type && node->node_type->kind != TYPE_UNKNOWN && ctx->symbols) {
                Symbol* existing = lookup_symbol_local(ctx->symbols, node->value);
                if (existing) {
                    // Update existing symbol's type
                    if (existing->type) free_type(existing->type);
                    existing->type = clone_type(node->node_type);
                } else {
                    // Add new symbol
                    add_symbol(ctx->symbols, node->value, clone_type(node->node_type), 0, 0, 0);
                }
            }
            break;

        case AST_TUPLE_DESTRUCTURE: {
            // a, b, _ = func() — last child is the RHS expression; preceding
            // children are the destructure lvalues (AST_VARIABLE_DECLARATIONs).
            // Without this case, destructured locals aren't visible in the
            // current function's symbol table, so `return v` after
            // `v, _ = some_tuple_call()` falls back to default inference (int).
            if (node->child_count >= 2) {
                int var_count = node->child_count - 1;
                ASTNode* rhs = node->children[var_count];

                // Process RHS so its tuple type gets resolved
                collect_constraints(rhs, ctx);

                Type* rhs_type = rhs ? rhs->node_type : NULL;
                if (rhs_type && rhs_type->kind == TYPE_TUPLE &&
                    rhs_type->tuple_count == var_count) {
                    // Bind each lvalue's slot type onto the corresponding
                    // AST_VARIABLE_DECLARATION node and into the symbol table.
                    for (int j = 0; j < var_count; j++) {
                        ASTNode* var = node->children[j];
                        if (!var) continue;
                        Type* slot = rhs_type->tuple_types[j];
                        if (!slot || slot->kind == TYPE_UNKNOWN) continue;

                        if (var->node_type) free_type(var->node_type);
                        var->node_type = clone_type(slot);

                        if (var->value && strcmp(var->value, "_") != 0 && ctx->symbols) {
                            Symbol* existing = lookup_symbol_local(ctx->symbols, var->value);
                            if (existing) {
                                if (existing->type) free_type(existing->type);
                                existing->type = clone_type(slot);
                            } else {
                                add_symbol(ctx->symbols, var->value, clone_type(slot), 0, 0, 0);
                            }
                        }
                    }
                }
            }
            break;
        }
            
        case AST_IDENTIFIER:
            // Look up in symbol table
            if (ctx->symbols) {
                Symbol* sym = lookup_symbol(ctx->symbols, node->value);
                if (sym && sym->type && sym->type->kind != TYPE_UNKNOWN) {
                    if (!node->node_type || is_type_inferrable(node->node_type)) {
                        if (node->node_type) free_type(node->node_type);
                        node->node_type = clone_type(sym->type);
                    }
                }
            }
            break;
            
        case AST_FUNCTION_CALL:
            // Process argument expressions
            for (int i = 0; i < node->child_count; i++) {
                collect_constraints(node->children[i], ctx);
            }

            // Look up function definition to get return type
            if (node->value) {
                Symbol* func_sym = lookup_qualified_symbol(ctx->symbols, node->value);
                if (func_sym && func_sym->type) {
                    if (!node->node_type || node->node_type->kind == TYPE_UNKNOWN) {
                        /* Calling an fn-typed local (is_fnptr=1): the
                         * call's result type is the function-type's
                         * return slot, NOT the full function type.
                         * Without this carve-out, `result = fp(...)`
                         * would stamp `result` as having type
                         * `fn(int, int) -> int` instead of `int`. */
                        if (func_sym->type->kind == TYPE_FUNCTION &&
                            func_sym->type->is_fnptr &&
                            func_sym->type->return_type) {
                            free_type(node->node_type);
                            node->node_type = clone_type(func_sym->type->return_type);
                        } else {
                            // Function call inherits the function's return type
                            free_type(node->node_type);
                            node->node_type = clone_type(func_sym->type);
                        }
                    }
                }
            }
            break;
            
        case AST_RETURN_STATEMENT:
            // Infer from return expression
            if (node->child_count > 0) {
                collect_constraints(node->children[0], ctx);
                if (!node->node_type || is_type_inferrable(node->node_type)) {
                    Type* expr_type = node->children[0]->node_type;
                    if (expr_type && expr_type->kind != TYPE_UNKNOWN) {
                        free_type(node->node_type);
                        node->node_type = clone_type(expr_type);
                    }
                }
            }
            break;
            
        case AST_MEMBER_ACCESS:
            // Member access: expr.field
            // Infer type from the struct field or Message type
            if (node->child_count > 0 && node->value) {
                ASTNode* base_expr = node->children[0];
                collect_constraints(base_expr, ctx);
                
                // Get the base expression's type
                Type* base_type = base_expr->node_type;
                
                // Handle Message type member access
                if (base_type && base_type->kind == TYPE_MESSAGE) {
                    // Message has fields: type (int), sender_id (int), payload_int (int), payload_ptr (void*)
                    if (strcmp(node->value, "type") == 0 || 
                        strcmp(node->value, "sender_id") == 0 || 
                        strcmp(node->value, "payload_int") == 0) {
                        free_type(node->node_type);
                        node->node_type = create_type(TYPE_INT);
                    } else if (strcmp(node->value, "payload_ptr") == 0) {
                        free_type(node->node_type);
                        node->node_type = create_type(TYPE_VOID); // void* represented as void
                    }
                }
                // #1132 bitstruct field access: the field's declared type comes
                // straight off its AST_BITSTRUCT_FIELD node. The definition is
                // stashed on the bitstruct's global symbol at registration.
                else if (base_type && base_type->kind == TYPE_BITSTRUCT && ctx->symbols) {
                    Symbol* bs = lookup_symbol(ctx->symbols, base_type->struct_name);
                    if (bs && bs->node) {
                        for (int i = 0; i < bs->node->child_count; i++) {
                            ASTNode* f = bs->node->children[i];
                            if (f && f->type == AST_BITSTRUCT_FIELD && f->value &&
                                strcmp(f->value, node->value) == 0) {
                                free_type(node->node_type);
                                node->node_type = clone_type(f->node_type);
                                break;
                            }
                        }
                    }
                }
                // Handle struct type member access
                else if (base_type && base_type->kind == TYPE_STRUCT && ctx->symbols) {
                    // Look up the struct definition
                    Symbol* struct_sym = lookup_symbol(ctx->symbols, base_type->struct_name);
                    
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        // Find the field in the struct definition
                        for (int i = 0; i < struct_def->child_count; i++) {
                            ASTNode* field = struct_def->children[i];
                            if (field && field->type == AST_STRUCT_FIELD && 
                                field->value && strcmp(field->value, node->value) == 0) {
                                // Found matching field - use its type
                                if (field->node_type) {
                                    free_type(node->node_type);
                                    node->node_type = clone_type(field->node_type);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            break;
            
        case AST_ARRAY_ACCESS:
            // Array access: arr[index]
            // Infer element type from array type
            if (node->child_count >= 2) {
                ASTNode* array_expr = node->children[0];
                ASTNode* index_expr = node->children[1];
                
                collect_constraints(array_expr, ctx);
                collect_constraints(index_expr, ctx);
                
                // Get array type and extract element type
                Type* array_type = array_expr->node_type;
                if (array_type && array_type->kind == TYPE_ARRAY && array_type->element_type) {
                    free_type(node->node_type);
                    node->node_type = clone_type(array_type->element_type);
                }
            }
            break;
            
        case AST_STRUCT_LITERAL:
            // Struct literal: StructName{ field: value, ... }
            // Look up struct definition to propagate field types
            if (node->value && ctx->symbols) {
                Symbol* struct_sym = lookup_symbol(ctx->symbols, node->value);
                
                if (struct_sym && struct_sym->type && struct_sym->type->kind == TYPE_STRUCT) {
                    // Set the struct literal's type to the struct type
                    free_type(node->node_type);
                    node->node_type = clone_type(struct_sym->type);
                }
                
                // Process each field initializer and propagate types to struct definition
                for (int i = 0; i < node->child_count; i++) {
                    ASTNode* field_init = node->children[i];
                    if (field_init && field_init->type == AST_ASSIGNMENT && field_init->child_count > 0) {
                        // Collect constraints from the value expression
                        collect_constraints(field_init->children[0], ctx);
                        
                        Type* val_type = field_init->children[0]->node_type;
                        
                        // Propagate field type back to struct definition
                        if (struct_sym && struct_sym->node && val_type && val_type->kind != TYPE_UNKNOWN) {
                            ASTNode* struct_def = struct_sym->node;
                            const char* field_name = field_init->value;
                            
                            // Find matching field in struct definition
                            for (int j = 0; j < struct_def->child_count; j++) {
                                ASTNode* field = struct_def->children[j];
                                if (field && field->type == AST_STRUCT_FIELD && 
                                    field->value && strcmp(field->value, field_name) == 0) {
                                    // Update field type if it's unknown
                                    if (!field->node_type || field->node_type->kind == TYPE_UNKNOWN) {
                                        if (field->node_type) free_type(field->node_type);
                                        field->node_type = clone_type(val_type);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            break;
            
        default:
            // Recursively collect from children
            for (int i = 0; i < node->child_count; i++) {
                collect_constraints(node->children[i], ctx);
            }
            break;
    }
}

// Infer return type from return statements in function body.
// Called with is_top_level=true only from infer_function_return_types; the
// recursive descent into control-flow children uses is_top_level=false.
// Given an identifier name, scan preceding siblings in a block for the
// variable declaration of that name and return the type of its initializer.
static Type* resolve_local_var_type(const char* name, ASTNode* block, int before_index, SymbolTable* symbols) {
    if (!name || !block) return NULL;
    for (int i = before_index - 1; i >= 0; i--) {
        ASTNode* stmt = block->children[i];
        if (!stmt) continue;

        // Tuple destructure: `name, _ = some_call()` — the destructure
        // node holds AST_VARIABLE_DECLARATION children for each lvalue
        // and the RHS expression as the last child. If `name` matches
        // one of the lvalues, return the type of the corresponding
        // tuple slot from the RHS's resolved tuple type. Without this
        // branch, callers like `target = build._get(ctx, key)` whose
        // body is `v, _ = map.get(ctx, key); return v` can't resolve
        // `_get`'s return type, defaulting it to int and breaking
        // every caller.
        if (stmt->type == AST_TUPLE_DESTRUCTURE && stmt->child_count >= 2) {
            int var_count = stmt->child_count - 1;
            ASTNode* rhs = stmt->children[var_count];
            for (int j = 0; j < var_count; j++) {
                ASTNode* var = stmt->children[j];
                if (!var || !var->value) continue;
                if (strcmp(var->value, name) != 0) continue;
                if (var->node_type && var->node_type->kind != TYPE_UNKNOWN) {
                    return clone_type(var->node_type);
                }
                if (rhs && rhs->node_type && rhs->node_type->kind == TYPE_TUPLE &&
                    j < rhs->node_type->tuple_count) {
                    Type* slot = rhs->node_type->tuple_types[j];
                    if (slot && slot->kind != TYPE_UNKNOWN) {
                        return clone_type(slot);
                    }
                }
                // RHS is a function call whose return type might be a
                // tuple resolved later in the inference loop — look it up.
                if (rhs && rhs->type == AST_FUNCTION_CALL && rhs->value && symbols) {
                    Symbol* func_sym = lookup_symbol(symbols, rhs->value);
                    if (!func_sym && strchr(rhs->value, '.')) {
                        char mangled[256];
                        strncpy(mangled, rhs->value, sizeof(mangled) - 1);
                        mangled[sizeof(mangled) - 1] = '\0';
                        for (char* p = mangled; *p; p++) { if (*p == '.') *p = '_'; }
                        func_sym = lookup_symbol(symbols, mangled);
                    }
                    if (func_sym && func_sym->type &&
                        func_sym->type->kind == TYPE_TUPLE &&
                        j < func_sym->type->tuple_count) {
                        Type* slot = func_sym->type->tuple_types[j];
                        if (slot && slot->kind != TYPE_UNKNOWN) {
                            return clone_type(slot);
                        }
                    }
                }
                break;
            }
        }

        if (stmt->type == AST_VARIABLE_DECLARATION && stmt->value &&
            strcmp(stmt->value, name) == 0 && stmt->child_count > 0) {
            ASTNode* init = stmt->children[0];
            // Use node_type if already set
            if (init->node_type && init->node_type->kind != TYPE_UNKNOWN) {
                return clone_type(init->node_type);
            }
            // Recognize common expression types directly
            if (init->type == AST_STRING_INTERP) return create_type(TYPE_STRING);
            if (init->type == AST_NULL_LITERAL) return create_type(TYPE_PTR);
            if (init->type == AST_LITERAL && init->node_type)
                return clone_type(init->node_type);
            if (init->type == AST_ARRAY_LITERAL)
                return init->node_type ? clone_type(init->node_type) : create_type(TYPE_ARRAY);
            if (init->type == AST_STRUCT_LITERAL && init->node_type)
                return clone_type(init->node_type);
            // Function call — look up function return type in symbol table
            if (init->type == AST_FUNCTION_CALL && init->value && symbols) {
                Symbol* func_sym = lookup_symbol(symbols, init->value);
                if (func_sym && func_sym->type && func_sym->type->kind != TYPE_UNKNOWN)
                    return clone_type(func_sym->type);
            }
            break;
        }
    }
    return NULL;
}

// Walk the subtree rooted at `node` looking for multi-value return
// statements. For each untyped AST_IDENTIFIER slot, resolve the
// type from `outer_block`'s preceding siblings (where any
// destructure that introduced the local lives) and stamp it onto
// the slot's node_type. After this pass, the recursive
// infer_return_type_impl below sees a fully-typed tuple instead of
// one with UNKNOWN slots. See Bug #4 in
// tests/integration/multi_return_destructure_chain/.
//
// Bounded recursion: only descends into block-shaped children
// (AST_BLOCK, AST_IF_STATEMENT, AST_FOR_LOOP, AST_WHILE_LOOP,
// AST_SWITCH_STATEMENT, AST_MATCH_STATEMENT, AST_MATCH_ARM,
// AST_DEFER_STATEMENT). Doesn't descend into nested function or
// closure definitions — they have their own scopes.
static void preresolve_return_idents_in(ASTNode* node, ASTNode* outer_block,
                                        int outer_index, SymbolTable* symbols) {
    if (!node) return;

    if (node->type == AST_RETURN_STATEMENT && node->child_count > 1) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* slot = node->children[i];
            if (!slot) continue;
            if (slot->type == AST_EXPRESSION_STATEMENT && slot->child_count > 0)
                slot = slot->children[0];
            if (slot->type != AST_IDENTIFIER || !slot->value) continue;
            if (slot->node_type && slot->node_type->kind != TYPE_UNKNOWN) continue;
            Type* local_type = resolve_local_var_type(slot->value, outer_block, outer_index, symbols);
            if (local_type) {
                if (slot->node_type) free_type(slot->node_type);
                slot->node_type = local_type;
            }
        }
        // Don't descend further — return statements terminate.
        return;
    }

    // Recurse into block-shaped children only. Skip function /
    // closure boundaries — those open new scopes whose locals can't
    // be resolved from `outer_block`.
    if (node->type == AST_FUNCTION_DEFINITION ||
        node->type == AST_BUILDER_FUNCTION ||
        node->type == AST_CLOSURE) {
        return;
    }

    int recurse =
        node->type == AST_BLOCK ||
        node->type == AST_IF_STATEMENT ||
        node->type == AST_FOR_LOOP ||
        node->type == AST_WHILE_LOOP ||
        node->type == AST_SWITCH_STATEMENT ||
        node->type == AST_MATCH_STATEMENT ||
        node->type == AST_MATCH_ARM ||
        node->type == AST_DEFER_STATEMENT;
    if (!recurse) return;

    for (int i = 0; i < node->child_count; i++) {
        preresolve_return_idents_in(node->children[i], outer_block, outer_index, symbols);
    }
}

static Type* infer_return_type_impl(ASTNode* body, SymbolTable* symbols, bool is_top_level) {
    if (!body) return NULL;

    if (body->type == AST_RETURN_STATEMENT && body->child_count > 0) {
        // Multi-value return: return a, b → TYPE_TUPLE
        if (body->child_count > 1) {
            Type* tuple = create_type(TYPE_TUPLE);
            tuple->tuple_count = body->child_count;
            tuple->tuple_types = malloc(body->child_count * sizeof(Type*));
            int has_unknown = 0;
            for (int i = 0; i < body->child_count; i++) {
                ASTNode* val = body->children[i];
                if (val->node_type && val->node_type->kind != TYPE_UNKNOWN) {
                    tuple->tuple_types[i] = clone_type(val->node_type);
                } else if (val->type == AST_IDENTIFIER && val->value && symbols) {
                    Symbol* sym = lookup_symbol(symbols, val->value);
                    if (sym && sym->type && sym->type->kind != TYPE_UNKNOWN) {
                        tuple->tuple_types[i] = clone_type(sym->type);
                    } else {
                        tuple->tuple_types[i] = create_type(TYPE_UNKNOWN);
                        has_unknown = 1;
                    }
                } else if (val->type == AST_LITERAL && val->value) {
                    // Infer literal type from value
                    if (strcmp(val->value, "true") == 0 || strcmp(val->value, "false") == 0) {
                        tuple->tuple_types[i] = create_type(TYPE_BOOL);
                    } else {
                        // Check if numeric. Start is_num = 0 and require at
                        // least one numeric character to flip it on; an
                        // empty buffer (the "" literal in tuple slots) must
                        // resolve as TYPE_STRING, not TYPE_INT. See bug #2
                        // in tests/integration/multi_return_destructure_chain/.
                        int is_num = 0;
                        for (const char* p = val->value; *p; p++) {
                            if (*p >= '0' && *p <= '9') { is_num = 1; }
                            else if (*p != '-' && *p != '.')      { is_num = 0; break; }
                        }
                        tuple->tuple_types[i] = create_type(is_num ? TYPE_INT : TYPE_STRING);
                    }
                } else {
                    tuple->tuple_types[i] = create_type(TYPE_UNKNOWN);
                    has_unknown = 1;
                }
            }
            // If all elements have UNKNOWN type, return NULL
            // If partially resolved, still return it — codegen will use function return type
            if (has_unknown) {
                int all_unknown = 1;
                for (int i2 = 0; i2 < tuple->tuple_count; i2++) {
                    if (tuple->tuple_types[i2]->kind != TYPE_UNKNOWN) { all_unknown = 0; break; }
                }
                if (all_unknown) {
                    free_type(tuple);
                    return NULL;
                }
            }
            return tuple;
        }

        ASTNode* return_expr = body->children[0];
        // Unwrap AST_EXPRESSION_STATEMENT (created by implicit return wrapping)
        if (return_expr->type == AST_EXPRESSION_STATEMENT && return_expr->child_count > 0) {
            return_expr = return_expr->children[0];
        }
        if (return_expr->node_type && return_expr->node_type->kind != TYPE_UNKNOWN) {
            return clone_type(return_expr->node_type);
        }
        // node_type may not be set on identifiers — resolve via symbol table
        if (return_expr->type == AST_IDENTIFIER && return_expr->value && symbols) {
            Symbol* sym = lookup_symbol(symbols, return_expr->value);
            if (sym && sym->type && sym->type->kind != TYPE_UNKNOWN) {
                return clone_type(sym->type);
            }
        }
        // String interpolation always returns a string (ptr)
        if (return_expr->type == AST_STRING_INTERP) {
            return create_type(TYPE_STRING);
        }
    }

    // Arrow function: the body IS the return expression (not a block).
    // Only applies at the top level (direct child of the function node).
    if (is_top_level &&
        body->type != AST_BLOCK && body->type != AST_RETURN_STATEMENT) {
        if (body->node_type && body->node_type->kind != TYPE_UNKNOWN &&
            body->node_type->kind != TYPE_VOID) {
            return clone_type(body->node_type);
        }
        // Resolve identifier types via symbol table
        if (body->type == AST_IDENTIFIER && body->value && symbols) {
            Symbol* sym = lookup_symbol(symbols, body->value);
            if (sym && sym->type && sym->type->kind != TYPE_UNKNOWN) {
                return clone_type(sym->type);
            }
        }
        if (body->type == AST_STRING_INTERP) {
            return create_type(TYPE_STRING);
        }
    }

    // Only descend into control-flow nodes that may contain return statements.
    // This avoids mistaking a string literal inside print() for a return type.
    switch (body->type) {
        case AST_BLOCK:
            // Pre-resolve nested multi-value returns. The existing
            // direct-child pre-resolve below handles single-value
            // returns at the block's top level only. A multi-value
            // return inside an if/while/for body sees AST_IDENTIFIER
            // slots whose types were never set (the surrounding
            // destructure that introduced the local lives in `body`,
            // not in the nested body that holds the return).
            // preresolve_return_idents_in walks the subtree from
            // here, finds every multi-value AST_RETURN_STATEMENT,
            // and stamps each untyped AST_IDENTIFIER slot from this
            // outer block's resolve_local_var_type. After this pass,
            // the recursive infer_return_type_impl below sees a
            // fully-typed tuple instead of one with UNKNOWN slots.
            // Bug #4 in tests/integration/multi_return_destructure_chain/.
            for (int i = 0; i < body->child_count; i++) {
                preresolve_return_idents_in(body->children[i], body, i, symbols);
            }
            for (int i = 0; i < body->child_count; i++) {
                ASTNode* child = body->children[i];
                if (!child) continue;
                // For return statements in a block, resolve local variable types
                // from preceding siblings before recursing.
                if (child->type == AST_RETURN_STATEMENT && child->child_count > 0) {
                    ASTNode* ret_expr = child->children[0];
                    // Unwrap AST_EXPRESSION_STATEMENT (from implicit return)
                    if (ret_expr->type == AST_EXPRESSION_STATEMENT && ret_expr->child_count > 0)
                        ret_expr = ret_expr->children[0];
                    if (ret_expr->type == AST_IDENTIFIER && ret_expr->value &&
                        (!ret_expr->node_type || ret_expr->node_type->kind == TYPE_UNKNOWN)) {
                        Type* local_type = resolve_local_var_type(ret_expr->value, body, i, symbols);
                        if (local_type) return local_type;
                    }
                }
                Type* rt = infer_return_type_impl(child, symbols, false);
                if (rt) return rt;
            }
            break;
        case AST_IF_STATEMENT:
        case AST_FOR_LOOP:
        case AST_WHILE_LOOP:
        case AST_SWITCH_STATEMENT:
        case AST_MATCH_STATEMENT:
        case AST_MATCH_ARM:
        case AST_DEFER_STATEMENT:
            for (int i = 0; i < body->child_count; i++) {
                if (!body->children[i]) continue;
                Type* rt = infer_return_type_impl(body->children[i], symbols, false);
                if (rt) return rt;
            }
            break;
        default:
            break;
    }

    return NULL;
}

Type* infer_return_type_from_body(ASTNode* body, SymbolTable* symbols) {
    return infer_return_type_impl(body, symbols, true);
}

// Collect constraints from function.
//
// Local variables declared inside this function are added to the symbol
// table while the body is processed, then unwound at the end so they
// don't leak into sibling functions. Without the unwind, `z` defined as
// a string local in one function and `z` destructured as a ptr local in
// another collide at the global table level — `lookup_symbol` returns
// either type depending on traversal order, producing spurious E0200
// "Type mismatch in variable initialization" diagnostics on later
// `something = z` assignments inside the second function.
//
// We unwind by snapshotting the head of the symbol list before processing
// and trimming back to it after. Function-level symbols (function
// definitions, externs, imports) are added by other code paths before any
// function body is visited, so they sit beneath the snapshot and are
// unaffected.
void collect_function_constraints(ASTNode* node, InferenceContext* ctx) {
    if (!node || (node->type != AST_FUNCTION_DEFINITION &&
                  node->type != AST_BUILDER_FUNCTION &&
                  node->type != AST_MAIN_FUNCTION)) return;

    Symbol* saved_head = ctx->symbols ? ctx->symbols->symbols : NULL;

    /* Issue #243 sealed scopes: relax qualified-call visibility
     * while walking the body of a cloned merged-module function so
     * internal calls into transitively-merged namespaces (e.g.
     * `json.parse` inside a merged http.client function) resolve
     * correctly. Save/restore the SymbolTable flag — same channel
     * the typechecker uses, just transient over this walk. */
    int saved_inside_merged = ctx->symbols ? ctx->symbols->inside_merged_body : 0;
    if (node->is_imported && ctx->symbols) {
        ctx->symbols->inside_merged_body = 1;
    }

    // Add parameters to symbol table so identifiers in function body can look them up
    int body_index = node->child_count - 1;
    for (int i = 0; i < body_index; i++) {
        ASTNode* param = node->children[i];
        if (param && param->value && param->node_type &&
            (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE)) {
            // Check if parameter already exists in symbol table
            Symbol* existing = lookup_symbol(ctx->symbols, param->value);
            if (existing) {
                // Update existing symbol's type if we now have a more specific type
                if (param->node_type->kind != TYPE_UNKNOWN) {
                    if (existing->type) free_type(existing->type);
                    existing->type = clone_type(param->node_type);
                }
            } else {
                // Add new symbol
                add_symbol(ctx->symbols, param->value, clone_type(param->node_type), 0, 0, 0);
            }
        }
    }

    // Collect constraints from function body
    if (body_index >= 0 && body_index < node->child_count) {
        collect_constraints(node->children[body_index], ctx);
    }

    // Unwind any symbols this function added so they don't pollute sibling
    // functions' lookups.
    if (ctx->symbols) {
        Symbol* current = ctx->symbols->symbols;
        while (current && current != saved_head) {
            Symbol* next = current->next;
            if (current->name) free(current->name);
            if (current->type) free_type(current->type);
            if (current->alias_target) free(current->alias_target);
            free(current);
            current = next;
        }
        ctx->symbols->symbols = saved_head;
    }

    if (ctx->symbols) ctx->symbols->inside_merged_body = saved_inside_merged;
}

// Main constraint collection
void collect_constraints(ASTNode* node, InferenceContext* ctx) {
    if (!node) return;
    
    switch (node->type) {
        case AST_LITERAL:
            collect_literal_constraints(node, ctx);
            break;

        case AST_NULL_LITERAL:
            if (!node->node_type) node->node_type = create_type(TYPE_PTR);
            break;

        case AST_ARRAY_LITERAL:
            // Infer array type from first element
            if (node->child_count > 0) {
                collect_constraints(node->children[0], ctx);
                Type* elem_type = node->children[0]->node_type;
                if (elem_type && elem_type->kind != TYPE_UNKNOWN) {
                    // Create array type with dynamic size (-1)
                    Type* array_type = create_array_type(clone_type(elem_type), node->child_count);
                    node->node_type = array_type;
                    add_constraint(ctx, node, array_type, "array literal type inference");
                }
                // Collect constraints for all elements
                for (int i = 1; i < node->child_count; i++) {
                    collect_constraints(node->children[i], ctx);
                }
            }
            break;
            
        case AST_BUILDER_FUNCTION:
        case AST_FUNCTION_DEFINITION:
        case AST_MAIN_FUNCTION:
            collect_function_constraints(node, ctx);
            break;

        case AST_STRUCT_DEFINITION:
            // Struct fields with initializers
            for (int i = 0; i < node->child_count; i++) {
                collect_constraints(node->children[i], ctx);
            }
            break;
            
        default:
            collect_expression_constraints(node, ctx);
            break;
    }
}

// Check if there are unresolved types
int has_unresolved_types(InferenceContext* ctx) {
    if (!ctx || !ctx->constraints) return 0;
    for (int i = 0; i < ctx->constraint_count; i++) {
        if (!ctx->constraints[i].resolved) {
            return 1;
        }
    }
    return 0;
}

// Propagate known types through constraint graph
void propagate_known_types(InferenceContext* ctx) {
    // Track progress for iterative propagation
    int progress = 0;
    (void)progress;  // Reserved for future iterative algorithm
    
    for (int i = 0; i < ctx->constraint_count; i++) {
        TypeConstraint* constraint = &ctx->constraints[i];
        
        if (constraint->resolved) continue;
        
        ASTNode* node = constraint->node;
        Type* required_type = constraint->required_type;
        
        // If node doesn't have a type or has unknown type, apply constraint
        if (!node->node_type || is_type_inferrable(node->node_type)) {
            if (node->node_type) {
                free_type(node->node_type);
            }
            node->node_type = clone_type(required_type);
            constraint->resolved = 1;
            progress = 1;
        }
        // If types match, mark as resolved
        else if (types_equal(node->node_type, required_type)) {
            constraint->resolved = 1;
            progress = 1;
        }
    }
}

// Solve constraints iteratively
int solve_constraints(InferenceContext* ctx) {
    ctx->iteration_count = 0;
    
    while (has_unresolved_types(ctx) && ctx->iteration_count < MAX_INFERENCE_ITERATIONS) {
        propagate_known_types(ctx);
        ctx->iteration_count++;
    }
    
    if (ctx->iteration_count >= MAX_INFERENCE_ITERATIONS) {
        report_ambiguous_types(ctx);
        return 0;
    }
    
    return 1;
}

// Report ambiguous types
void report_ambiguous_types(InferenceContext* ctx) {
    for (int i = 0; i < ctx->constraint_count; i++) {
        if (!ctx->constraints[i].resolved) {
            TypeConstraint* c = &ctx->constraints[i];
            aether_error_with_suggestion(
                c->reason ? c->reason : "cannot infer type",
                c->line, c->column,
                "add an explicit type annotation, e.g. x: int = ...");
        }
    }
}

// Propagate types from function call sites to function definitions
int propagate_function_call_types(ASTNode* program, SymbolTable* table);
int propagate_call_types_in_tree(ASTNode* tree, const char* func_name, ASTNode* func_def, int param_count);

// Helper to recursively find function calls and propagate types
int propagate_call_types_in_tree(ASTNode* tree, const char* func_name, ASTNode* func_def, int param_count) {
    if (!tree || !func_name) return 0;
    int changed = 0;

    // For function definitions: skip parameter nodes but recurse into the body
    if (tree->type == AST_FUNCTION_DEFINITION || tree->type == AST_BUILDER_FUNCTION) {
        int body_idx = tree->child_count - 1;
        if (body_idx >= 0 && tree->children[body_idx]) {
            changed += propagate_call_types_in_tree(tree->children[body_idx], func_name, func_def, param_count);
        }
        return changed;
    }
    
    // Check if this is a function call to our target function
    // Also match qualified calls: "mymath.double_it" matches definition "mymath_double_it"
    int is_match = 0;
    if (tree->type == AST_FUNCTION_CALL && tree->value) {
        if (strcmp(tree->value, func_name) == 0) {
            is_match = 1;
        } else if (strchr(tree->value, '.')) {
            // Convert dots to underscores and check
            char mangled[512];
            strncpy(mangled, tree->value, sizeof(mangled) - 1);
            mangled[sizeof(mangled) - 1] = '\0';
            for (char* p = mangled; *p; p++) { if (*p == '.') *p = '_'; }
            if (strcmp(mangled, func_name) == 0) is_match = 1;
        }
    }
    if (is_match) {
        // This is a call to our function - propagate argument types to parameters
        int arg_count = tree->child_count;
        for (int i = 0; i < arg_count && i < param_count; i++) {
            ASTNode* arg = tree->children[i];
            ASTNode* param = func_def->children[i];

            if (arg && param &&
                (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE)) {
                // If parameter type is unknown and argument type is known, propagate it
                if ((!param->node_type || param->node_type->kind == TYPE_UNKNOWN) &&
                    arg->node_type && arg->node_type->kind != TYPE_UNKNOWN) {
                    if (param->node_type) free_type(param->node_type);
                    param->node_type = clone_type(arg->node_type);
                    changed++;
                }
            }
        }
    }

    // Recursively process all children
    for (int i = 0; i < tree->child_count; i++) {
        changed += propagate_call_types_in_tree(tree->children[i], func_name, func_def, param_count);
    }
    return changed;
}

// Propagate types from function call sites to function definitions.
// Returns the number of type updates made (0 = stable, nothing changed).
int propagate_function_call_types(ASTNode* program, SymbolTable* table) {
    (void)table;  // Unused for now
    if (!program) return 0;
    int total_changed = 0;

    // Find all function calls and match them with definitions
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* node = program->children[i];
        if (!node) continue;

        // Look for function definitions
        if ((node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION) && node->value) {
            const char* func_name = node->value;
            int param_count = node->child_count - 1; // Last child is body

            // Search every other top-level node (including other function bodies)
            for (int j = 0; j < program->child_count; j++) {
                if (i != j) {
                    total_changed += propagate_call_types_in_tree(program->children[j], func_name, node, param_count);
                }
            }
        }
    }
    return total_changed;
}

// Infer return types for all functions
// Scan AST for multi-return statements and fill UNKNOWN tuple elements
static void merge_tuple_returns(ASTNode* node, Type* merged) {
    if (!node || !merged || merged->kind != TYPE_TUPLE) return;
    if (node->type == AST_RETURN_STATEMENT && node->child_count > 1 &&
        node->child_count == merged->tuple_count) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode* val = node->children[i];
            if (merged->tuple_types[i]->kind == TYPE_UNKNOWN) {
                if (val->node_type && val->node_type->kind != TYPE_UNKNOWN) {
                    free_type(merged->tuple_types[i]);
                    merged->tuple_types[i] = clone_type(val->node_type);
                } else if (val->type == AST_LITERAL && val->value) {
                    // Infer literal type. Same fix as the matching site
                    // in infer_return_type_impl: start is_num = 0 so the
                    // empty-string literal "" doesn't decay to TYPE_INT.
                    // See bug #2 in tests/integration/multi_return_destructure_chain/.
                    int is_num = 0;
                    for (const char* p = val->value; *p; p++) {
                        if (*p >= '0' && *p <= '9') { is_num = 1; }
                        else if (*p != '-' && *p != '.')      { is_num = 0; break; }
                    }
                    free_type(merged->tuple_types[i]);
                    merged->tuple_types[i] = create_type(is_num ? TYPE_INT : TYPE_STRING);
                }
            }
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        merge_tuple_returns(node->children[i], merged);
    }
}

void infer_function_return_types(ASTNode* program, SymbolTable* table) {
    if (!program) return;

    for (int i = 0; i < program->child_count; i++) {
        ASTNode* node = program->children[i];
        if (!node || (node->type != AST_FUNCTION_DEFINITION && node->type != AST_BUILDER_FUNCTION)) continue;

        // Infer return type from return statements. Also re-infer when
        // node_type is VOID, because earlier iterations may have guessed
        // VOID for a function whose body's `return v` referenced a local
        // whose type wasn't yet resolvable (e.g. destructured from a
        // call to a function whose own return type was still UNKNOWN).
        // Same logic for partially-resolved tuples: a TUPLE(string, UNKNOWN)
        // typed in iteration N may be refinable in iteration N+1 once
        // more function signatures are known. Without firing on
        // partially-resolved tuples the early guess sticks and codegen
        // emits the UNKNOWN slot as int. Bug #3 in
        // tests/integration/multi_return_destructure_chain/.
        int body_index = node->child_count - 1;
        if (body_index >= 0 && body_index < node->child_count) {
            int has_unknown_tuple_slot = 0;
            if (node->node_type && node->node_type->kind == TYPE_TUPLE) {
                for (int s = 0; s < node->node_type->tuple_count; s++) {
                    Type* slot = node->node_type->tuple_types[s];
                    if (!slot || slot->kind == TYPE_UNKNOWN) {
                        has_unknown_tuple_slot = 1;
                        break;
                    }
                }
            }
            if (!node->node_type ||
                node->node_type->kind == TYPE_UNKNOWN ||
                node->node_type->kind == TYPE_VOID ||
                has_unknown_tuple_slot) {
                Type* return_type = infer_return_type_from_body(node->children[body_index], table);
                if (return_type) {
                    if (node->node_type) free_type(node->node_type);
                    node->node_type = return_type;
                } else if (!node->node_type) {
                    // No explicit return, assume void (only on first pass).
                    free_type(node->node_type);
                    node->node_type = create_type(TYPE_VOID);
                }
            }
            // If return type is a tuple with UNKNOWN elements, merge from all returns
            if (node->node_type && node->node_type->kind == TYPE_TUPLE) {
                merge_tuple_returns(node, node->node_type);
            }
        }
    }
}

// Classic truncation-on-assign: inside a function returning `ptr`, the
// pattern
//     out = 0          // inferred as int (0 is a valid int literal)
//     if cond { out = some_ptr_call() }
//     return out       // emitted as `return out;` where out is `int`
// generates `int out = 0;` in C. On 64-bit targets the later ptr-typed
// write truncates to 32 bits; the caller then dereferences a junk
// pointer. The language accepts `0` as either int or ptr, so the
// declaration's type should widen when a subsequent assignment gives
// it a ptr value. This pass walks each function body once, finds the
// offending pattern, and widens both the decl's node_type and the
// symbol-table entry so codegen emits `void* out = NULL;`.
//
// Conservative on purpose:
//   - only triggers when the declaration's initializer is the literal 0
//     (not any int expression) — widening arbitrary int locals to ptr
//     would hide real type mismatches.
//   - only looks at assignments in the same block (and its children);
//     a ptr write in a later branch still counts.
static int is_literal_zero(ASTNode* init) {
    if (!init || init->type != AST_LITERAL || !init->value) return 0;
    const char* p = init->value;
    if (*p == '+' || *p == '-') p++;
    if (*p == '\0') return 0;
    for (; *p; p++) if (*p != '0') return 0;
    return 1;
}

static int assignment_to_is_ptr(ASTNode* node, const char* name) {
    if (!node) return 0;
    if (node->type == AST_ASSIGNMENT && node->child_count >= 2) {
        ASTNode* lhs = node->children[0];
        ASTNode* rhs = node->children[1];
        if (lhs && lhs->value && strcmp(lhs->value, name) == 0 &&
            rhs && rhs->node_type && rhs->node_type->kind == TYPE_PTR) {
            return 1;
        }
    }
    // Reassignment via AST_VARIABLE_DECLARATION with the same name is
    // how Python-style reassigns lower in practice; RHS is children[0].
    if (node->type == AST_VARIABLE_DECLARATION && node->value &&
        strcmp(node->value, name) == 0 && node->child_count > 0) {
        ASTNode* rhs = node->children[0];
        if (rhs && rhs->node_type && rhs->node_type->kind == TYPE_PTR) {
            return 1;
        }
    }
    for (int i = 0; i < node->child_count; i++) {
        if (assignment_to_is_ptr(node->children[i], name)) return 1;
    }
    return 0;
}

static void widen_ptr_assigned_locals_in_block(ASTNode* block, SymbolTable* symbols) {
    if (!block) return;
    if (block->type == AST_BLOCK) {
        for (int i = 0; i < block->child_count; i++) {
            ASTNode* stmt = block->children[i];
            if (!stmt) continue;
            if (stmt->type == AST_VARIABLE_DECLARATION &&
                stmt->value && stmt->child_count > 0 &&
                stmt->node_type && stmt->node_type->kind == TYPE_INT &&
                is_literal_zero(stmt->children[0])) {
                // Scan later siblings (including nested blocks) for a
                // ptr-typed write to this name.
                int widen = 0;
                for (int j = i + 1; j < block->child_count; j++) {
                    if (assignment_to_is_ptr(block->children[j], stmt->value)) {
                        widen = 1;
                        break;
                    }
                }
                if (widen) {
                    free_type(stmt->node_type);
                    stmt->node_type = create_type(TYPE_PTR);
                    // Also tag the initializer so later code treats the
                    // literal 0 as a ptr-slot null (codegen reads this
                    // when emitting the `= ...` for the declaration).
                    free_type(stmt->children[0]->node_type);
                    stmt->children[0]->node_type = create_type(TYPE_PTR);
                    if (symbols) {
                        Symbol* sym = lookup_symbol(symbols, stmt->value);
                        if (sym) {
                            if (sym->type) free_type(sym->type);
                            sym->type = create_type(TYPE_PTR);
                        }
                    }
                }
            }
        }
    }
    // Recurse into children so nested blocks (if/while/for bodies) get
    // the same treatment for their own locals.
    for (int i = 0; i < block->child_count; i++) {
        widen_ptr_assigned_locals_in_block(block->children[i], symbols);
    }
}

static void widen_ptr_assigned_locals(ASTNode* program, SymbolTable* symbols) {
    if (!program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* node = program->children[i];
        if (!node) continue;
        if (node->type != AST_FUNCTION_DEFINITION &&
            node->type != AST_BUILDER_FUNCTION) continue;
        // Only widen inside ptr-returning functions — widening in an
        // int-returning function would change the return type and
        // break code that genuinely wanted an int accumulator.
        if (!node->node_type || node->node_type->kind != TYPE_PTR) continue;
        for (int c = 0; c < node->child_count; c++) {
            ASTNode* child = node->children[c];
            if (child && child->type == AST_BLOCK) {
                widen_ptr_assigned_locals_in_block(child, symbols);
            }
        }
    }
}

// Main inference function
int infer_all_types(ASTNode* program, SymbolTable* table) {
    if (!program) return 0;
    
    InferenceContext* ctx = create_inference_context(table);
    
    // Phase 1: Collect constraints from the entire program
    collect_constraints(program, ctx);
    
    // Phase 2: Solve basic constraints
    int success = solve_constraints(ctx);
    
    // Phase 3-5: Interleaved propagation + constraint solving.
    // Each pass: propagate call-site types into parameter definitions,
    // re-infer function return types, sync those return types into the
    // global function-symbol table (so call sites in other functions
    // resolve correctly on the next pass), then re-collect and re-solve.
    // This handles deep call chains (a->b->c->d) where each level needs
    // one propagation pass followed by one constraint-solve pass.
    //
    // The return-type sync inside the loop (rather than only after) is
    // load-bearing for tuple-destructured callers: `target = some_call()`
    // where some_call() returns `(ptr, string)`. Without per-iteration
    // sync, `some_call()`'s call-site node_type stays UNKNOWN until phase
    // 6, after which no further constraint pass runs to set `target`'s
    // type from the resolved tuple slot.
    for (int pass = 0; pass < MAX_INFERENCE_ITERATIONS; pass++) {
        int changed = propagate_function_call_types(program, table);

        // Refresh function return types. Crucially, on iteration N this
        // produces a return type that uses iteration N-1's func_sym info
        // for any cross-function inference (e.g. resolving a destructure
        // local from the called function's tuple return). To converge,
        // we re-publish those return types onto the function symbols and
        // require child->node_type to be a *more specific* type (i.e.
        // not VOID/UNKNOWN) before incrementing `changed` — otherwise a
        // function whose body genuinely returns void would loop forever
        // re-syncing the same VOID.
        infer_function_return_types(program, table);
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* child = program->children[i];
            if (!child || !child->value || !child->node_type) continue;
            if (child->type != AST_FUNCTION_DEFINITION &&
                child->type != AST_BUILDER_FUNCTION) continue;
            if (child->node_type->kind == TYPE_UNKNOWN ||
                child->node_type->kind == TYPE_VOID) continue;
            Symbol* func_sym = lookup_symbol(table, child->value);
            if (!func_sym) continue;
            int sync = 0;
            if (!func_sym->type ||
                func_sym->type->kind == TYPE_UNKNOWN ||
                func_sym->type->kind == TYPE_VOID) {
                sync = 1;
            } else if (func_sym->type->kind == TYPE_TUPLE &&
                       child->node_type->kind == TYPE_TUPLE &&
                       func_sym->type->tuple_count == child->node_type->tuple_count) {
                // Sync only when the child's tuple is *strictly* more
                // specific — fewer UNKNOWN slots. Without strictness,
                // syncing TUPLE(s, UNKNOWN) → TUPLE(s, UNKNOWN) loops
                // forever and exhausts MAX_INFERENCE_ITERATIONS.
                int sym_unknown = 0, child_unknown = 0;
                for (int s = 0; s < func_sym->type->tuple_count; s++) {
                    Type* a = func_sym->type->tuple_types[s];
                    Type* b = child->node_type->tuple_types[s];
                    if (!a || a->kind == TYPE_UNKNOWN) sym_unknown++;
                    if (!b || b->kind == TYPE_UNKNOWN) child_unknown++;
                }
                if (child_unknown < sym_unknown) sync = 1;
            }
            if (sync) {
                if (func_sym->type) free_type(func_sym->type);
                func_sym->type = clone_type(child->node_type);
                changed++;
            }
        }

        free_inference_context(ctx);
        ctx = create_inference_context(table);
        collect_constraints(program, ctx);
        success = solve_constraints(ctx);

        if (changed == 0) break;
    }

    // Phase 6: Infer function return types (now that return expressions have types)
    infer_function_return_types(program, table);

    // Phase 7: Widen `out = 0` locals to ptr when a later assignment in
    // the same ptr-returning function is ptr-typed. See
    // widen_ptr_assigned_locals above for the rationale — prevents
    // silent pointer truncation on 64-bit targets.
    widen_ptr_assigned_locals(program, table);

    free_inference_context(ctx);

    return success;
}
