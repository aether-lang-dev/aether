# Cross-compiling for iOS (arm64)

`ae build --target=aarch64-ios` builds Mach-O arm64 artifacts for iPhone and
iPad, `--target=aarch64-ios-simulator` / `--target=x86_64-ios-simulator`
build for the simulator, and `--target=aarch64-ios-macabi` /
`--target=x86_64-ios-macabi` build for **Mac Catalyst** (a UIKit app running on
a Mac). Unlike every other cross target, all of these are driven by the
**Xcode toolchain** rather than by `zig cc`.

## Why iOS is not a zig target

The other cross targets are self-contained because zig bundles each one's libc,
headers and linker. Apple's SDKs are Xcode-licensed and cannot be redistributed
that way, so there is no bundled-toolchain path for iOS and there will not be
one. `--target=aarch64-ios` therefore shells to `xcrun clang` with the SDK
`xcrun` reports, which means:

- **The build host must be a Mac with Xcode installed.** The Command Line Tools
  alone carry no iPhoneOS SDK. If `xcrun --sdk iphoneos --show-sdk-path` fails,
  so does the build, with a message saying exactly that.
- `xcrun` is asked for the SDK path rather than hardcoding
  `/Applications/Xcode.app/...`, so a relocated Xcode, a beta Xcode, or a
  `DEVELOPER_DIR` override all work.
- Device, simulator and Catalyst are **separate targets**, not flags on one
  target. They use different SDKs and stamp different Mach-O platforms (`IOS`,
  `IOSSIMULATOR`, `MACCATALYST`); a binary built for one will not load on
  another. Catalyst is the odd one: its triple carries `-ios` like the device
  arm, but it builds against the **macOS** SDK (`xcrun --sdk macosx`).

## What to build: usually a library, not an executable

iOS does not run loose executables. A binary only launches from inside a signed
`.app` bundle, so on iOS the useful artifact is almost always a **library that
Xcode links into an app**, not a standalone program:

```bash
# A static archive with the aether_<name>() C ABI. THIS is what an App Store
# build links: iOS forbids third-party dynamic libraries, so a shipped app must
# link Aether statically. The .a holds your code AND the Aether runtime/stdlib,
# so Xcode needs exactly one file in "Link Binary With Libraries".
ae build --target=aarch64-ios --emit=staticlib mylib.ae -o libmylib.a

# A Mach-O dylib with the same C ABI. Fine for local development and for
# Catalyst, but NOT shippable to the App Store (see above).
ae build --target=aarch64-ios --emit=lib mylib.ae -o libmylib.dylib

# An executable, if you really want one (still needs signing + a bundle).
ae build --target=aarch64-ios hello.ae -o hello

# A target-format object, to hand to your own link step / Xcode build phase.
ae build --target=aarch64-ios --emit=obj mylib.ae -o mylib.o

# Portable C + catalog, compiled later by Xcode itself. Needs no Xcode here.
ae build --target=aarch64-ios --emit=csrc mylib.ae -o mylib
```

`--emit=staticlib` is the primary case on iOS. Apple does not allow
third-party dynamic libraries inside an App Store binary, so a dylib is a
development convenience rather than a shipping artifact; `--emit=staticlib`
produces a single `.a` containing the program objects together with the Aether
runtime and stdlib compiled for that triple.

`--emit=lib` is also **supported on iOS**, whereas the zig
cross targets still reject it. The dylib is linked with
`-install_name @rpath/<leaf>`, which is what an Xcode *Embed Frameworks* phase
expects; without it the load command would record the build-machine path and the
library would fail to load from inside the bundle.

For a step-by-step walkthrough of wiring one of these into an actual app —
bridging header, calling from SwiftUI, bundling, and the mistakes that cost
the most time — see **[swiftui-ios-app.md](swiftui-ios-app.md)**.

The exported symbols are the ordinary `aether_<name>()` C ABI (see
[emit-lib.md](emit-lib.md)), so Swift and Objective-C call them through a
bridging header with no glue. Note that `--emit=lib` writes only the library —
the **header comes from `--emit=csrc`** (same catalog codegen, so the two cannot
drift) or from `aetherc --emit-header`:

