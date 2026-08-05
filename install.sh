#!/usr/bin/env bash
# Aether Language Installer
# Usage: ./install.sh              (installs to ~/.aether)
#        ./install.sh /usr/local   (installs to /usr/local, needs sudo)
#        ./install.sh --editor-only (installs only editor extension)
set -eo pipefail

# Always run from the repository root (where this script lives)
cd "$(dirname "$0")"

# First line of a command's combined output.
#
# Do NOT write `cmd --version | head -1` here: with `set -o pipefail` above,
# `head` exits after one line, the producer then dies of SIGPIPE, and the
# pipeline reports THAT failure even though the read succeeded. It depends on
# whether the producer finished writing before `head` closed the pipe, so it
# fails intermittently and only under load, which cost a green CI run once
# already: the GNU-make probe rejected `GNU Make 4.3` while its own error
# message printed that same string back.
#
# A command substitution has no pipe to break, so nothing gets signalled. A
# non-zero exit is tolerated because callers only want whatever was printed.
first_line() {
    local out
    out=$("$@" 2>&1) || true
    printf '%s\n' "${out%%$'\n'*}"
}

# Handle --editor-only flag
if [ "$1" = "--editor-only" ]; then
    EDITOR_ONLY=1
    INSTALL_DIR="$HOME/.aether"
else
    EDITOR_ONLY=0
    INSTALL_DIR="${1:-$HOME/.aether}"
fi
BIN_DIR="$INSTALL_DIR/bin"
LIB_DIR="$INSTALL_DIR/lib/aether"

# Touching the user's shell rc files (`~/.zshrc`, `~/.bashrc`, etc.)
# is appropriate ONLY when installing to the canonical user prefix
# (`$HOME/.aether`). When a caller passes an explicit override path —
# `make test-install` does this with a `mktemp -d` tmpdir, package
# managers may pass `/usr/local/aether`, CI may pass an isolated
# build root — we MUST NOT rewrite the user's PATH/AETHER_HOME to
# point at the override. The tmpdir gets deleted; the package
# manager owns its own PATH-setup story; the user's daily-driver
# environment shouldn't be a side-effect of someone else's test or
# build. Set explicitly here so every downstream branch can short-
# circuit on the same flag.
UPDATE_SHELL_RC=0
if [ "$INSTALL_DIR" = "$HOME/.aether" ]; then
    UPDATE_SHELL_RC=1
fi
# Caller can force-suppress the rc touch even at the default prefix
# (e.g. CI imaging a base layout into a fresh user's $HOME without
# their consent). Mirrors AETHER_NO_HELP_HINT's opt-out shape.
if [ -n "$AETHER_INSTALL_NO_RC" ]; then
    UPDATE_SHELL_RC=0
fi
INCLUDE_DIR="$INSTALL_DIR/include/aether"
SRC_DIR="$INSTALL_DIR/share/aether"

# Colors (if terminal supports it)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    RED='\033[0;31m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    GREEN='' YELLOW='' RED='' BOLD='' NC=''
fi

info()  { printf "${BOLD}%s${NC}\n" "$1"; }
ok()    { printf "${GREEN}%s${NC}\n" "$1"; }
warn()  { printf "${YELLOW}%s${NC}\n" "$1"; }
error() { printf "${RED}%s${NC}\n" "$1"; }

