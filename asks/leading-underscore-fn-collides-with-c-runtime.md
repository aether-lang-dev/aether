# A leading-underscore function name is emitted verbatim and collides with the C runtime (Windows)

**From:** the aeb line (2026-08-10) · **Where it bit:** winbaz (Windows 11 /
MSYS2 MINGW64, gcc 16.1.0) — **10 of aeb's 118 tests fail to compile**, all with
the same error, all because a test helper is called `_write`.

**Affects:** v0.513.0 / v0.515.0. Reproduced on `d104ba17`.
**Windows-only** — the identical program builds and runs on Linux.

## Symptom

```console
$ ae build tests/test_java_cache.ae -o tjc --lib lib --lib tools
tjc.c:319:6: error: conflicting types for '_write'; have 'void(const char *, const char *)'
In file included from tjc.c:18:
C:/msys64/mingw64/include/io.h:247:23: note: previous declaration of '_write'
      with type 'int(int, const void *, unsigned int)'
  247 |   _CRTIMP int __cdecl _write(int _FileHandle,const void *_Buf,unsigned int _MaxCharCount);
```

## Minimal repro

Ten lines, no imports, no aeb involved:

```aether
_write(p: string, c: string) {
    println("leading underscore: ${p} ${c}")
}
main() {
    _write("a", "b")
    return 0
}
```

```console
$ ae build us.ae -o us          # Windows / MINGW64
us.c:232:6: error: conflicting types for '_write'; ...
us.ae:1:6:   error: conflicting types for '_write'; ...
Build failed.

$ ae build lu.ae -o lu          # Linux, same source
Built: lu
$ ./lu
ab
```

Linux is fine because glibc does not declare `_write`. The MSVCRT/UCRT headers
do, so every Windows build that includes `io.h` (which the generated C does,
transitively) sees the clash.

## Cause

A top-level Aether function is emitted as a C function with **the same name,
verbatim**. `_write` in Aether becomes `void _write(const char*, const char*)`
in the generated C — landing squarely in the C implementation's **reserved
identifier namespace**.

Per C11 §7.1.3, identifiers beginning with an underscore at file scope are
reserved *for the implementation*. MSVCRT uses that namespace heavily and
declares real functions there: `_write`, `_read`, `_open`, `_close`, `_access`,
`_aligned_malloc`, … A sample of just four MinGW headers (`io.h`, `stdio.h`,
`stdlib.h`, `string.h`) turns up dozens; the first twenty alphabetically are

```
_abs64 _access _access_s _aligned_free _aligned_malloc _aligned_msize
_aligned_offset_malloc _aligned_offset_realloc _aligned_offset_recalloc
_aligned_realloc _aligned_recalloc _atodbl _atodbl_l _atof_l _atoflt
_atoflt_l _atoi_l _atoi64 _atoi64_l _atol_l
```

`_write`, `_read`, `_open`, `_close` are exactly the names a programmer reaches
for when writing a private I/O helper, which is what makes this likely to
recur rather than a one-off.

## Why this is worth fixing rather than documenting

Aether **already treats a decorated underscore as a linkage signal** — issue
**#279** gives *trailing*-underscore names (`helper_`) internal linkage:

```c
/* compiler/codegen/codegen_func.c:930 */
// Trailing-underscore convention `foo_` marks a function as
// file-local — ... Emit as `static` so two .ae files in the same
// namespace bundle / [[bin]] can each declare their own
// `record_start_` / `helper_` without the generated C colliding at
// link time. Closes #279.
if (fn_has_internal_linkage(func)) {
    fprintf(gen->output, "static ");
}
```

So the language has a convention for "this is private, don't export it" — and
it is spelled with the underscore on the **wrong end** for the instinct most
people have. A leading underscore reads as private in Python, C#, JavaScript
and much C; in Aether it is the one spelling that gets *no* protection and
maximum collision risk.

Confirmed the trailing form is unaffected:

```console
$ cat us2.ae
write_(p: string, c: string) { println("trailing underscore ok: ${p} ${c}") }
main() { write_("c", "d") return 0 }
$ ae build us2.ae -o us2 && ./us2.exe
Built: us2.exe
trailing underscore ok: c d
```

## Ask, in preference order

1. **Give leading-underscore functions internal linkage too** — the same
   `static` treatment `fn_has_internal_linkage` already applies to trailing
   underscore. A `static void _write(...)` still shadows nothing at link time,
   and a file-scope `static` whose name matches a declared CRT prototype is
   still a conflicting-types error at *compile* time, so on its own this is
   necessary but not sufficient. It is the cheapest half.

2. **Prefix or mangle the emitted symbol** so user code cannot land in the
   implementation's namespace at all — e.g. emit `_write` as
   `aeuser__write`, or as `aether_fn__write`, keeping the Aether-level name
   unchanged. This closes the class rather than the instance, and it also
   removes the (rarer, but real) risk for names like `_read` that a future
   libc adds. Callers are all compiler-generated, so the rename is internal.

3. **Failing either, reject it at parse time** with a diagnostic that names the
   convention: "function name `_write` is reserved — identifiers beginning with
   `_` are reserved for the C implementation; use `write_` for a file-local
   helper (#279)". A loud, portable, early error beats a Windows-only wall of
   gcc output pointing at generated C.

Option 3 alone would have saved this entirely: the failure surfaces as a
`conflicting types` error in a `.c` file the user never wrote, on one platform
only, with no hint that the function *name* is the problem.

## Impact on the aeb line

10 of 118 aeb tests do not compile on Windows — every `*_cache` test, since
they share a `_write(path, content)` fixture helper:
`test_java_cache`, `test_go_cache`, `test_rust_cache`, `test_ts_cache`,
`test_clojure_cache`, `test_kotlin_cache`, `test_scala_cache`,
`test_dotnet_cache`, `test_groovy_cache`, `test_aether_cache`. All fail
identically. Verified on the box that they share the one cause.

aeb can and will rename its helper — that is a one-line workaround per file and
we are not blocked. Filing because the *next* person to write `_read` or
`_close` will lose the same afternoon, on Windows only, with a diagnostic that
points at generated C rather than at their function name.

## Not being asked

- No change to the trailing-underscore convention (#279); it works and this ask
  depends on it as precedent.
- Not asking for a general C-keyword/identifier blocklist. The specific,
  bounded rule "leading underscore at file scope is reserved" is C11 §7.1.3 and
  is worth honouring on its own.
- Not asking for anything at the `--emit=lib` alias layer; the collision is in
  the plain program path.

## Environment

winbaz, MSYS2 / MINGW64 on Windows 11, gcc 16.1.0, aether v0.515.0 built from
`/c/Users/paul/scm/aether`. Linux comparison: same source, aether 0.512.0,
builds and runs clean.
