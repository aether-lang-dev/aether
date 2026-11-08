# Vendored libfyaml 1.0.0-beta2

Byte-identical subset of upstream libfyaml 1.0.0-beta2
(<https://github.com/pantoniou/libfyaml>), MIT — full licence in `LICENSE`
here and in `THIRD_PARTY_LICENSES.md`. It bundles xxHash (BSD-2-Clause, the
licence is in `src/xxhash.c`) and BLAKE3's portable C implementation.
Vendored so `std.yaml` works with no system libfyaml and no program links
`-lfyaml`: native builds on any box, MSYS2's static Windows link, and
`ae build --target ...` cross builds with no sysroot.

- Upstream release asset: libfyaml-1.0.0-beta2.tar.gz
- SHA-256: `f891584e396ee17b520fc2f486f4848edd4315d2cc6d38aad4c5569aec113ec5`
- Regenerate with: `scripts/vendor-libfyaml.sh` (edit VERSION/SHA256 there to bump)

Layout: `include/` is upstream's `include/`. `src/` holds, side by side,
the files upstream keeps in `src/lib`, `src/util`, `src/xxhash`,
`src/thread`, `src/allocator` and `src/blake3` (no basename repeats), so
their quoted includes resolve next to each other with no include path.
Left out: tests, tools, examples, docs, the build system, the generic and
reflection subsystems, and BLAKE3's assembly and SIMD variants.

**No local patches.** There is no `config.h`: the configure-step defines
(portable BLAKE3 only, no SIMD or assembly, qsort_r and mremap where the
platform has them) live in `std/yaml/aether_libfyaml_vendored.c`, and
`std/yaml/aether_libfyaml_names.h` renames every exported symbol to
`aether_<name>`. Nothing in this directory is edited. If a change is ever
needed, it goes upstream — not here.

This directory is compiled ONLY as textual includes of
`aether_libfyaml_vendored.c` (a single translation unit, guarded by
`AETHER_VENDOR_YAML`). Its .c files must never be added to MANIFEST,
STD_SRC, or any build's source list.
