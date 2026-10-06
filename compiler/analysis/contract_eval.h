#ifndef CONTRACT_EVAL_H
#define CONTRACT_EVAL_H

/* Compile-time contract predicate evaluation (see docs/contract-folding.md).
 *
 * One evaluator, two consumers:
 *
 *   - The TYPECHECKER diagnoses: a predicate that is decidably FALSE — at a
 *     function definition, or at a call site with the actual arguments
 *     substituted for the parameters — is a compile error. `divide(10, 0)`
 *     against `b: int where b != 0` fails the build instead of panicking at
 *     run time.
 *   - CODEGEN elides: a predicate that is decidably TRUE at the definition
 *     emits no runtime check (this replaced codegen's older, double-based
 *     `try_fold_predicate`, whose 2^53 precision loss could mis-fold exact
 *     int64 comparisons).
 *
 * The evaluator is deliberately narrow, and two of its refusals are POLICY,
 * not laziness:
 *
 *   - It never evaluates CALLS — not even the pure-conversion whitelist that
 *     const initializers accept. The const layer is whitelist-only precisely
 *     so compile-time evaluation can't synthesize std.fs / std.net calls past
 *     the --emit=lib capability gate (see the block comment above
 *     is_const_expression in typechecker.c). Contract folding inherits that
 *     boundary.
 *   - Integer arithmetic is int64, not double. A diagnostic that mis-folds
 *     `x == 9007199254740993` because both sides rounded to 2^53 would error
 *     on correct code, which is the one way this feature can fail badly.
 *
 * Everything outside the supported subset is CONTRACT_UNKNOWN, and UNKNOWN
 * always means "keep the runtime check, say nothing".
 */

#include <stddef.h>
#include <stdint.h>
#include "../ast.h"

typedef enum {
    CONTRACT_UNKNOWN = 0,   /* not decidable at compile time */
    CONTRACT_FALSE,
    CONTRACT_TRUE
} ContractTri;

/* Evaluation environment. `names[i]`/`args[i]` bind a callee parameter name
 * to the caller's argument expression (call-site folding); an empty binding
 * set gives definition-site folding. `program` (may be NULL) enables
 * resolution of top-level `const` declarations and enum-member constants.
 *
 * Argument expressions are themselves evaluated WITHOUT the parameter
 * bindings — they live in the caller's scope, where the callee's parameter
 * names mean nothing. */
#define CONTRACT_ENV_MAX_PARAMS 32
typedef struct {
    const char* names[CONTRACT_ENV_MAX_PARAMS];
    ASTNode*    args[CONTRACT_ENV_MAX_PARAMS];
    int         count;
    ASTNode*    program;
} ContractEnv;

/* Tri-state evaluation of a contract predicate.
 *
 * Short-circuit verdicts are deliberately asymmetric: `<unknown> && false`
 * decides FALSE (a FALSE verdict never skips any runtime evaluation — it
 * becomes a build error or falls through to the runtime check), but
 * `<unknown> || true` stays UNKNOWN, because a TRUE verdict makes codegen
 * elide the whole check and the unknown operand may be a side-effectful
 * call the runtime would have evaluated. `true || <anything>` is TRUE —
 * that mirrors runtime short-circuit order exactly. Rationale in the
 * implementation. */
ContractTri contract_eval_predicate(ASTNode* pred, ContractEnv* env);

/* Source-like rendering of a predicate expression, for diagnostics and for
 * the runtime panic message. Moved here from codegen_stmt.c so the
 * typechecker's compile-time error and codegen's runtime panic render the
 * same violation identically. */
typedef struct { char* buf; size_t cap; size_t off; } ContractStr;
void contract_str_terminate(ContractStr* s);
void contract_sprint_expr(ContractStr* s, ASTNode* e);

/* The int64 value of a compile-time integer expression (a literal, a
 * top-level `const`, an enum member, or arithmetic over them), resolving
 * names against `program`. Returns 1 and sets *out when the value is an
 * integer known at compile time; 0 otherwise (a float, a bool, a call,
 * anything unknown). Used by codegen to place a `switch` case in a
 * threaded-dispatch table (#2378). */
int contract_eval_int64(ASTNode* e, ASTNode* program, int64_t* out);

#endif /* CONTRACT_EVAL_H */
