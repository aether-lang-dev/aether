# A statement call that returns a destructible struct and takes a fresh-string argument emits `void` into its struct temporary (C compile error, since 0.792)

**From:** aeo (2026-10-10) · **Where it bit:** two aeo specs
(`spec_share`, `spec_protocol_delegate`) stopped compiling on 0.793 through
0.801 (0.792 was never released). They had compiled on 0.791. The line was
the ordinary std.spec chain
`spec.expect_str(f(x)).to_equal_str(g(y))`, where `g` returns a fresh heap
string. aeo now binds `g(y)` to a local first. This ask is for the compiler.

## Repro (no std.spec needed)

```aether
import std.string

struct Box {
    v: string
}

fresh(s: string) -> string {
    return string.concat(s, "")
}

box(s: string) -> Box {
    return Box { v: string.concat(s, "") }
}

chk(b: Box, s: string) -> Box {
    if string.equals(b.v, s) == 1 {
        println("eq")
    }
    return b
}

main() {
    chk(box(fresh("a")), fresh("a"))
}
```

| release | result |
|---|---|
| 0.778.0, 0.791.0 | builds, prints `eq` |
| 0.793.0 … 0.803.0 (and 0.799.0 on FreeBSD 15) | `error: assigning to 'Box' (aka 'struct Box') from incompatible type 'void'` from the C compiler |

Variants on 0.801.0:

- `b = box(fresh("a"))` then `chk(b, fresh("a"))`: **still fails**. The
  outer call alone is enough. It needs a fresh-string argument and a
  struct result in statement position.
- `w = fresh("a")` then `chk(box(fresh("a")), w)`: builds.
- `x = chk(box(fresh("a")), fresh("a"))` (result bound, not discarded):
  builds.

## The emitted C (0.801.0, `--emit-c`)

```c
{ Box _ae_stmp0 = {0}; Box _ae_stmp1 = {0};
  ({ __auto_type _eo0 = ((_ae_stmp1 = ({ char* _ad_0 = (char*)(fresh("a"));
         Box _ad_r = box(_ad_0); aether_heap_str_free(_ad_0); _ad_r; })));
     (_ae_stmp0 = ({ char* _ad_1 = (char*)(fresh("a"));
         Box _ad_r = chk(_eo0, _ad_1); aether_heap_str_free(_ad_1);
         (void)_ad_r; })); });
  Box_destroy(&_ae_stmp0); Box_destroy(&_ae_stmp1); }
```

The outer call's argument wrap ends in `(void)_ad_r;`. That is the
discarded-statement form from `compiler/codegen/codegen_expr.c:591`
(`w->discarded ? "(void)_ad_r; })" : "_ad_r; })"`), written so that
`fs.delete("${dir}/f")` doesn't warn -Wunused-value. But the same statement
is also wrapped as a statement struct temporary
(`_ae_stmp0 = (...)`, then `Box_destroy(&_ae_stmp0)`), and that needs the
value. Those temporaries were introduced in 6632ad3a ("statement struct
temporaries are destroyed"), which falls in 0.791..0.793. So the call is
both "discarded" and "captured for destruction", and the two wraps disagree.

## Ask

When a discarded call is captured into a statement struct temporary, its
argument wrap should yield `_ad_r`, not `(void)_ad_r`. One option is to clear
`discarded` for a call the statement-temporary pass captures. The destroy then
frees the result, and no -Wunused-value fires because the value is assigned.
A regression test that is the repro above, plus the std.spec form, would
cover aeo's case.
