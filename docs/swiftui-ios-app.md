# Using an Aether library from a SwiftUI iOS app

How to put an Aether compute core inside an iOS app: build the `.ae` as a
library for iOS, call its `aether_<name>()` exports from Swift through a
bridging header, and ship it in the app bundle.

This is the walkthrough. Two companions cover the pieces it leans on:
[cross-ios.md](cross-ios.md) for the iOS target itself (why it uses Xcode
rather than zig, what does not work on iOS, XCFramework and signing), and
[emit-lib.md](emit-lib.md) for the `aether_<name>()` ABI and the full type
mapping.

## The shape

```
  fibcore.ae  ──ae build --emit=csrc──▶  fibcore.h    ──▶ bridging header
      │                                  (prototypes)
      └──ae build --emit=staticlib──▶  libfibcore.a  ──▶ linked into the app
                --target=…-ios              (aether_fib)
```

Swift never sees Aether. It sees C functions. Aether's job is the
computation; the app shell, the UI, `Info.plist`, bundling and signing stay
in Xcode.

**This is a library, not an app.** iOS does not run loose executables, so
Aether cannot own `main()` here — see *What to build* in
[cross-ios.md](cross-ios.md). If you want the UI in Aether too, that needs a
UIKit backend for aether-ui, which does not exist yet.

## Step 1 — write the Aether side

Everything you `export` becomes a C entry point. Keep the surface small and
scalar where you can; it is the seam you have to marshal across.

```aether
// fibcore.ae — the whole computation lives here.
export fib(n: int) -> long {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
```

## Step 2 — generate the bridging header

`--emit=lib` writes only the library. The header comes from `--emit=csrc`,
which runs the same catalog codegen, so the two cannot drift:

```sh
ae build --emit=csrc fibcore.ae -o fibcore     # fibcore.h + fibcore.c + catalog
```

```c
/* fibcore.h */
int64_t aether_fib(int32_t n);
```

Note this step needs no Xcode and no toolchain at all — it only writes
source, so it works on any machine and in any CI lane.

## Step 3 — build the library for iOS

One invocation per slice. Device and simulator are separate targets: same
architecture is possible, but the Mach-O *platform* differs and a binary
built for one will not load on the other.

```sh
# Device — a static archive holding your code AND the Aether runtime/stdlib.
# This is what a shipping app links: iOS forbids third-party dylibs in an
# App Store binary.
ae build --target=aarch64-ios --emit=staticlib fibcore.ae -o libfibcore.a

# Simulator — pick the slice that matches YOUR Mac, see below
ae build --target=aarch64-ios-simulator --emit=staticlib fibcore.ae -o libfibcore-sim.a
```

`--emit=lib` gives a `.dylib` instead. It is convenient for a quick
command-line loop (and fine for Mac Catalyst), but it is **not shippable to the
App Store** — see *What to build* in [cross-ios.md](cross-ios.md). Reach for
`--emit=staticlib` unless you have a reason not to.

### Pick the right simulator architecture

The simulator runs your Mac's architecture, not the phone's:

| Host Mac | Simulator target |
|---|---|
| Apple Silicon | `aarch64-ios-simulator` |
| Intel | `x86_64-ios-simulator` |

Getting this wrong produces a library that links but cannot load, and
`simctl` reports it in a thoroughly unhelpful way — see *Troubleshooting*.

### Deployment target

The minimum iOS version is baked into the clang triple, so it is fixed at
build time. Default is 15.0; override with `AETHER_IOS_MIN`:

```sh
AETHER_IOS_MIN=17.0 ae build --target=aarch64-ios --emit=staticlib fibcore.ae -o libfibcore.a
```

Check it with `vtool -show-build libfibcore.a`. The library's floor only
needs to be **at or below** the app's — a 15.0 library links fine into a 16.0
app.

## Step 4 — call it from SwiftUI

Add `fibcore.h` to your target's bridging header (Xcode: *Objective-C
Bridging Header* in build settings). The exports are then plain Swift
functions:

```swift
import SwiftUI

let fibInputs: [Int32] = [20, 22, 24, 26, 28, 30]

@MainActor
final class FibModel: ObservableObject {
    @Published var results: [(n: Int32, value: Int64)] = []

    func run() {
        Task.detached(priority: .userInitiated) {
            var out: [(Int32, Int64)] = []
            for n in fibInputs {
                out.append((n, aether_fib(n)))     // <-- Aether
                let snapshot = out
                await MainActor.run { self.results = snapshot }
            }
        }
    }
}

struct ContentView: View {
    @StateObject private var model = FibModel()

    var body: some View {
        List(model.results, id: \.n) { r in
            HStack {
                Text("fib(\(r.n))").font(.system(.body, design: .monospaced))
                Spacer()
                Text("\(r.value)").bold()
            }
        }
        .onAppear { model.run() }
    }
}
```

Run Aether calls **off the main actor** if they are not instant, exactly as
you would any other blocking computation — nothing about the FFI makes a slow
function safe to call from the UI thread.

### Types across the boundary

| Aether | C / Swift |
|---|---|
| `int` | `int32_t` / `Int32` |
| `long` | `int64_t` / `Int64` |
| `float` | `double` / `Double` |
| `bool` | `int` / `Int32` (0 or 1) |
| `string` (param) | `const char*` / `UnsafePointer<CChar>` |
| `string` (return) | `const char*` / `UnsafePointer<CChar>?` |

