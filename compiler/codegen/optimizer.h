#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "../ast.h"

// Compiler optimization passes

// Constant folding: evaluate constant expressions at compile time
// Example: 2 + 3 * 4 -> 14
ASTNode* optimize_constant_folding(ASTNode* node);

// Dead code elimination: remove unreachable code
// Example: if (true) { A } else { B } -> { A }
ASTNode* optimize_dead_code(ASTNode* node);

// Tail call optimization: convert tail-recursive calls to loops
// Example: factorial(n, acc) -> while loop
ASTNode* optimize_tail_calls(ASTNode* node);

// Apply all optimizations
ASTNode* optimize_ast(ASTNode* node);

// Compile-time `when` / static-if resolution (issue #483).
// Evaluates each `when` condition at compile time and prunes the AST to the
// selected arm, IN PLACE, before type-checking. The unselected arm is freed
// and never type-checked or emitted. `target.os` / `target.arch` are
// compiler-provided compile-time string constants for the build target.
// Returns 0 on success; non-zero if any `when` condition was not a
// compile-time constant (the driver aborts the build).
int resolve_when_statements(ASTNode* program);

// Optimization statistics
typedef struct {
    int constants_folded;
    int dead_code_removed;
    int tail_calls_detected;
    int series_loops_collapsed;  // constant-addend series loops replaced with closed-form expressions
    int linear_loops_collapsed;  // linear counter-sum loops collapsed via triangular-number formula
} OptimizationStats;

extern OptimizationStats global_opt_stats;

void reset_optimization_stats(void);
void print_optimization_stats(void);

#endif // OPTIMIZER_H

