/* Missing-return check (#2684).
 *
 * A function whose result is not void must return a value on every path. A
 * path that reaches the end of the body instead falls off the end of a
 * non-void C function, which is undefined behaviour: the caller reads
 * whatever the return register held (a string printed "(null)" on MinGW).
 *
 * The check is over the type-checked program, so a `match` knows the type it
 * matches on. It reports each function, clause and closure whose end control
 * can reach, at the closing brace, naming the statement that lets control
 * through.
 */

#ifndef AETHER_MISSING_RETURN_H
#define AETHER_MISSING_RETURN_H

#include "../ast.h"

/* Reports every definition in `program` whose result is not void and whose
 * end control can reach. Returns the number of errors reported. */
int check_missing_returns(ASTNode* program);

#endif /* AETHER_MISSING_RETURN_H */
