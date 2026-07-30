# `ae bindgen consts`, importing C macro constants

Ports of C codebases hand-copy flag macros, log levels, error codes and
size constants into `.ae` files, and every copied value drifts silently
when the C header changes. `ae bindgen consts` generates that file from
the header instead.

```sh
ae bindgen consts sentinel.h -o lib/sentinel/module.ae
ae bindgen consts redis.h -I ../src --match OBJ_ -o lib/objflags/module.ae
```

```aether
import sentinel

main() {
    flags = sentinel.SRI_S_DOWN | sentinel.SRI_O_DOWN
    if flags == sentinel.SRI_FAILOVER { ... }   // agrees with C exactly
}
```

## What imports

Object-like macros whose full expansion is one of:

| Expansion | Example | Generated |
|---|---|---|
| integer constant expression | `(1<<4)`, `(A\|B)`, `512ll*1024*1024`, `-1`, `'x'` | `const NAME = <folded value>` |
| string literal(s) | `"7.4.0"`, `"a" "b"` | `const NAME = "ab"` |
| float literal | `0.75`, `1e6` | `const NAME = 0.75` |

The C preprocessor does the work: candidates come from `cc -E -dM`, and a
probe file run through `cc -E` resolves nested macros, so the values are
exactly what a C compilation unit would see. Nothing is executed; both
stages are preprocessor-only.

## What does not import

Function-like macros, casts (`((void*)0)`), and anything that is not a
scalar constant expression. Skipped macros are listed in a comment at the
end of the generated file so the omission is visible in review, never
silent. Names in the reserved namespace (leading underscore) are excluded
up front.

## Flags

| Flag | Meaning |
|---|---|
| `-I <dir>` | Include directory for the preprocessor (repeatable) |
| `--match <PREFIX>` | Import only macros whose name starts with PREFIX |
| `-o <out.ae>` | Output file; stdout when omitted |

## Workflow notes

The generated file starts with a `do not edit` header and is meant to be
regenerated whenever the C header changes; check it in like any other
source so diffs show constant changes explicitly. The generated module
exports every imported const, so `import <name>` then `<name>.CONST`
works with the standard package layout (`lib/<name>/module.ae`).
