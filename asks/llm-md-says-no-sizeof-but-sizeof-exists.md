# Docs fix: `LLM.md` says "there is no `sizeof`" — but `sizeof(T)` exists and is the recommended form

**From:** the LangArena port (2026-09-09) · **Where it bit:** allocating
heap structs for the binarytrees benchmark. The stale note actively steered me
INTO the exact footgun it was trying to warn about.

## What's wrong

`LLM.md`, in the "Idioms that keep biting" section, states:

> **`malloc(N) as *T` is sized BY HAND; adding a field to `T` corrupts the
> heap.** There is no `sizeof` in Aether, so heap-allocated structs carry a
> literal byte count at each allocation site — `malloc(128) as *Sha`,
> `malloc(136) as *LeafCert` — and 40+ crypto modules do this.

But `sizeof(T)` **does** exist and is used across the stdlib:

```
std/http1/module.ae:69:        r = malloc(sizeof(HttpResponse)) as *HttpResponse
std/os/module.ae:593:          t = malloc(sizeof(LocalTime)) as *LocalTime
std/bignum/module.ae:77:       n = malloc(sizeof(BN)) as *BN
std/cryptography/module.ae:594: w = malloc(sizeof(DigestCtx)) as *DigestCtx
```

and the compiler itself emits a warning steering you to it:

```
warning: `malloc(24) as *TreeNode` sizes the allocation by a literal byte
count; use `malloc(sizeof(TreeNode))` so a struct-layout change cannot
silently under-allocate
```

## Why it matters

Following the LLM.md note, I hand-counted `malloc(24) as *TreeNode`
(int + two pointers, guessing at alignment). That works until someone adds a
field — the precise heap-corruption-far-from-the-edit failure the note itself
describes. The compiler warning tipped me off; switching to
`malloc(sizeof(TreeNode))` cleared the warning and removed the latent bug. So
the guidance is not just stale, it's *inverted*: the note tells a porter to do
the dangerous thing, when the safe primitive is right there and the stdlib
already uses it uniformly.

## Suggestion

Rewrite that LLM.md bullet to: "heap-allocate structs with
`malloc(sizeof(T)) as *T` — never a hand-counted byte literal; the compiler
warns on the literal form. (Historically some crypto modules hand-sized; those
are the anti-pattern, not the model.)" If any allocation sites in the tree
still use literal counts, they're the ones to migrate — not the pattern to
teach.

## Impact / workaround

Doc-only; no code blocked. Filed because LLM.md is the first thing a porting
session reads, and this bullet cost real time and introduced a latent bug that
the language had already made unnecessary.
