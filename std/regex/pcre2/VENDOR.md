# Vendored PCRE2 10.44

Byte-identical subset of upstream PCRE2 10.44
(<https://github.com/PCRE2Project/pcre2>), BSD-3-Clause — full licence in
`LICENCE` here and in `THIRD_PARTY_LICENSES.md`. Vendored for #1389 so
`std.regex` works without a system libpcre2-8: native builds on boxes
without the library, and `ae build --target ...` cross builds with no
CROSSBUILD_SYSROOT.

- Upstream tarball: pcre2-10.44.tar.gz
- SHA-256: `86b9cb0aa3bcb7994faa88018292bc704cdbb708e785f7c74352ff6ea7d3175b`
- Regenerate with: `scripts/vendor-pcre2.sh` (edit VERSION/SHA256 there to bump)

Three files are upstream files under the name upstream designates for
non-autotools embedding: `pcre2.h` (from `pcre2.h.generic`), `config.h`
(from `config.h.generic`), `pcre2_chartables.c` (from
`pcre2_chartables.c.dist`). Everything else keeps its upstream name.

**No local patches.** Feature selection lives in
`std/regex/aether_pcre2_vendored.c` (8-bit code units, Unicode on, JIT
off); nothing in this directory is edited. If a change is ever needed,
it goes upstream — not here.

This directory is compiled ONLY as textual includes of
`aether_pcre2_vendored.c` (a single translation unit, guarded by
`AETHER_VENDOR_PCRE2`). Its .c files must never be added to MANIFEST,
STD_SRC, or any build's source list.
