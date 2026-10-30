# Getting Started with Aether

This page installs Aether, runs a first program, and starts a project. The
language itself is taught by [the path through the examples](../examples/README.md),
with [the tutorial](tutorial.md) beside it; [Learn Aether](#learn-aether) at
the end says how they fit together.

## Installation

### Quick Install (recommended)

```bash
git clone https://github.com/aether-lang-dev/aether.git
cd aether
./install.sh
```

This builds the compiler, CLI tool, and standard library, then installs everything to `~/.aether`. After restarting your terminal (or running `source ~/.bashrc`, `~/.zshrc`, or `~/.bash_profile`), the `ae` command is available globally.

To also build the native contrib modules (sqlite, embedded Python/Lua/…)
whose dev libraries your machine has:

```bash
./install.sh --contrib-too
```

To install to a custom location:

```bash
./install.sh /usr/local    # System-wide install (needs sudo)
```

### Prerequisites

The installer checks for these automatically and provides platform-specific install commands if something is missing. Here's what you need:

**macOS:**
```bash
xcode-select --install
# Or with Homebrew: brew install gcc make
```

**Linux (Debian/Ubuntu/Pop!_OS/Mint):**
```bash
sudo apt-get install build-essential
```

**Linux (Fedora):**
```bash
sudo dnf install gcc make
```

**Linux (RHEL/CentOS/Rocky/AlmaLinux):**
```bash
sudo yum install gcc make
```

**Linux (Arch/Manjaro):**
```bash
sudo pacman -S base-devel
```

**Linux (openSUSE):**
```bash
sudo zypper install gcc make
```

**Linux (Alpine):**
```bash
apk add build-base
```

**Windows:**

The easiest way is to download the pre-built release binary, no MSYS2, no manual toolchain required:

1. Download `aether-*-windows-x86_64.zip` from [GitHub Releases](https://github.com/aether-lang-dev/aether/releases)
2. Extract to any folder, e.g. `C:\aether`
3. Add `C:\aether\bin` to your PATH (System Settings → Environment Variables → Path)
4. **Restart your terminal** (so PATH takes effect)
5. Open any terminal (PowerShell or CMD):

```powershell
ae version
ae init hello
cd hello
ae run
```

**GCC is downloaded automatically on the first `ae run`** (~80 MB, one-time). No MSYS2, no separate installer needed.

> **Windows Defender tip:** The first build may trigger a scan. Adding `C:\aether` to Windows Security exclusions speeds things up.

> **Building from source / contributors:** Install [MSYS2](https://www.msys2.org/), open "MSYS2 MinGW 64-bit", then `pacman -S mingw-w64-x86_64-gcc make git` and `make ae`.

### Development Build (without installing)

If you prefer to build without installing to your system:

```bash
make ae
./build/ae version
```

This builds the `ae` CLI tool in the `build/` directory. You'll need to use `./build/ae` instead of just `ae`.

### Editor Setup

For syntax highlighting and a better development experience:

**VS Code / Cursor:**
```bash
cd editor/vscode
./install.sh
```

Features included:
- Syntax highlighting with TextMate grammar
- Custom "Aether Erlang" dark theme optimized for Aether code
- `.ae` file icons
- Basic language configuration (comments, brackets, etc.)

After installation, open any `.ae` file and VS Code will automatically apply syntax highlighting. Select the "Aether Erlang" theme via `Preferences > Color Theme` for the best experience.

## Your First Aether Program

Create `hello.ae`:

```aether
main() {
    name = "Aether"
    println("Hello, ${name}!")
}
```

Run it:

```bash
ae run hello.ae
```

`ae run` compiles the file to C, builds it and runs it, keeping the build
in a cache so the next run of an unchanged file starts at once. The other
everyday commands:

```bash
ae build hello.ae -o hello      # an executable
./hello
ae check hello.ae               # type-check only, no build
ae fmt hello.ae                 # format in place (--check to only check)
ae build --target wasm hello.ae # WebAssembly; needs Emscripten (emcc) on PATH
node hello.js
```

## A Project

```bash
ae init myproject
cd myproject
ae run
ae test
```

`ae init` writes `aether.toml`, `src/main.ae` and `tests/`. `ae run` and
`ae build` in the directory build the program `aether.toml` names, and
`ae test` runs the tests under `tests/` ([Testing](testing.md)). The
configuration:

```toml
[package]
name = "myproject"
version = "0.1.0"
description = "A new Aether project"
license = "MIT"

[[bin]]
name = "myproject"
path = "src/main.ae"

[dependencies]

[build]
target = "native"
# link_flags = "-lsqlite3 -lcurl"  # Link external C libraries
```

`link_flags` links C libraries a program calls through `extern`
([C interoperability](c-interop.md)). A project's own modules live in
`lib/<name>/module.ae`; [the modules example](../examples/01-language/14-modules/main.ae)
shows one, and [`--lib`](../examples/06-tooling/04-lib-path/main.ae) adds
more directories to search.

### Packages

```bash
ae add github.com/user/repo          # the default branch
ae add github.com/user/repo@v1.0.0   # a version
ae add github.com/user/repo@v1.1.0 --update   # move a pinned dependency
```

GitHub, GitLab, Bitbucket, Codeberg and self-hosted forges work the same
way. `ae add` needs no git: it takes the archive the forge serves, pins the
hash of its tree in `aether.toml`, and refuses a later fetch that is not
that tree. With a version, it takes the package's release artifact for your
platform when the package publishes one. [Package fetching](package-fetching.md)
has the rest: mirrors, release artifacts, and publishing binary packages.

## Tools for When You Are Stuck

**`ae help <script.ae>`** turns the compiler's terse findings into
suggestions: a misspelt name, a missing import, a YAML-shaped line inside a
builder block. `--fix` applies the safe rewrites after showing the diff,
and `--json` is for CI. It makes no network call and runs nothing
([Config-IS-Code Diagnostics](cic-help.md),
[an example](../examples/06-tooling/05-ae-help/broken.ae)).

**`ae repl`** evaluates as you type. Assignments and constants persist,
multi-line blocks continue until their braces close, `:show` prints the
session, `:reset` clears it and `:quit` leaves.

```
ae> x = 5
ae> println(x * 2)
10
ae> :quit
```

## Learn Aether

Three documents, each with one job:

1. **[The path](../examples/README.md)**: the examples in order, from the
   first program to actors, the standard library, C, the sandbox, the
   tools and whole programs. Each is one file that teaches one idea, says
   how to run it, and ends with the output it prints; CI checks that it
   still does.
2. **[The tutorial](tutorial.md)**: the path told as a story. A chapter per
   part, saying what each step shows and why it comes there, the mistakes
   newcomers make, and exercises with their answers.
3. **[The language reference](language-reference.md)**: everything, to look
   up. The [glossary](glossary.md) defines the words the others use.

Start with the first chapter of the tutorial and its examples, run each
one, and change it.

## Version Management

Install, upgrade, and switch between Aether releases without reinstalling:

```bash
ae upgrade              # Install the latest release and switch to it
ae install v0.830.0     # Install a specific release (latest if omitted)
ae use v0.830.0         # Switch to an installed version
ae version              # Show current version
ae version list         # List all available releases (marks installed/active)
ae version installed    # List only what is installed locally
```

`ae install` / `ae use` are the short forms of `ae version install` /
`ae version use`, which still work. `ae upgrade` (alias `ae update`) is a
no-op when you are already on the latest release.

Versions are stored in `~/.aether/versions/`. The active version is symlinked to `~/.aether/current` (Linux/macOS) or copied to `~/.aether/bin/` (Windows).

`ae version` tells you when a newer release exists, naming it and pointing at
`ae upgrade`. It matters more than it sounds: `ae run` resolves the standard
library from the install rather than from a checkout, so on a toolchain that has
aged the stdlib answers too, and a function added since resolves as undefined.
The same line appears when a build fails, which is where that symptom shows up.
The check asks GitHub at most once a day and caches the answer in
`~/.aether/cache`, so `ae version` still answers offline, and it says nothing at
all when the check cannot be made. Nothing updates itself; the decision is
yours.

`ae version list` asks GitHub what exists and marks the releases it also finds
locally, so it answers "what could I install?". `ae version installed` reads
only the disk, which is the question to ask when your installs have aged out of
the release window (they will show no Status in `list`) or when you are offline.

### Reclaiming disk space

Each release occupies its full size, so a store of several releases gets large.
Two commands prune it:

```bash
ae version remove v0.829.0       # Remove one or more installed releases
ae version gc --dry-run          # Show what gc would remove
ae version gc --keep 3           # Keep the newest 3, remove the rest
ae version gc --keep 3 --dedupe  # ...and share identical files afterwards
ae version dedupe                # Share identical files, remove nothing
```

Both `remove` and `gc` refuse to delete the release you are using, whether
that is the one `current` points at or the one `~/.aether/active_version`
pins, so the worst case is that they decline. Versions are ordered numerically,
not alphabetically, so `--keep 2` on a store containing v0.9.0 and v0.10.0 keeps
v0.10.0.

`dedupe` reclaims space without removing anything, by pointing identical files
at the same storage. Roughly 70% of a multi-version store is byte-identical, and
about 20% of a *single* version is duplicated between `include/aether/` and
`share/aether/`, so it is worth running even with one release installed. It
prefers copy-on-write reflinks (btrfs, XFS with `reflink=1`, APFS) and falls
back to hardlinks; where the filesystem supports neither (FAT, some network
mounts, a Windows account without Developer Mode), it reports that and changes
nothing. It never alters file contents, and it is safe to re-run: a second pass
finds nothing left to share.

One reporting quirk worth knowing: after a reflink dedupe, `du` still reports
the old size, because it counts every reflinked file at full size even though
the space really has come back. `df` shows the truth. After a *hardlink* dedupe
`du` drops, because it counts a shared inode once.

## WebAssembly and Embedded Targets

Aether supports cross-compilation to platforms without pthreads or POSIX APIs via the `PLATFORM` Makefile variable:

```bash
# Build for WebAssembly (requires Emscripten SDK)
make stdlib PLATFORM=wasm
# Or force cooperative mode on native for testing:
make stdlib EXTRA_CFLAGS="-DAETHER_NO_THREADING"

# Build for embedded ARM (syntax-check only, requires arm-none-eabi-gcc)
make ci-embedded

# Docker-based cross-compilation (no local toolchain needed):
make docker-ci-wasm        # Emscripten + Node.js execution
make docker-ci-embedded    # ARM Cortex-M4 syntax-check
```

On threadless platforms, the cooperative scheduler (`aether_scheduler_coop.c`) replaces the multi-core scheduler. All actors run on a single thread via `aether_scheduler_poll()`. Multi-actor programs work correctly, messages are processed cooperatively during `wait_for_idle()`.

Stdlib modules that depend on filesystem or networking return errors gracefully (NULL, 0, -1). Console I/O (`print`, `println`) always works.

### ESP32 (Xtensa)

The generated C builds as an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) component for the ESP32 (the chip on boards such as the ESP32-2432S028R "cheap yellow display"): ESP-IDF's `xtensa-esp32-elf-gcc` compiles it with the runtime, using the cooperative scheduler, and the generated C supplies the `app_main()` entry point ESP-IDF calls. `tests/esp32/project/` is a working project; its `main/CMakeLists.txt` lists the runtime sources and the settings an ESP32 needs, notably smaller scheduler tables (`MAX_CORES=1 QUEUE_SIZE=64 SCHEDULER_DRAIN_BATCH=32 MAX_ACTORS_PER_CORE=256`; at the defaults they need about 10 MB, against roughly 320 KB of DRAM).

```bash
make ci-esp32   # needs podman or docker; pulls espressif/idf
```

builds a few programs (hello-world, a stateful actor, an actor pipeline, ping-pong) into firmware and boots each in Espressif's QEMU, checking the console output. A program is about 150 KB of firmware. Not yet covered: threads (FreeRTOS has pthreads, so the multicore scheduler is a candidate), the filesystem and networking, and real hardware.

## Troubleshooting

### Build Failures

**"Aether compiler not found" (Windows)**
- Make sure you **restarted your terminal** after adding `C:\aether\bin` to PATH
- Verify: `where ae` should show `C:\aether\bin\ae.exe`
- Verify: `where aetherc` should show `C:\aether\bin\aetherc.exe`
- If both exist but it still fails, set `AETHER_HOME`: `set AETHER_HOME=C:\aether`

**"gcc: command not found" (Windows)**
- This should not happen with the pre-built binary, `ae` auto-downloads GCC (~80 MB) on first run
- If you built from source via MSYS2, open the "MSYS2 MinGW 64-bit" shell, not plain PowerShell
- Verify: `gcc --version` should show MinGW-w64 GCC

**"pthread.h: No such file or directory"**
- Linux: `sudo apt-get install libpthread-stubs0-dev`
- Windows: The Aether runtime uses Win32 threads natively, no pthread library needed

**A function from the standard library is undefined**
- `ae run` reads the standard library of the installed toolchain: run `ae version`, and `ae upgrade` when it names a newer release.

**Test failures (building from source)**
- Run `make test` for unit tests, `make test-ae` for integration tests
- Check for port conflicts if network tests fail (port 8080)

### Quick tips when you hit trouble

1. Building from source and anything looks stale: rebuild with `make clean && make`.
2. On Windows, launch `ae` from a directory where your user can write (the first run downloads GCC into `~\.aether\`).
3. Inside an actor body, `state` is a reserved keyword, pick a different local-variable name; outside actor bodies it has no special meaning.

### Platform-Specific Notes

**macOS:**
- May need `xcode-select --install` for command line tools
- Homebrew GCC recommended: `brew install gcc`
- Apple Silicon (M1/M2/M3): Runtime auto-detects P-cores for consistent performance
- Thread affinity is advisory on macOS; occasional benchmark variance is normal

**Linux:**
- Kernel 4.14+ recommended for full NUMA support
- AddressSanitizer may require `gcc-multilib` on some distributions
- Each scheduler thread is bound to one of the CPUs the process may use (its `taskset` or cpuset mask), and a container's CPU quota limits how many threads start

**Windows:**
- Pre-built binaries work in any terminal (PowerShell, CMD, Windows Terminal)
- GCC is auto-downloaded on first `ae run` no MSYS2 or manual setup required
- The runtime uses Win32 threads natively, no pthreads library required
- Each scheduler thread gets an ideal processor (`SetThreadIdealProcessor`), a hint: Windows keeps it there and moves it when another program is using that CPU
- **Building from source:** Use MSYS2 MinGW 64-bit shell with `make ae`