```bash
ae build --target=aarch64-ios --emit=csrc mylib.ae -o mylib   # -> mylib.h
```

```swift
// bridging header: #include "mylib.h"
let sum = aether_add(2, 3)
```

A `-> string` export surfaces as `const char*`: the generated `aether_<name>()`
wrapper calls `aether_string_data()` on the `AetherString*` for you, so Swift
receives a NUL-terminated C string. Copy it (`String(cString:)`) rather than
retaining the pointer.

## Deployment target

The deployment target is part of the clang triple, so it is fixed at build
time. The default is **iOS 15.0** for the device and simulator targets. Catalyst
has its own floor, and it differs by architecture: **13.1** on x86_64 (the
`macabi` ABI does not exist before it) and **14.0** on arm64, since arm64
Catalyst did not exist until Apple Silicon — clang silently raises anything
lower to 14.0, so asking for less produces a triple that does not describe its
own output. Override any of them with `AETHER_IOS_MIN`:

```bash
AETHER_IOS_MIN=17.0 ae build --target=aarch64-ios --emit=lib mylib.ae -o libmylib.dylib
```

It lands in `LC_BUILD_VERSION`, which you can check with
`vtool -show-build <file>`.

## What does not work on iOS

iOS is a sandboxed platform, and some of the standard library assumes a
general-purpose OS underneath. None of this fails the build; it is behaviour to
plan around.

| Surface | Status on iOS |
|---|---|
| `os.system` (shell-out) | Always returns `-1`. iOS marks `system(3)` unavailable, so there is no shell to exec into. See `AETHER_HAS_SHELL`. |
| `os.run` / `os.run_capture` | Compile and link, but iOS forbids spawning child processes in an app sandbox. |
| `std.dl` | `dlopen` is restricted to libraries already inside the app bundle. |
| `std.audio` | Reports unavailable. miniaudio's iOS backend is AVAudioSession (Objective-C), which is not valid C, so the null backend is selected via `-DMA_NO_COREAUDIO` — the same treatment the macOS cross target gets. |
| `libaether_sandbox.so` | Not available. It is an `LD_PRELOAD` mechanism, which iOS has no equivalent of. Use `--emit=lib` capability gating and `hide` / `seal except` instead, both of which are compile-time and work everywhere. |
| `std.fs` paths | Confined to the app's container by the OS sandbox. |
| OpenSSL / zlib / nghttp2 | Not linked, so HTTPS/TLS, hashing, base64 and compression report errors at runtime — the same warn-and-degrade as every other cross target without a `CROSSBUILD_SYSROOT`. `std.regex` is the exception and always works (vendored PCRE2). |

`AETHER_HAS_SHELL` is a Tier-0 platform capability alongside
`AETHER_HAS_FILESYSTEM` and friends (`runtime/config/aether_optimization_config.h`).
It is 0 on iOS/tvOS/watchOS and can be forced off anywhere with
`-DAETHER_NO_SHELL`.

## Building a fat / XCFramework artifact

`ae` emits one slice per invocation. Combine them with Apple's own tools:

```bash
ae build --target=aarch64-ios           --emit=staticlib mylib.ae -o device/libmylib.a
ae build --target=aarch64-ios-simulator --emit=staticlib mylib.ae -o sim/libmylib.a
ae build --target=aarch64-ios-macabi    --emit=staticlib mylib.ae -o catalyst/libmylib.a

# Device and simulator slices cannot go in one fat binary (same arch, different
# platform) — that is exactly what an XCFramework is for.
xcodebuild -create-xcframework \
    -library device/libmylib.a \
    -library sim/libmylib.a \
    -library catalyst/libmylib.a \
    -output MyLib.xcframework
```

## Signing

`ae` does not sign anything. Sign the artifact as part of your Xcode build, or
by hand:

```bash
codesign -s "Apple Development: you@example.com" libmylib.dylib
```
