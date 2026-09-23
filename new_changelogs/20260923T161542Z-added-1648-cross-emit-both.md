- **`ae build --target=<triple> --emit=both` works (#1648).** It was the
  last mode still rejected under `--target`, on the grounds that "the cross
  path links once and cannot produce both artifacts from one invocation".
  That described a design the code does not have: `--emit=both` has always
  run two ordinary builds, one `--emit=exe` and one `--emit=lib`, each with
  its own link — and both already worked cross. #1648 has no carve-out left.

- **A cross build names its outputs for the target, not the host.** Making
  `--emit=both` work cross exposed that several names were derived from the
  machine doing the build:
  - a Linux executable built on Windows was called `app.exe`, from the
    host's `EXE_EXT`;
  - with `-o NAME` the library pass appended the host's extension, so a
    Linux `.so` built on Windows was `NAME.dll`; without `-o`, a Windows
    library built on Linux came out as `libapp.dll.so`;
  - `--target X`, the two-argument spelling `cmd_build` accepts, was not
    read at all when naming, so it fell back to the host;
  - `--emit=staticlib -o libgreet.a` for a Windows target wrote a correct
    `!<arch>` archive named `libgreet.a.dll`, and `--emit=obj` did the same
    to an object — the DLL-naming rule keyed on a flag every lib-codegen
    mode sets;
  - a Windows DLL's import library was named by zig after the first *input*
    file (`app.lib` for `libapp.dll`, `appw.dll.lib` for `-o appw`), not the
    DLL a consumer links against. It is `<dll-stem>.lib` now.

  Each extension now comes from the target, classified by the canonical
  triple from `cross_target_to_zig` — the same mapping the link itself uses
  — so an alias like `arm64-macos` cannot be named one way and linked
  another. For the Emscripten `wasm` target, where an executable and a
  library are *both* a `.js` + `.wasm` pair, the library is `libNAME.js` +
  `libNAME.wasm` beside `NAME.js` + `NAME.wasm`, as an unnamed library is
  `lib<name>` on every other target; otherwise the two passes wrote the
  same module.

  `tests/integration/cross_emit_lib` now runs end to end on a Windows host,
  where its static-archive section used to fall through to a Linux triple
  and die with "cannot execute binary file", and it checks names exactly,
  since MSYS makes `[ -f both ]` true for `both.exe`.
