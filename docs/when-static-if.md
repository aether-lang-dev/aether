# Compile-time `when` / static-if

`when` is a compile-time `if`. The condition is evaluated by the compiler,
and **only the selected arm is type-checked and emitted**, the other arm
parses but is pruned before any later phase ever sees it. This lets `.ae`
source fork by platform or build capability *visibly in Aether* instead of
pushing `#ifdef` into the generated C.

```aether,fragment
when target.os == "windows" {
    extern win_only(handle: ptr) -> int
    poll(h: ptr) -> int { return win_only(h) }
} else {
    poll(h: ptr) -> int { return posix_poll(h) }
}
```

On a non-Windows host the `windows` arm, extern binding and all, is
discarded before type-checking. `win_only` never has to exist, type-check,
or link. That is the whole point: platform-specific externs and bindings
are gated cleanly, with no dead symbol leaking into the build.

## Surface

```
when <const-condition> {
    <statements or top-level declarations>
} else {
    <statements or top-level declarations>
}
```

- Works as a **statement** inside a function body (the arms hold
  statements), and at **top level** around declarations, functions,
  externs, imports, structs (the arms hold declarations). The two forms
  are distinguished by where the `when` appears; you write them the same
  way.
- The `else` is optional. When the condition is false and there is no
  `else`, the `when` contributes nothing.
- Arms chain with `else when`:

  ```aether
  when target.os == "darwin" {
      ...
  } else when target.os == "linux" {
      ...
  } else {
      ...
  }
  ```

- The arm braces are a **compile-time grouping**, not a runtime scope: at
  top level the surviving arm's declarations are spliced directly into the
  program, and in a function body the surviving arm's statements run in the
  enclosing scope (no extra `{ }` block in the generated C).

`when` is also the keyword for Erlang-style function-clause guards
(`classify(x) when x < 0 -> ...`). Those are unaffected: a guard `when`
only appears *after* a clause's parameter list, never at the head of a
statement or declaration, so the two uses never collide.

## Supported condition forms

The condition must reduce to a compile-time constant boolean. Supported
building blocks:

| Form | Example |
|---|---|
| OS equality / inequality | `target.os == "linux"`, `target.os != "windows"` |
| Architecture equality / inequality | `target.arch == "x86_64"`, `target.arch != "arm"` |
| String-literal equality between two constants | `"a" == "b"` |
| Boolean literal | `when true { ... }`, `when false { ... }` |
| Numeric literal (nonzero is true) | `when 1 { ... }` |
| Negation | `!(target.os == "windows")` |
| Conjunction / disjunction | `target.os == "darwin" \|\| target.os == "linux"` |

These compose freely with `&&`, `||`, and `!`.

A condition that is **not** a compile-time constant (e.g. a runtime
variable, an unsupported call, a comparison the folder doesn't recognise)
is a **hard compile error**, the build fails with a message listing the
supported forms. `when` never guesses an arm; if the compiler can't decide
the condition at compile time, it stops.

### `target.os` and `target.arch`

`target.os` and `target.arch` are compiler-provided compile-time string
constants describing the build target. They are sourced from the **same C
preprocessor macros** the runtime's `os.platform()` uses (see
`os_platform_raw` in `std/os/aether_os.c`), so the canonical names match
across the toolchain. Note the "target" here is the host that built
`aetherc`: `target_os_string` / `target_arch_string` in
`compiler/codegen/optimizer.c` return values baked from the C preprocessor
macros active when the compiler was compiled. `ae build --target wasm` does
cross-compile the generated C to WebAssembly via `emcc`, but it does not
change these constants: under `--target wasm` a `when target.os == ...`
still reports the host, not `"wasm"`, exactly as `os.platform()` and
`select()` also key off the host.

| Constant | Canonical values |
|---|---|
| `target.os` | `"windows"`, `"darwin"` (macOS), `"linux"`, `"freebsd"`, `"openbsd"`, `"netbsd"`, `"dragonfly"`, `"solaris"`, `"wasm"`, `"unknown"` |
| `target.arch` | `"x86_64"`, `"aarch64"`, `"x86"`, `"arm"`, `"riscv64"`, `"ppc64"`, `"wasm"`, `"unknown"` |

Note macOS is `"darwin"` (matching `os.platform()` and Go/Rust's
convention), not `"macos"`. `target.os` / `target.arch` are only meaningful
inside a `when` condition.

## Semantics: only the selected arm is type-checked and emitted

The condition is evaluated by a pre-typecheck pass
(`resolve_when_statements`, in `compiler/codegen/optimizer.c`, invoked from
`compiler/aetherc.c` between parse and type-check). That pass:

1. Const-evaluates each `when` condition.
2. Replaces the `when` node, in place, with the contents of its selected
   arm (splicing top-level declarations to top level, statement arms into
   the enclosing block).
3. Frees the unselected arm entirely.

Because this runs **before** type-checking and symbol collection, the
unselected arm is never type-checked, never collects symbols, and is never
handed to codegen. An undefined function or a platform-only `extern` in the
dead arm is therefore harmless, it is gone before anything can object to
it. This is what makes `when` sound for gating platform bindings, rather
than a cosmetic `if` that still requires the dead branch to compile.

## How it complements `select()` and `--emit=lib --with=`

- **`select()` is a *runtime* platform switch.** It lowers to an `#ifdef`
  chain in the generated C, choosing a *value* per platform at the C-compile
  step (`select(linux: a, macos: b, other: c)`). Every arm of a `select()`
  must type-check, because the value is chosen in C, not in Aether. Use
  `select()` to pick a constant per platform; use `when` to fork *which
  code exists at all*, to gate an entire extern, function, or import that
  only makes sense on one platform. See
  [`named-args-and-select.md`](named-args-and-select.md).

- **`--emit=lib --with=`** governs which *capabilities* (fs / net / os) a
  compiled library is allowed to use. `when` operates one level up: it
  decides *which declarations are in the program* before the capability
  gate runs. The pruning pass runs before the `--emit=lib` import gate, so
  an `import std.net` gated behind a `when` arm that the target doesn't
  select is removed before the gate inspects the import set, a platform
  whose arm doesn't pull in `std.net` won't trip the net-capability check.
  See [`emit-lib.md`](emit-lib.md).

- **`const` / compile-time constant folding** ([`compile-time-eval.md`](compile-time-eval.md))
  is the value-level analogue: it folds constant *expressions* to literals.
  `when` is the statement/declaration-level analogue: it folds a constant
  *condition* to a choice of arm. The same capability-gate discipline
  applies, the `when` condition evaluator is a small, audited folder
  (OS/arch strings, bool/numeric literals, `== != && || !`), not a general
  interpreter, so resolving a `when` performs no I/O.

## Where it lives

- **Parsing:** `parse_when_statement` in `compiler/parser/parser.c`, reached
  from `parse_statement` (statement form) and `parse_top_level_decl`
  (top-level form). AST node kind `AST_WHEN_STATEMENT`
  (`compiler/ast.h`), laid out like `AST_IF_STATEMENT`:
  `children[0]` = condition, `children[1]` = then-arm, `children[2]` =
  optional else-arm.
- **Condition evaluation + pruning:** `when_eval_condition`,
  `target_os_string` / `target_arch_string`, and `resolve_when_statements`
  in `compiler/codegen/optimizer.c`.
- **Driver wiring:** `compiler/aetherc.c` calls `resolve_when_statements`
  after module merge and the `@derive` pass, and before the `--emit=lib`
  import gate and `typecheck_program`.