if [ "$EDITOR_ONLY" -eq 0 ]; then
    # Detect Linux distribution
    detect_linux_distro() {
        if [ -f /etc/os-release ]; then
            . /etc/os-release
            echo "$ID"
        elif [ -f /etc/debian_version ]; then
            echo "debian"
        elif [ -f /etc/redhat-release ]; then
            echo "rhel"
        elif [ -f /etc/arch-release ]; then
            echo "arch"
        else
            echo "unknown"
        fi
    }

    # Get install command for the current OS
    get_install_hint() {
        case "$(uname -s)" in
            Darwin)
                echo "  macOS: xcode-select --install"
                echo "     Or: brew install gcc make"
                ;;
            Linux)
                distro=$(detect_linux_distro)
                case "$distro" in
                    ubuntu|debian|pop|mint|elementary)
                        echo "  Debian/Ubuntu: sudo apt-get install build-essential"
                        ;;
                    fedora)
                        echo "  Fedora: sudo dnf install gcc make"
                        ;;
                    rhel|centos|rocky|almalinux)
                        echo "  RHEL/CentOS: sudo yum install gcc make"
                        ;;
                    arch|manjaro|endeavouros)
                        echo "  Arch Linux: sudo pacman -S base-devel"
                        ;;
                    opensuse*)
                        echo "  openSUSE: sudo zypper install gcc make"
                        ;;
                    alpine)
                        echo "  Alpine: apk add build-base"
                        ;;
                    void)
                        echo "  Void Linux: sudo xbps-install -S base-devel"
                        ;;
                    gentoo)
                        echo "  Gentoo: emerge sys-devel/gcc sys-devel/make"
                        ;;
                    *)
                        echo "  Linux: Install gcc and make using your package manager"
                        echo "         Common packages: build-essential, base-devel, or gcc + make"
                        ;;
                esac
                ;;
            MINGW*|MSYS*|CYGWIN*)
                echo "  Windows: Install MinGW-w64 from https://www.mingw-w64.org/"
                echo "           Add MinGW bin directory to PATH"
                echo "           Use: mingw32-make ae"
                ;;
            *)
                echo "  Install GCC (or Clang) and GNU Make for your platform"
                ;;
        esac
    }

    # Check prerequisites
    info "Checking prerequisites..."

    MISSING_DEPS=""

    if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
        MISSING_DEPS="C compiler (gcc, clang, or cc)"
    fi

    if ! command -v make >/dev/null 2>&1 && ! command -v mingw32-make >/dev/null 2>&1; then
        if [ -n "$MISSING_DEPS" ]; then
            MISSING_DEPS="$MISSING_DEPS, make"
        else
            MISSING_DEPS="make"
        fi
    fi

    # Detect make command. The Makefile is GNU make syntax (ifeq, $(shell),
    # pattern rules), so we need GNU make specifically. Prefer `gmake` when it
    # exists — it is the GNU make binary on every BSD, and on Linux `gmake` and
    # `make` are the same program. Falling back to bare `make` picks BSD make on
    # FreeBSD/*BSD, which then dies with a confusing "Invalid line ifeq" parse
    # error mid-build (asks/install-sh-picks-bsd-make-on-freebsd.md).
    if command -v gmake >/dev/null 2>&1; then
        MAKE_CMD="gmake"
    elif command -v make >/dev/null 2>&1; then
        MAKE_CMD="make"
    elif command -v mingw32-make >/dev/null 2>&1; then
        MAKE_CMD="mingw32-make"
    fi

    if [ -n "$MISSING_DEPS" ]; then
        error "Error: Missing prerequisites: $MISSING_DEPS"
        echo ""
        echo "Install the required tools:"
        get_install_hint
        echo ""
        echo "After installing, run this script again:"
        echo "  $0"
        exit 1
    fi

    # Resolve the C compiler we'll hand to make. The Makefile hardcodes
    # `CC := gcc` (Makefile:136), but FreeBSD/macOS ship no gcc — the system
    # compiler is `cc` (-> clang). Prefer an explicit $CC, else gcc, else the
    # POSIX `cc`, else clang, and pass it through as `CC=` so the build uses a
    # compiler that actually exists (asks/install-sh-picks-bsd-make-on-freebsd.md
    # ask 3). `cc` is gcc on Linux and clang on the BSDs/macOS.
    if [ -n "${CC:-}" ]; then
        CC_BIN="$CC"
    elif command -v gcc >/dev/null 2>&1; then
        CC_BIN="gcc"
    elif command -v cc >/dev/null 2>&1; then
        CC_BIN="cc"
    elif command -v clang >/dev/null 2>&1; then
        CC_BIN="clang"
    else
        CC_BIN="cc"
    fi
    CC_VERSION=$(first_line "$CC_BIN" --version)
    ok "  C compiler: $CC_VERSION  (CC=$CC_BIN)"

    # Verify the chosen make is GNU make, not just *a* make. On BSD, `make` is
    # BSD make, which cannot parse this GNU Makefile; catch that here with an
    # actionable message instead of a mid-build "Invalid line ifeq" parse error.
    # (mingw32-make on Windows is GNU make, so it passes this probe.)
    #
    # The banner is captured once and both the test and the message read that
    # one value, so they can never contradict each other.
    MAKE_VERSION=$(first_line "$MAKE_CMD" --version)
    case "$MAKE_VERSION" in
        *[Gg][Nn][Uu]\ [Mm]ake*) ;;
        *)
            error "Error: '$MAKE_CMD' is not GNU make (the Makefile is GNU make syntax)."
            echo "  Found: $MAKE_VERSION"
            echo "  Install GNU make and re-run:"
            echo "    FreeBSD/*BSD: pkg install gmake   (then this script prefers gmake)"
            echo "    Linux:        it is already 'make'"
            exit 1
            ;;
    esac
    ok "  make: $MAKE_VERSION"
    echo ""

    # Fetch latest tags so the Makefile picks up the correct version number.
    # Without this, make clean + rebuild uses stale local tags (e.g. 0.22.0 instead of 0.25.0).
    if git rev-parse --git-dir > /dev/null 2>&1; then
        info "Fetching latest tags (ensures correct version number)..."
        git fetch --tags --quiet 2>/dev/null || true
    fi

    # Build. Pass CC= explicitly so a box with no gcc (FreeBSD/macOS) uses the
    # resolved system compiler instead of the Makefile's hardcoded `gcc`.
    info "Building Aether..."
    $MAKE_CMD CC="$CC_BIN" compiler 2>&1 | tail -1
    $MAKE_CMD CC="$CC_BIN" ae 2>&1 | tail -1

    # Build precompiled stdlib
    info "Building standard library..."
    $MAKE_CMD CC="$CC_BIN" stdlib 2>&1 | tail -1

    # Build the language server too so the editor extension's LSP
    # client can wire up out of the box (go-to-def, hover,
    # diagnostics). `make lsp` is fast — it links the already-built
    # libaether_compiler.a archive — so building it unconditionally
    # is cheap. If it fails (older toolchain, sandbox without libgcc
    # on a stripped Linux container, etc.) we don't abort the
    # install: the editor extension falls back to syntax-only mode
    # and the user still has a working `ae` / `aetherc`.
    info "Building language server..."
    if ! $MAKE_CMD CC="$CC_BIN" lsp 2>&1 | tail -1; then
        warn "  lsp build failed — editor extension will fall back to syntax-only mode."
    fi

    # Install
    info "Installing to $INSTALL_DIR..."

    mkdir -p "$BIN_DIR" "$LIB_DIR" "$INCLUDE_DIR" "$SRC_DIR"

    # Detect Windows exe extension
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
        *) EXE="" ;;
    esac

    # Binaries
    cp "build/ae${EXE}" "$BIN_DIR/ae${EXE}"
    cp "build/aetherc${EXE}" "$BIN_DIR/aetherc${EXE}"
    chmod 755 "$BIN_DIR/ae${EXE}" "$BIN_DIR/aetherc${EXE}"

    # Language server, if it has been built. The editor extension
    # auto-wires the LSP by looking for `aether-lsp` on PATH (or via
    # the `aether.lsp.path` setting) and falls back to syntax-only
    # mode when it's not found. Shipping it alongside ae/aetherc
    # means the editor's "go to definition" / hover / diagnostics
    # work out of the box without the user having to know about it.
    # Optional: if `make lsp` wasn't run before `make install`, the
    # file is absent and we skip silently — that's the documented
    # syntax-only fallback, not a broken install.
    if [ -f "build/aether-lsp${EXE}" ]; then
        cp "build/aether-lsp${EXE}" "$BIN_DIR/aether-lsp${EXE}"
        chmod 755 "$BIN_DIR/aether-lsp${EXE}"
    fi

    # macOS: remove quarantine attribute so Gatekeeper doesn't block unsigned binaries
    if [ "$(uname -s)" = "Darwin" ]; then
        xattr -cr "$BIN_DIR/ae${EXE}" "$BIN_DIR/aetherc${EXE}" 2>/dev/null || true
        [ -f "$BIN_DIR/aether-lsp${EXE}" ] && \
            xattr -cr "$BIN_DIR/aether-lsp${EXE}" 2>/dev/null || true
    fi

    # Precompiled library
    if [ -f build/libaether.a ]; then
        cp build/libaether.a "$LIB_DIR/libaether.a"
    fi

    # Headers (preserve directory structure for relative includes).
    # Whole-tree walk rather than per-subdir enumeration so new
    # modules added under std/ or new subdirs under std/http/ etc.
    # don't silently fall off the install.
    mkdir -p "$INCLUDE_DIR"
    # The public embedder header lives in include/, outside the runtime/ and
    # std/ trees walked below, so it shipped in no install at all and
    # runtime/libaether_caps.c could not find it when cross-compiling (#1420).
    cp include/*.h "$INCLUDE_DIR/" 2>/dev/null || true
    (cd runtime && find . -name '*.h' -print) | while read -r h; do
        mkdir -p "$INCLUDE_DIR/runtime/$(dirname "$h")"
        cp "runtime/$h" "$INCLUDE_DIR/runtime/$h" 2>/dev/null || true
    done
    (cd std && find . -name '*.h' -print) | while read -r h; do
        mkdir -p "$INCLUDE_DIR/std/$(dirname "$h")"
        cp "std/$h" "$INCLUDE_DIR/std/$h" 2>/dev/null || true
    done

    # Runtime + stdlib source (sources are the fallback the compiler
    # falls through to for relinking; module.ae descriptors are what
    # the resolver looks up on `import std.X`). Whole-tree copy means
    # every module's full content lands — including Aether-only
    # modules (file, dir, path, list, map, host, intarr, tcp) whose
    # only payload is module.ae.
    mkdir -p "$SRC_DIR"
    cp -r runtime "$SRC_DIR/" 2>/dev/null || true
    cp -r std     "$SRC_DIR/" 2>/dev/null || true
    # Contrib module.ae descriptors + headers (issue #334). With these
    # in place, `import contrib.X` resolves the same way `import std.X`
    # does — share/aether/contrib/<X>/module.ae sits next to
    # share/aether/std/<X>/module.ae. The matching .a archives are
    # built+installed separately by `make install-contrib`, which
    # probes for system dependencies (sqlite3-dev, etc.).
    cp -r contrib "$SRC_DIR/" 2>/dev/null || true
    # Trim source-tree noise from contrib: tests, benchmarks, example
    # .ae, build/CI scripts, and the .c/.m files (those compile into
    # the libaether_<x>.a archives via `make contrib`; no value in
    # also shipping the source).
    find "$SRC_DIR/contrib" -type d -name tests       -exec rm -rf {} + 2>/dev/null || true
    find "$SRC_DIR/contrib" -type d -name benchmarks  -exec rm -rf {} + 2>/dev/null || true
    find "$SRC_DIR/contrib" -type f -name 'example_*.ae' -delete 2>/dev/null || true
    find "$SRC_DIR/contrib" -type f -name 'test_*.ae' -delete 2>/dev/null || true
    find "$SRC_DIR/contrib" -type f -name 'test_*.sh' -delete 2>/dev/null || true
    find "$SRC_DIR/contrib" -type f -name 'build.sh'  -delete 2>/dev/null || true
    find "$SRC_DIR/contrib" -type f -name 'ci.sh'     -delete 2>/dev/null || true
    # Keep contrib/host/<lang>/aether_host_<lang>.c — plain install
    # doesn't ship libaether_host_<lang>.a, so downstream apps that
    # `import contrib.host.<lang>` compile the bridge from source.
    # See docs/install-layout.md "What does NOT ship" for context.
    find "$SRC_DIR/contrib" -type f -name '*.c' \
        ! -path '*/contrib/host/*/aether_host_*.c' -delete 2>/dev/null || true
    find "$SRC_DIR/contrib" -type f -name '*.m' -delete 2>/dev/null || true
    # Trim install-noise that confuses external consumers
    # (aetherBuild and the like). runtime/examples/ holds standalone
    # benches with their own main() — never link-suitable.
    # runtime/io/ is an orphaned poller hub; the active poller
    # variants live under runtime/scheduler/. Both trip naive
    # `find runtime -name '*.c'` consumers.
    rm -rf "$SRC_DIR/runtime/examples" 2>/dev/null || true
    rm -rf "$SRC_DIR/runtime/io"       2>/dev/null || true
    # Authoritative MANIFEST listing link-suitable .c files (#329).
    # Downstream consumers (aetherBuild's aeb-link et al.) read this
    # instead of trying to enumerate via `find` — the latter pulls
    # in benchmarks / orphan poller hubs / etc. that aren't link-
    # suitable. Generated by `make stdlib` above.
    if [ -f build/MANIFEST ]; then
        install -m 644 build/MANIFEST "$SRC_DIR/MANIFEST" 2>/dev/null \
            || cp build/MANIFEST "$SRC_DIR/MANIFEST" 2>/dev/null || true
    fi

    # Register installed version in ~/.aether/versions/ so that:
    # - 'ae version list' shows it as "installed"
    # - 'ae version use vX.Y.Z' can switch back after trying another version
    # Get version from the VERSION file (not from ae binary, which may read stale active_version)
    if [ -f VERSION ]; then
        INSTALLED_VER=$(cat VERSION | tr -d '[:space:]')
    else
        INSTALLED_VER=""
        if [[ $(first_line "$BIN_DIR/ae" --help) =~ [0-9]+\.[0-9]+\.[0-9]+ ]]; then
            INSTALLED_VER="${BASH_REMATCH[0]}"
        fi
    fi
    if [ -n "$INSTALLED_VER" ] && [ "$INSTALLED_VER" != "0.0.0-dev" ]; then
        VTAG="v$INSTALLED_VER"
        VER_ENTRY="$INSTALL_DIR/versions/$VTAG"
        mkdir -p "$INSTALL_DIR/versions"

        # Copy the full install into versions/ so 'ae version use' works.
        # Remove old entry first (may be stale from a previous build).
        rm -rf "$VER_ENTRY"
        mkdir -p "$VER_ENTRY"
        for subdir in bin lib include share; do
            if [ -d "$INSTALL_DIR/$subdir" ]; then
                cp -r "$INSTALL_DIR/$subdir" "$VER_ENTRY/$subdir"
            fi
        done
        for f in VERSION LICENSE README.md; do
            if [ -f "$INSTALL_DIR/$f" ]; then
                cp -f "$INSTALL_DIR/$f" "$VER_ENTRY/$f" 2>/dev/null || true
            fi
        done

        # Write active_version marker and update current symlink.
        # `current` is a symlink on POSIX; on Windows MinGW, `ln -sf`
        # silently degrades to a directory copy (no admin / developer
        # mode = no real symlinks), so on a re-install we may find
        # `current` as a real directory rather than a link. `rm -rf`
        # handles both cases; without it, `set -eo pipefail` would
        # abort here on the second install ("rm: cannot remove
        # 'current': Is a directory") and the PATH-setup block
        # below would never run.
        echo "$INSTALLED_VER" > "$INSTALL_DIR/active_version"
        rm -rf "$INSTALL_DIR/current"
        ln -sf "$VER_ENTRY" "$INSTALL_DIR/current" 2>/dev/null \
            || cp -r "$VER_ENTRY" "$INSTALL_DIR/current"
    else
        # Fallback: just remove stale symlink/directory
        rm -rf "$INSTALL_DIR/current" 2>/dev/null || true
    fi

    ok "  Installed successfully"
    echo ""

    # PATH setup — only when installing to the canonical user prefix.
    # See the `UPDATE_SHELL_RC` discussion at the top of this file for
    # why test-install / package-manager invocations must NOT rewrite
    # the operator's shell rc.
    if [ "$UPDATE_SHELL_RC" -eq 0 ]; then
        info "Skipping shell rc update (install prefix is not the default \$HOME/.aether — or AETHER_INSTALL_NO_RC was set)."
        echo "  Set PATH manually to use this install: export PATH=\"$BIN_DIR:\$PATH\""
        echo ""
        # Skip verification too — without rc update we can't promise the
        # operator's next shell sees the new binary, and that's the
        # contract `info "Verifying installation..."` implies. Do a
        # quiet probe instead.
        if "$BIN_DIR/ae" version >/dev/null 2>&1; then
            ok "  binary verified at $BIN_DIR/ae"
        else
            error "  binary at $BIN_DIR/ae did not respond to 'version'"
            exit 1
        fi
        echo ""
        echo "========================================="
        ok "  Aether installed successfully (no shell rc touched)!"
        echo "========================================="
        exit 0
    fi
    SHELL_NAME="$(basename "$SHELL")"
    EXPORT_LINE="export PATH=\"$BIN_DIR:\$PATH\""
    AETHER_HOME_LINE="export AETHER_HOME=\"$INSTALL_DIR\""

    # Detect Windows (MSYS2 / MinGW / Cygwin / Git Bash). On Windows
    # the persistent PATH lives in the Windows registry — updating
    # ~/.bash_profile alone reaches only login bash and misses every
    # other shell (PowerShell, cmd.exe, VS Code's terminal, the
    # interactive bash-shell that doesn't source .bash_profile).
    # `setx PATH ...` writes to the user-level Windows env block,
    # which propagates to all future shells regardless of which
    # one the user picks.
    IS_WINDOWS=0
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
    esac

    IN_PATH=0
    case ":$PATH:" in
        *":$BIN_DIR:"*) IN_PATH=1 ;;
    esac
    # On Windows, also probe the registry-persisted user PATH —
    # the in-process $PATH may not yet reflect a newly-applied
    # setx from a prior install.
    if [ "$IS_WINDOWS" -eq 1 ] && [ "$IN_PATH" -eq 0 ]; then
        BIN_DIR_WIN="$(cygpath -w "$BIN_DIR" 2>/dev/null || echo "$BIN_DIR")"
        # PowerShell -Command can be slow but is the only reliable
        # way to read the Windows User-scope PATH from MSYS2 bash.
        WIN_USER_PATH="$(powershell.exe -NoProfile -Command "[Environment]::GetEnvironmentVariable('PATH','User')" 2>/dev/null | tr -d '\r' | tr -d '\n')"
        case ";$WIN_USER_PATH;" in
            *";$BIN_DIR_WIN;"*) IN_PATH=1 ;;
        esac
    fi

    if [ "$IS_WINDOWS" -eq 1 ]; then
        info "Setting up Windows user PATH (visible in PowerShell + cmd.exe + future bash)..."
        BIN_DIR_WIN="$(cygpath -w "$BIN_DIR" 2>/dev/null || echo "$BIN_DIR")"
        INSTALL_DIR_WIN="$(cygpath -w "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")"
        # Read the current user PATH, prepend our bin dir if absent,
        # and write back via setx. Reading via PowerShell avoids
        # mangling that `setx PATH "%PATH%;..."` would do (which
        # snapshots the *expanded* system+user PATH into user).
        CUR_USER_PATH="$(powershell.exe -NoProfile -Command "[Environment]::GetEnvironmentVariable('PATH','User')" 2>/dev/null | tr -d '\r' | tr -d '\n')"
        case ";$CUR_USER_PATH;" in
            *";$BIN_DIR_WIN;"*)
                ok "  Already in Windows User PATH: $BIN_DIR_WIN"
                ;;
            *)
                if [ -z "$CUR_USER_PATH" ]; then
                    NEW_USER_PATH="$BIN_DIR_WIN"
                else
                    NEW_USER_PATH="$BIN_DIR_WIN;$CUR_USER_PATH"
                fi
                if powershell.exe -NoProfile -Command "[Environment]::SetEnvironmentVariable('PATH','$NEW_USER_PATH','User')" 2>/dev/null; then
                    ok "  Added $BIN_DIR_WIN to Windows User PATH"
                else
                    warn "  Could not update Windows User PATH; add $BIN_DIR_WIN manually:"
                    echo "       setx PATH \"$BIN_DIR_WIN;%PATH%\""
                fi
                ;;
        esac
        # Also set AETHER_HOME at the Windows level so child processes
        # and other shells see it.
        powershell.exe -NoProfile -Command "[Environment]::SetEnvironmentVariable('AETHER_HOME','$INSTALL_DIR_WIN','User')" 2>/dev/null \
            && ok "  Set AETHER_HOME=$INSTALL_DIR_WIN" \
            || warn "  Could not set Windows AETHER_HOME"
        echo ""
        # Don't touch shell rc files on Windows: the Windows PATH
        # update covers MSYS2 bash too (Windows env propagates into
        # the bash subshell environment). Touching .bash_profile
        # here would only add a duplicate entry on every login.
        IN_PATH=1
    fi

    if [ "$IN_PATH" -eq 0 ]; then
        info "Setting up PATH..."

        # Detect shell config file
        SHELL_RC=""
        IS_FISH=0
        case "$SHELL_NAME" in
            zsh)  SHELL_RC="$HOME/.zshrc" ;;
            bash) SHELL_RC="$HOME/.bash_profile" ;;
            fish) SHELL_RC="$HOME/.config/fish/config.fish"; IS_FISH=1 ;;
        esac

        if [ -n "$SHELL_RC" ]; then
            # Ensure parent directory exists (fish config may not exist yet)
            mkdir -p "$(dirname "$SHELL_RC")"

            if grep -q "AETHER_HOME" "$SHELL_RC" 2>/dev/null; then
                # Update existing AETHER_HOME and PATH to point to the new install dir
                if [ "$IS_FISH" -eq 1 ]; then
                    sed -i.bak "s|set -gx AETHER_HOME .*|set -gx AETHER_HOME \"$INSTALL_DIR\"|" "$SHELL_RC"
                    sed -i.bak "s|fish_add_path .*aether.*|fish_add_path \"$BIN_DIR\"|" "$SHELL_RC"
                    # Append the PATH line if the previous run never wrote it
                    # (rare: rc file edited by hand between installs, or an
                    # earlier installer release that only wrote AETHER_HOME).
                    if ! grep -qE 'fish_add_path[^#]*aether' "$SHELL_RC" 2>/dev/null; then
                        echo "fish_add_path \"$BIN_DIR\"" >> "$SHELL_RC"
                    fi
                else
                    sed -i.bak "s|export AETHER_HOME=.*|$AETHER_HOME_LINE|" "$SHELL_RC"
                    # Match only PATH lines that start with the aether bin dir (not lines
                    # that happen to contain "aether" alongside other unrelated entries)
                    sed -i.bak "s|export PATH=\".*aether.*/bin:\\\$PATH\"|$EXPORT_LINE|" "$SHELL_RC"
                    # Self-heal: if the rc file has AETHER_HOME but no aether
                    # PATH entry (early-installer bug, hand-edited rc, or the
                    # user deleted the PATH line by accident), APPEND it. Without
                    # this, the post-install verification succeeds (the script
                    # invokes "$BIN_DIR/ae" by absolute path) but the user's
                    # next shell still can't find `ae` and the install looks
                    # broken from the operator's seat.
                    if ! grep -qE 'export PATH=.*\.?aether.*/bin' "$SHELL_RC" 2>/dev/null; then
                        echo "$EXPORT_LINE" >> "$SHELL_RC"
                        ok "  Backfilled missing PATH entry in $SHELL_RC"
                    fi
                fi
                rm -f "$SHELL_RC.bak"
                ok "  Updated AETHER_HOME in $SHELL_RC"
            else
                # Fresh install -- append new block
                if [ -f "$SHELL_RC" ] && [ -s "$SHELL_RC" ]; then
                    if [ "$(tail -c 1 "$SHELL_RC" | wc -l)" -eq 0 ]; then
                        printf '\n' >> "$SHELL_RC"
                    fi
                fi
                echo "" >> "$SHELL_RC"
                echo "# Aether Language" >> "$SHELL_RC"
                if [ "$IS_FISH" -eq 1 ]; then
                    echo "set -gx AETHER_HOME \"$INSTALL_DIR\"" >> "$SHELL_RC"
                    echo "fish_add_path \"$BIN_DIR\"" >> "$SHELL_RC"
                else
                    echo "$AETHER_HOME_LINE" >> "$SHELL_RC"
                    echo "$EXPORT_LINE" >> "$SHELL_RC"
                fi
                ok "  Added to $SHELL_RC"
            fi
        fi
        echo ""
    fi

    # Verify
    info "Verifying installation..."
    if "$BIN_DIR/ae" version >/dev/null 2>&1; then
        VERSION=$("$BIN_DIR/ae" version 2>&1)
        ok "  $VERSION"
    else
        error "  Verification failed"
        exit 1
    fi

    echo ""
    echo "========================================="
    ok "  Aether installed successfully!"
    echo "========================================="
    echo ""

    if [ "$IN_PATH" -eq 0 ] && [ -n "$SHELL_RC" ]; then
        warn "Restart your terminal or run:"
        echo "  source $SHELL_RC"
        echo ""
    elif [ "$IS_WINDOWS" -eq 1 ]; then
        warn "Open a NEW terminal (PowerShell, cmd.exe, or bash) for"
        echo "  the PATH change to take effect. Existing shells won't"
        echo "  see the update — Windows env-var changes propagate to"
        echo "  newly-spawned processes only."
        echo ""
    fi

    echo "Get started:"
    echo "  ae init myproject"
    echo "  cd myproject"
    echo "  ae run"
    echo ""
    echo "Or run a file directly:"
    echo "  ae run hello.ae"
    echo ""