The full table is in [emit-lib.md](emit-lib.md#type-mapping).

**Strings need care.** For a `-> string` export the generated wrapper calls
`aether_string_data()` for you, so what Swift receives really is a
NUL-terminated C string rather than an opaque handle. Copy it immediately and
do not retain the raw pointer:

```swift
if let p = aether_greet("SwiftUI") {
    let s = String(cString: p)     // copy now
    // `p` must not be stored — its lifetime is not yours to manage
}
```

Prefer scalars across the boundary where you can. If you need to return
structured data, returning a JSON string that Swift decodes is usually less
work than hand-marshalling, and it keeps the ABI surface one function wide.

## Step 5 — get it into the bundle

### The Xcode way

Drop `libfibcore.a` into **Link Binary With Libraries**. That is the whole
integration — the archive already contains the Aether runtime and stdlib, so
there is exactly one file to add and no embed or signing phase.

If you used `--emit=lib` instead, a `.dylib` additionally needs an **Embed
Frameworks** phase with *Code Sign On Copy*. `ae` links it
`-install_name @rpath/<leaf>`, which is what that phase expects — without it
the load command would record the build machine's path and the app would fail
to launch anywhere else.

Device and simulator slices cannot coexist in one Mach-O (same architecture,
different platform), so ship the slices as an XCFramework rather than a fat
binary. `xcodebuild -create-xcframework` takes the `.a` files directly; see
*Building a fat / XCFramework artifact* in [cross-ios.md](cross-ios.md), which
also covers the Mac Catalyst slice (`--target=aarch64-ios-macabi`).

### Without an `.xcodeproj`

A bundle is a directory, an `Info.plist` and an executable, so the whole loop
can run from the command line — useful for CI and for a quick check:

```sh
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)

# a dylib is easier to hand to swiftc directly than an archive
ae build --target=x86_64-ios-simulator --emit=lib fibcore.ae -o libfibcore.dylib

xcrun --sdk iphonesimulator swiftc \
    -target x86_64-apple-ios16.0-simulator -sdk "$SDK" \
    -parse-as-library \
    -import-objc-header fibcore.h FibApp.swift libfibcore.dylib \
    -Xlinker -rpath -Xlinker @executable_path \
    -o AetherFib.app/AetherFib

cp Info.plist libfibcore.dylib AetherFib.app/
codesign --force --sign - AetherFib.app

xcrun simctl install booted AetherFib.app
xcrun simctl launch  booted dev.example.fibdemo
xcrun simctl io      booted screenshot shot.png
```

`-parse-as-library` is required whenever the app uses `@main`. The
`Info.plist` needs at minimum `CFBundleIdentifier`, `CFBundleExecutable`,
`CFBundlePackageType` (`APPL`), `MinimumOSVersion` and `UIDeviceFamily`.

## What the Aether side can and cannot do

Actors, the scheduler, strings, collections, `std.math` and `std.regex` (the
vendored PCRE2 needs no sysroot) all work. Keep the Aether side pure
computation:

- **No networking.** No TLS is linked, and App Transport Security blocks
  cleartext anyway. Do networking in Swift with `URLSession` and hand Aether
  the bytes.
- **No child processes.** `os.system` returns `-1` on iOS and the sandbox
  forbids `fork`/`exec` regardless.
- **No filesystem outside the app container**, and pass paths in from Swift
  rather than constructing them in Aether.

The full table is in [cross-ios.md](cross-ios.md#what-does-not-work-on-ios).

## Troubleshooting

| Symptom | Cause |
|---|---|
| `building for iOS but linking object built for macOS` | A host build got into the link. Every object must come from an `--target=…-ios` build. |
| `simctl`: `Invalid or missing Program/ProgramArguments` | The binary's architecture does not match the simulator's. On an Intel Mac an `aarch64-ios-simulator` build fails exactly this way — and so does a plain C control binary, so it looks like a broken simulator rather than a wrong slice. Check with `file`. |
| `Library not loaded: /path/on/build/machine/lib….dylib` | The `install_name` is absolute. `ae` sets `@rpath/<leaf>`; if you relinked by hand, pass `-install_name @rpath/<leaf>`. |
| `'main' attribute cannot be used in a module that contains top-level code` | Add `-parse-as-library` to `swiftc`. |
| `'NavigationStack' is only available in iOS 16.0 or newer` | The Swift target's deployment version, unrelated to Aether. `AETHER_IOS_MIN` only sets the *library's* floor. |
| `std.regex` matches nothing and returns no error | Something linked a stub built without `-DAETHER_HAS_PCRE2 -DAETHER_VENDOR_PCRE2`. `ae build --target` adds both; a hand-rolled compile of the runtime sources must too. |
| `could not locate the iphoneos SDK` | Command Line Tools only. Install Xcode, then `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`. |

## A note on verification

An iOS library that links is not an iOS library that runs. `otool`, `lipo`
and `vtool` confirm shape, not behaviour, and the two come apart in practice.
Run the thing on a simulator before believing it — `simctl install` +
`simctl launch` is a few seconds and catches what inspection cannot.
