# `fs.read_binary` collapses every failure into `"cannot read file"`

**From:** the aeb line (2026-08-09) · **Found while:** root-causing an
intermittent aeb build bug where a target reported a cache miss and rebuilt
nothing. The proximate cause was a failed content hash — and the error string
Aether handed back could not say which file failed, or why.

**Affects:** `v0.506.0` (current main, `b2401b50`). Verified present at HEAD.

## Summary

`fs.read_binary(path)` returns `(bytes, length, err)`. On **any** failure the
`err` is the constant string:

```
cannot read file
```

No `errno`. No path. No kind. Six distinct failure modes — including
**sandbox denial** and **silent truncation** — are indistinguishable to the
caller.

The machinery to fix this already exists in the same file: the issue-#392
structured-error pilot (`AETHER_FS_KIND_*`) covers `fs.copy` / `move` /
`realpath` / `chmod`. `read_binary` was simply left out of it.

## Where the information is lost

`std/fs/aether_fs.c:1339` — `fs_read_binary_raw` funnels everything to a bare
`NULL`, discarding `errno` at each step:

```c
char* fs_read_binary_raw(const char* path, int* out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    if (!aether_sandbox_check("fs_read", path)) return NULL;   // (1) sandbox denial

    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;                                       // (2) ENOENT/EACCES/EMFILE/EISDIR

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }  // (3) not seekable
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    size_t alloc_cap = (size_t)size + 1;
    char* buf = (char*)aether_caps_malloc(alloc_cap);
    if (!buf) { fclose(fp); return NULL; }                      // (4) OOM / cap exceeded

    size_t read = (size > 0) ? fread(buf, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (read != (size_t)size) {                                 // (5) short read / truncation
        aether_caps_free(buf, alloc_cap); return NULL;
    }
    ...
}
```

`fs_read_binary_tuple` (`:1421-1427`) then synthesises one constant for all of
them:

```c
    if (!buf) {
        out._0 = (void*)string_empty();
        out._1 = 0;
        out._2 = "cannot read file";      // <-- every case above lands here
        return out;
    }
```

Note the asymmetry: the very next branch (`:1438`) *does* distinguish
`"allocation failed"` for the wrapper alloc. So the tuple already carries
distinct reasons where the code bothered to produce them.

`fs.read` (`std/fs/module.ae:378`) has the same shape —
`"cannot open file"` vs `"cannot read file"`, no errno, no path.

## Why the sandbox case is the worst of them

Failure (1) is `aether_sandbox_check("fs_read", path)` — a *policy* refusal,
not an I/O error. A program running under `spawn_sandboxed` that reads a
non-granted path is told "cannot read file", which reads as a missing or
corrupt file. That is actively misleading: the file is present and readable,
the grant is missing. Anyone debugging a sandbox policy is sent looking at the
filesystem instead of at their grants.

## Impact on the caller (concrete, this is how it was found)

aeb's content-addressed build cache hashes a target's transitive import
closure. `lib/cache/module.ae:221`:

```aether
hash_file(p: string) {
    bytes, length, rerr = fs.read_binary(p)
    if string.length(rerr) > 0 { return "", rerr }
    return cryptography.sha256_hex(bytes, length)
}
```

Any unreadable member of a 27-file closure aborts the key computation, and aeb
falls through to a rebuild. That part is aeb's design and is fine. What is not
fine is that the resulting diagnostic — for a 79-target parallel build — is
`"cannot read file"` with **no path**. There is no way to attribute the failure
to a file, so an intermittent hashing failure under concurrency is
undiagnosable from the caller's side. aeb has its own bug to fix here (it
currently swallows `rerr` entirely), but fixing that only surfaces a constant
string.

Reproduced deliberately (aeb, aether-ui tree): `chmod 000` one closure member →
the target reports a cache miss and rebuilds, with no indication which file was
unreadable.

This generalises well past aeb: **no Aether program can currently distinguish
"file absent" from "permission denied" from "I/O error" from "sandbox denied"
on a binary read.** Any tool that wants to treat a missing optional file as
benign but a permission error as fatal cannot do so today.

## Minimal repro

```aether
import std.fs
import std.string

probe(label: string, p: string) {
    bytes, len, err = fs.read_binary(p)
    println("${label}: err='${err}' len=${len}")
}

main() {
    probe("missing   ", "/tmp/definitely-not-here-12345")
    probe("directory ", "/tmp")
    probe("no-perm   ", "/etc/shadow")
    return 0
}
```

Actual (v0.506.0) — three different causes, one string:

```
missing   : err='cannot read file' len=0
directory : err='cannot read file' len=0
no-perm   : err='cannot read file' len=0
```

Wanted: something that names the cause and the path, e.g.

```
missing   : err='/tmp/definitely-not-here-12345: no such file or directory' len=0
directory : err='/tmp: is a directory' len=0
no-perm   : err='/etc/shadow: permission denied' len=0
```

## Ask

1. **Extend the #392 structured-error pilot to `fs.read_binary`.** The kind
   constants already exist in `std/fs/aether_fs.h:15-27`
   (`NOT_FOUND`, `PERMISSION_DENIED`, `IS_DIR`, `IO`, `INVALID`, …). Capture
   `errno` at each early-return in `fs_read_binary_raw` rather than collapsing
   to `NULL`, and map it with the same `errno`→kind helper `fs_copy` uses
   (`aether_fs.c:1487`).

2. **Put the path in the message.** Even without a kind code, `"<path>:
   permission denied"` would have made this a five-minute diagnosis instead of
   a multi-hour one. This alone would resolve most of the pain.

3. **Give the sandbox denial its own kind/message**, distinct from an I/O
   error — a policy refusal is not a filesystem failure. `PERMISSION_DENIED`
   with a message that says *sandbox* would do.

4. **Same treatment for `fs.read`** (`std/fs/module.ae:378`), which has the
   identical flaw. Worth doing together — callers pick between them by
   text-vs-binary, not by error quality, and the asymmetry is a trap.

### On compatibility

The `(bytes, length, err)` arity does **not** need to change. Callers test
`string.length(err) > 0`, which keeps working if the message merely becomes
informative. That makes (2) a safe, standalone first step even if the full
kind-code work in (1) lands later.

If a kind code is added, the natural shape mirrors `fs.copy` —
`(bytes, length, kind, err)` — but that is an arity break and should be a
deliberate, separately-versioned decision, not a drive-by. **The ask here is
satisfied by (2) + (3) alone**; (1) and (4) are the fuller fix.

## Not being asked

- No change to the success path or its `@heap` ownership contract (documented
  at `std/fs/module.ae:240-250`) — that part works and the aliasing rules there
  are subtle enough to leave alone.
- Not asking for the four-extern split-accessor path
  (`fs_try_read_binary` + getters) to grow errors; the tuple shape is the
  canonical entry point per #271/#273, and it is the one aeb uses.
- Not asking for `errno` to be exposed as a raw integer to Aether. A kind
  constant plus a human-readable message is the right altitude.

## Related

- Issue **#392** — the structured-error pilot this asks to extend
  (`fs.copy`/`move`/`realpath`/`chmod` already have `(…, kind, message)`).
- Issues **#271** / **#273** — the tuple-return consolidation that produced
  `fs_read_binary_tuple` and its `"cannot read file"` constant.
- aeb-side companion bug (being fixed independently, in the aeb repo): the
  cache-key path discards this `err` instead of reporting it, and records a
  cache-miss marker *before* the build runs, so a hashing failure renders as a
  normal rebuild. See `aeb/asks/fanout-reports-miss-but-skips-rebuild.md`.