fi

# IDE Extension Installation (optional)
install_editor_extension() {
    local editor_cmd="$1"
    local editor_name="$2"
    local ext_dir="$3"
    local src_dir="$(dirname "$0")/editor/vscode"
    local installer="$src_dir/install.sh"

    if [ ! -d "$src_dir" ] || [ ! -x "$installer" ]; then
        warn "  Extension installer not found at $installer"
        return 1
    fi

    # Delegate to editor/vscode/install.sh — single source of truth for
    # which files ship and what folder name they ship under. Earlier
    # this function maintained its own copy of that logic and silently
    # drifted: it was naming the folder after the project VERSION
    # (e.g. aether-language-0.105.0) and copying only 5 of the 8
    # required files, so a release-built install would shadow the
    # editor-side script's correct install with a folder that was
    # missing themes/, the icon-theme manifest, and the README.
    info "Installing Aether extension for $editor_name..."
    "$installer" "$ext_dir" || return 1
    return 0
}

# Detect all supported editors
EDITORS_FOUND=0

prompt_install_extension() {
    local editor_cmd="$1"
    local editor_name="$2"
    local ext_dir="$3"

    if [ "$EDITOR_ONLY" -eq 1 ]; then
        # Direct install when using --editor-only flag
        install_editor_extension "$editor_cmd" "$editor_name" "$ext_dir"
    elif [ ! -t 0 ]; then
        # Non-interactive (piped/CI) — skip prompt, auto-install
        install_editor_extension "$editor_cmd" "$editor_name" "$ext_dir"
    else
        # Interactive prompt during normal install
        printf "Install Aether syntax highlighting for $editor_name? [y/N] "
        read -r response
        case "$response" in
            [yY]|[yY][eE][sS])
                install_editor_extension "$editor_cmd" "$editor_name" "$ext_dir"
                ;;
            *)
                echo "  Skipped"
                ;;
        esac
    fi
}

