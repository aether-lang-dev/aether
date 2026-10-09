/* Trusted names in `sandbox.enforce(perms, foo, bar) { block }`.
 *
 * Inside the block every sandbox-checked operation is held to `perms`. The
 * names after `perms` are exempt: a call to one of them, written in the
 * block, runs with the sandbox depth the `enforce` was entered at, i.e. with
 * the authority of the code that wrote the `enforce`. A name is a function
 * (`foo`) or an imported module (`db`, covering every `db.x(...)`).
 *
 * The pass runs at the start of type checking. It validates the names, marks
 * the calls in the block (lexically: nested blocks and closures included, the
 * innermost `enforce` winning), gives each such `enforce` a site number, and
 * drops the extra arguments so the call type-checks as the plain
 * two-argument `enforce`. Codegen then routes each marked call through a
 * generated wrapper with the target's exact signature, so the call's
 * arguments are still evaluated inside the sandbox; only the trusted
 * function's own body runs at the lowered depth. */
#ifndef AETHER_SANDBOX_TRUST_H
#define AETHER_SANDBOX_TRUST_H

#include "../ast.h"

/* Returns the number of errors reported (through type_error). */
int sandbox_trust_pass(ASTNode* program);

/* The site a marked call belongs to, or 0 when the call is not trusted. */
int sandbox_trust_site_of_call(const ASTNode* call);

/* The site number of an `enforce` call that has trusted names, else 0. */
int sandbox_trust_site_of_enforce(const ASTNode* call);

/* How many marked calls there are, and the i-th of them (for codegen to
 * collect the wrappers it must emit). */
int sandbox_trust_call_count(void);
const ASTNode* sandbox_trust_call_at(int i);

/* #2613: forget the marked calls and sites inside `node`, a definition the
 * prune drops after type checking (module_sweep_unreachable). */
void sandbox_trust_forget_within(const ASTNode* node);

#endif
