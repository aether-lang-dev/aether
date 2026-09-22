# A heap string captured+reassigned by a closure is freed on return

**From:** aeb on CachyOS (2026-09-22) · **Reproduced on:** 0.699.0 and **0.706.0**
(aeb's current `AETHER_PIN`/`AETHER_FETCH`)

A local `string` that a closure captures and reassigns holds the right value
inside the function that owns it, and is **freed before the caller reads the
returned value**. The callee sees `"src/b.tssrc/a.ts"`; the caller, one line
later, sees six bytes of junk.

The same function shape returning a string that was *never* captured is fine,
which is what makes this a capture-path escape-analysis miss rather than a
general return-escape problem.

## Reproducer (`probe4.ae`) — with its own control

```aether
import std.io
import std.fs
import std.string

// The captured accumulator is correct INSIDE the function...
captured(root: string) -> string {
    out = ""
    _n, _e = fs.walk(root, |p: string, kind: int, depth: int| {
        if kind != 1 { return 0 }
        out = string.concat(out, p)
        return 0
    })
    println("  inside callee: len=${string.length(out)} [${out}]")
    return out
}

// CONTROL: same shape, but the returned string was never captured.
not_captured(root: string) -> string {
    seen = 0
    _n, _e = fs.walk(root, |p: string, kind: int, depth: int| {
        if kind == 1 { seen = seen + 1 }
        return 0
    })
    out = string.concat("files=", string.from_int(seen))
    println("  inside callee: len=${string.length(out)} [${out}]")
    return out
}

main() {
    r1 = captured("src")
    println("  in caller    : len=${string.length(r1)} [${r1}]")
    r2 = not_captured("src")
    println("  in caller    : len=${string.length(r2)} [${r2}]")
}
```

With `src/a.ts` and `src/b.ts` present:

```
  inside callee: len=16 [src/b.tssrc/a.ts]
  in caller    : len=6  [W<junk>U]        <-- captured
  inside callee: len=7  [files=2]
  in caller    : len=7  [files=2]          <-- control, correct
```

Length 6 with varying bytes run to run: a freed buffer being read back.

## What narrows it

Four probes, all on 0.706.0, same file:

| shape | result |
|-------|--------|
| captured accumulator, read **inside** the callee only | correct |
| captured accumulator, **returned and read by the caller** | **freed** |
| same accumulator but the value assigned is a literal, not the callback parameter | **freed** (so it is not about `p`'s lifetime) |
| captured **int** accumulator, returned | correct (so it is heap-string-specific) |
| returned string never captured by any closure | correct |

An earlier probe that only ever printed inside the callee looked healthy — the
value really is intact there. Anything that observes the *returned* value sees
the free. That is worth knowing for whoever writes the regression test: a test
that asserts inside the callee passes against the bug.

`fs.walk` is just the closure-taking function nearest to hand; nothing in the
above depends on it being a filesystem walk.

## Where it bit

`aeb/lib/bldr/module.ae`'s `_walk_collect(root, ext, name_excludes,
path_excludes)` — the shared "collect every source file under this tree"
helper — is exactly the `captured` shape, and returns the accumulator. Its
callers write that return straight to disk:

```aether
_ew_af = io.write_file(argfile, bldr._walk_collect(source_dir, ".ts", ".d.ts", "/node_modules/"))
```

so `target/<mod>/tsc_sources.txt` was six bytes of junk. That file is the
source set for the content-addressed cache key, so **every `lib/ts` and
`lib/dotnet` cache key was computed over garbage**. It also reached `/bin/sh`,
because the next stage sorts the argfile through the shell:

```
printf '%s\n' '<junk>' | sort
sh: -c: line 2: unexpected EOF while looking for matching `''
```

which is how it was noticed at all — an unbalanced quote in a build log, three
layers away from the cause. Found while running aeb's `itests/nx-examples`.

## Workaround, if it helps triage

Replacing the reassigned accumulator with a `std.strbuilder` fixes it, because
the captured variable is then a `ptr` that is never reassigned, so the
heap-string tracker is not involved:

```aether
sb = strbuilder.new(4096)
_n, _werr = fs.walk(root, |p: string, kind: int, depth: int| {
    if strbuilder.length(sb) > 0 { strbuilder.append(sb, "\n") }
    strbuilder.append(sb, p)
    return 0
})
return strbuilder.finish(sb)
```

aeb has taken that workaround (it is the better shape for a tree walk anyway),
so aeb is not blocked. Filing it because the general form — "accumulate into a
captured local, return it" — is an ordinary thing to write, and it fails
silently with plausible-looking short output rather than crashing.

## Possibly related

`asks/closure-locals-unified-with-outer-block-names-emit-undeclared-c.md` and
`asks/closure-promotion-still-live-on-0697-after-partial-fix.md` are both about
closure-promoted cells, and this is a lifetime question about the same cells.
Whether it is the same root cause is not something this reporter can say — the
symptom there is a C compile error, here it is a silent use-after-free.