# Check for Cursor (command in PATH, or macOS app bundle with extensions dir)
if command -v cursor >/dev/null 2>&1 && [ -d "$HOME/.cursor" ]; then
    [ "$EDITORS_FOUND" -eq 0 ] && echo ""
    info "Detected Cursor"
    prompt_install_extension "cursor" "Cursor" "$HOME/.cursor/extensions"
    EDITORS_FOUND=$((EDITORS_FOUND + 1))
elif [ -d "/Applications/Cursor.app" ] && [ -d "$HOME/.cursor/extensions" ]; then
    [ "$EDITORS_FOUND" -eq 0 ] && echo ""
    info "Detected Cursor (macOS app)"
    prompt_install_extension "cursor" "Cursor" "$HOME/.cursor/extensions"
    EDITORS_FOUND=$((EDITORS_FOUND + 1))
fi

# Check for VS Code (command in PATH, or macOS app bundle with extensions dir)
if command -v code >/dev/null 2>&1 && [ -d "$HOME/.vscode" ]; then
    [ "$EDITORS_FOUND" -eq 0 ] && echo ""
    info "Detected VS Code"
    prompt_install_extension "code" "VS Code" "$HOME/.vscode/extensions"
    EDITORS_FOUND=$((EDITORS_FOUND + 1))
elif [ -d "/Applications/Visual Studio Code.app" ] && [ -d "$HOME/.vscode/extensions" ]; then
    [ "$EDITORS_FOUND" -eq 0 ] && echo ""
    info "Detected VS Code (macOS app)"
    prompt_install_extension "code" "VS Code" "$HOME/.vscode/extensions"
    EDITORS_FOUND=$((EDITORS_FOUND + 1))
fi

# Check for VSCodium (command in PATH, or macOS app bundle with extensions dir)
if command -v codium >/dev/null 2>&1 && [ -d "$HOME/.vscode-oss" ]; then
    [ "$EDITORS_FOUND" -eq 0 ] && echo ""
    info "Detected VSCodium"
    prompt_install_extension "codium" "VSCodium" "$HOME/.vscode-oss/extensions"
    EDITORS_FOUND=$((EDITORS_FOUND + 1))
elif [ -d "/Applications/VSCodium.app" ] && [ -d "$HOME/.vscode-oss/extensions" ]; then
    [ "$EDITORS_FOUND" -eq 0 ] && echo ""
    info "Detected VSCodium (macOS app)"
    prompt_install_extension "codium" "VSCodium" "$HOME/.vscode-oss/extensions"
    EDITORS_FOUND=$((EDITORS_FOUND + 1))
fi

# Show skip message only once at the end (for normal install)
if [ "$EDITORS_FOUND" -gt 0 ] && [ "$EDITOR_ONLY" -eq 0 ]; then
    echo ""
    echo "You can reinstall editor extensions later with:"
    echo "  $0 --editor-only"
fi

# Error if --editor-only but no editors found
if [ "$EDITORS_FOUND" -eq 0 ] && [ "$EDITOR_ONLY" -eq 1 ]; then
    error "No supported editor detected."
    echo ""
    echo "Supported editors: VS Code, Cursor, VSCodium"
    echo "Make sure the editor is installed and its CLI command is in PATH:"
    echo "  - VS Code: 'code' command (install from Command Palette)"
    echo "  - Cursor: 'cursor' command"
    echo "  - VSCodium: 'codium' command"
    exit 1
fi
