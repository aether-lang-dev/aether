#!/usr/bin/env bash
# contrib_build.sh — build static libs for contrib modules.
#
# Two modes:
#
# 1. Default (MODULES unset) — best-effort: probe every contrib module
#    (sqlite + host bridges). For each available dep, compile + archive.
#    Skip + tally any whose dev libs aren't installed. Same shape as
#    historical behaviour; convenient for dev boxes.
#
# 2. MODULES=<list> (comma-separated) — explicit, fail-hard: build ONLY
#    the named modules; any requested module that can't compile is a
#    hard failure (exit 1), not a silent skip. This is the mode the
#    `aether-build --with=<lang>` path uses, where the caller has already
#    `apt install`ed the matching -dev kit and expects the bridge to
#    build. Silent SKIP in that path is the v0.209.0 footgun
#    (ctr_notes.md Bug 6) we're closing here.
#
# Examples:
#   MODULES=python                    # build only host_python; fail if it can't
#   MODULES=python,lua                # build python AND lua; fail if either can't
#   (unset)                           # build-all, tolerate skips
#
# Writes build/contrib/MANIFEST listing one line per built module:
#     <module>\t<archive_path>
# `make install-contrib` reads this manifest to know what to ship.
#
# Called from `make contrib` (default mode) or
# `make contrib MODULES=<list>` (explicit mode).

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/contrib"
mkdir -p "$OUT"

CC="${CC:-gcc}"
BASE_CFLAGS=(
    -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -DAETHER_HAS_SANDBOX
    -I"$ROOT" -I"$ROOT/runtime" -I"$ROOT/runtime/actors"
    -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils"
    -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config"
    -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/io"
    -I"$ROOT/std/math" -I"$ROOT/std/net" -I"$ROOT/std/collections"
    -I"$ROOT/std/json"
)

# --- cross-compile (target/sysroot) mode -------------------------------------
# When CONTRIB_TARGET is set, build the contrib archives for a FOREIGN target
# with `zig cc -target <triple>`, mirroring what #1208 did to ae.c's
# run_cross_build. Two sysroots feed it (both optional per tier):
#   AETHER_SYSROOT     — the OS base sysroot (FreeBSD needs this; provides libc
#                        headers/libs). Same var + flag recipe as ae.c #1208.
#   CROSSBUILD_SYSROOT — aether-crossbuild's sysroots/<triple>/, holding the
#                        Tier-2/3 deps (openssl/zlib/nghttp2/pcre2, and — once
#                        recipes/sqlite.sh lands — sqlite3) as .a + headers.
# Tier 1 modules (no external C dep) need neither and compile like core.
CONTRIB_TARGET="${CONTRIB_TARGET:-}"
CROSS_MODE=""
CROSS_ARCH_WANT=""     # a `file`(1) substring the produced archive must match
if [ -n "$CONTRIB_TARGET" ]; then
    CROSS_MODE=1
    command -v zig >/dev/null 2>&1 || {
        echo "Error: CONTRIB_TARGET=$CONTRIB_TARGET needs zig on PATH." >&2
        exit 1
    }
    CC="zig cc -target $CONTRIB_TARGET"

    # FreeBSD (Tier B) base sysroot — verbatim from #1208: --sysroot ALONE does
    # not make zig search a FreeBSD sysroot's include/lib, so -I/-L are explicit.
    # Archiving needs only the COMPILE flags (no CRT/libc.so.7 link dance).
    case "$CONTRIB_TARGET" in
        *freebsd*)
            if [ -z "${AETHER_SYSROOT:-}" ]; then
                echo "Error: $CONTRIB_TARGET needs a FreeBSD base sysroot, but AETHER_SYSROOT is unset." >&2
                echo "  Provision one with aether-crossbuild (scripts/fetch-freebsd-base.sh <cpu> [major])" >&2
                echo "  then set AETHER_SYSROOT=<crossbuild>/bases/<cpu>-freebsd[ver]." >&2
                exit 1
            fi
            BASE_CFLAGS+=(
                "--sysroot=$AETHER_SYSROOT"
                -I"$AETHER_SYSROOT/usr/include"
                -L"$AETHER_SYSROOT/usr/lib" -L"$AETHER_SYSROOT/lib"
            )
            CROSS_ARCH_WANT="FreeBSD"
            ;;
        *linux*)   CROSS_ARCH_WANT="ELF" ;;
        *macos*)   CROSS_ARCH_WANT="Mach-O" ;;
        # Windows (Tier A, self-contained like linux): zig bundles the full
        # MinGW-w64 target — no sysroot. A contrib .o member is a PE/COFF object.
        *windows*) CROSS_ARCH_WANT="COFF" ;;
    esac

    # Tier-2/3 deps: point compiles at the crossbuild sysroot, NOT host libs.
    if [ -n "${CROSSBUILD_SYSROOT:-}" ] && [ -d "$CROSSBUILD_SYSROOT" ]; then
        BASE_CFLAGS+=( -I"$CROSSBUILD_SYSROOT/include" -L"$CROSSBUILD_SYSROOT/lib" )
    fi

    echo "  Cross mode: CONTRIB_TARGET=$CONTRIB_TARGET"
    [ -n "${AETHER_SYSROOT:-}" ]     && echo "             AETHER_SYSROOT=$AETHER_SYSROOT"
    [ -n "${CROSSBUILD_SYSROOT:-}" ] && echo "             CROSSBUILD_SYSROOT=$CROSSBUILD_SYSROOT"
fi

# In target mode a dep is "present" iff the crossbuild sysroot carries it — the
# host's pkg-config/system libs are irrelevant to a foreign target. Returns 0
# and echoes the -I include flag when <header> is found under CROSSBUILD_SYSROOT
# (and, for a named lib, its .a too); returns 1 (→ SKIP/FAIL) otherwise. Tier-1
# modules don't call this (no external dep). Tier-2/3 route their probes here.
cross_dep_present() {   # <header-basename> [lib-basename-without-lib/.a]
    local hdr="$1" lib="${2:-}"
    [ -n "${CROSSBUILD_SYSROOT:-}" ] || return 1
    [ -f "$CROSSBUILD_SYSROOT/include/$hdr" ] || return 1
    if [ -n "$lib" ]; then
        [ -f "$CROSSBUILD_SYSROOT/lib/lib$lib.a" ] || \
        [ -f "$CROSSBUILD_SYSROOT/lib/lib$lib.so" ] || return 1
    fi
    echo "-I$CROSSBUILD_SYSROOT/include"
    return 0
}

# probe_<lang> echoes dev-include flags on stdout when the dep is
# available. Returns 0 if available, 1 if not. (Mirror of the probe
# helpers in contrib_host_demos.sh — kept in sync, not sourced, so
# this script stays runnable even if the demos script is renamed.)
#
# Each probe queries the distro's REAL -dev kit via pkg-config or a
# language-specific config tool (python3-config / perl -MExtUtils::Embed).
# The earlier vendored-fallback path
# (`/opt/aether/include/<lang>/` populated from
# hosted-language-headers) was removed when ctr_notes.md Bug 6 proved
# pyconfig.h / luaconf.h / perl.h are distro-generated multiarch
# dispatchers that can't be portably vendored. The aether-build image
# now apt-installs the matching -dev kit per `--with=<lang>` layer.

probe_sqlite() {
    # Cross mode: sqlite3 is Tier 3 (absent from the FreeBSD base and from
    # zig's bundled targets). It's "present" iff aether-crossbuild's
    # recipes/sqlite.sh staged libsqlite3.a + sqlite3.h into CROSSBUILD_SYSROOT.
    # If not, return 1 → the two-mode loop SKIPs (build-all) or FAILs
    # (explicit MODULES=), never emitting a broken archive.
    if [ -n "$CROSS_MODE" ]; then
        cross_dep_present sqlite3.h sqlite3
        return
    fi
    if pkg-config --exists sqlite3 2>/dev/null; then
        pkg-config --cflags-only-I sqlite3
        return 0
    fi
    # Fallback: header in default include path.
    if printf '#include <sqlite3.h>\nint main(){return 0;}\n' | \
        $CC -E -xc - >/dev/null 2>&1; then
        echo ""
        return 0
    fi
    return 1
}

probe_avcodec() {
    # FFmpeg's decode libraries. Video-only shim, but swscale does the RGBA
    # conversion and avutil carries the frame/image helpers, so all four are
    # required together -- a partial install is a SKIP, not a half-build.
    if [ -n "$CROSS_MODE" ]; then
        cross_dep_present libavcodec/avcodec.h avcodec
        return
    fi
    if pkg-config --exists libavcodec libavformat libavutil libswscale libswresample 2>/dev/null; then
        pkg-config --cflags-only-I libavcodec libavformat libavutil libswscale libswresample
        return 0
    fi
    return 1
}

probe_lua() {
    for v in lua5.4 lua5.3 lua; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            return 0
        fi
    done
    return 1
}

probe_python() {
    if command -v python3-config >/dev/null 2>&1; then
        python3-config --includes
        return 0
    fi
    return 1
}

probe_ruby() {
    # pkg-config first, when the distro ships a .pc.
    for v in ruby-3.4 ruby-3.3 ruby-3.2 ruby-3.1 ruby-3.0 ruby; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            return 0
        fi
    done
    # Fall back to asking RUBY ITSELF. Several distributions (CachyOS/Arch
    # among them) install the headers with NO ruby.pc at all, so every
    # pkg-config branch above fails and the bridge silently never builds —
    # observed on the nightly box, where ruby 3.4.10 was installed and
    # `contrib-host-check` still reported "libaether_host_ruby.a
    # unavailable". A hardcoded version list also ages badly: this one
    # stopped at 3.2 while the box had 3.4.
    #
    # rubyhdrdir + rubyarchhdrdir are what ruby's own extension builds use,
    # so they are correct wherever ruby runs. Same principle as the soname
    # probes in the [3/3] spec phase: ask the runtime, do not guess at the
    # packaging.
    if command -v ruby >/dev/null 2>&1; then
        rb_inc="$(ruby -rrbconfig -e 'print RbConfig::CONFIG["rubyhdrdir"]' 2>/dev/null)"
        rb_arch="$(ruby -rrbconfig -e 'print RbConfig::CONFIG["rubyarchhdrdir"]' 2>/dev/null)"
        if [ -n "$rb_inc" ] && [ -f "$rb_inc/ruby.h" ]; then
            printf -- '-I%s' "$rb_inc"
            [ -n "$rb_arch" ] && printf -- ' -I%s' "$rb_arch"
            return 0
        fi
    fi
    return 1
}

probe_perl() {
    if command -v perl >/dev/null 2>&1; then
        perl -MExtUtils::Embed -e ccopts 2>/dev/null && return 0
    fi
    return 1
}

probe_tcl() {
    if pkg-config --exists tcl 2>/dev/null; then
        pkg-config --cflags-only-I tcl
        return 0
    fi
    sdk=$(xcrun --show-sdk-path 2>/dev/null) || true
    if [ -n "${sdk:-}" ] && [ -f "$sdk/usr/include/tcl.h" ]; then
        echo "-I$sdk/usr/include"
        return 0
    fi
    return 1
}

probe_duktape() {
    if pkg-config --exists duktape 2>/dev/null; then
        pkg-config --cflags-only-I duktape
        return 0
    fi
    # Fallback: header in default include path (Debian's duktape-dev
    # installs duktape.h at /usr/include/duktape.h without a .pc file).
    if printf '#include <duktape.h>\nint main(){return 0;}\n' | \
        $CC -E -xc - >/dev/null 2>&1; then
        echo ""
        return 0
    fi
    return 1
}

probe_tinygo() {
    # The bridge .c is pure dlopen via std.dl (aether_dl_*) — it does NOT
    # need the Go toolchain at COMPILE time. libffi is the only
    # build-time dep, and even that's conditional on AETHER_HAS_LIBFFI
    # for the tinygo.call_dynamic escape hatch. The `go` / `tinygo`
    # binaries are only invoked at USER-build time (aeb's tinygo_lib
    # builder shells `go build -buildmode=c-shared` — or `tinygo build
    # -buildmode=c-shared -target=wasm` for wasm — on the .go source).
    #
    # Probe shape: if libffi-dev is present, opt in to the AETHER_HAS_LIBFFI
    # path and emit its cflags; otherwise the bridge still builds without
    # the escape hatch.
    if pkg-config --exists libffi 2>/dev/null; then
        echo "-DAETHER_HAS_LIBFFI $(pkg-config --cflags libffi 2>/dev/null)"
        return 0
    fi
    # No libffi: the bridge still compiles (the call_dynamic block is
    # behind #ifdef AETHER_HAS_LIBFFI).
    echo ""
    return 0
}

probe_tinyweb() {
    # ws_handshake.c needs only libc — implements SHA-1 + base64 inline
    # for the RFC 6455 Sec-WebSocket-Accept handshake. No system deps,
    # no pkg-config probes; the build always succeeds.
    echo ""
    return 0
}

probe_factor() {
    # The bridge .c loads libfactor purely via dlopen (mirrors host/lua,
    # host/python after their dlopen rewrites) — it needs NO Factor dev
    # lib at COMPILE time. The aether-lang-dev/factor-language fork's
    # libfactor + bootstrapped factor.image are only required at RUNTIME
    # (the host_factor integration test gates on AETHER_FACTOR_SONAME /
    # AETHER_FACTOR_IMAGE and skips when unset). So the archive always
    # builds; downstream just won't be able to *run* Factor code without
    # the fork present.
    echo ""
    return 0
}

probe_racket() {
    # Unlike host/factor (whose bridge needs NO headers — the fork's
    # embedding entry points are plain dlsym'd C-string functions), the
    # Racket CS value model is macro-based: chezscheme.h's Snil / Scons /
    # Smake_bytevector / Sbytevector_data are tagged-pointer macros, not
    # linkable functions, so the bridge's real (-DAETHER_HAS_RACKET) path
    # needs the embedding headers at COMPILE time. Those headers
    # (chezscheme.h, racketcsboot.h, api.h) live in a *built* Racket CS
    # tree, not in any apt package — so this probes for an explicit
    # include dir and SKIPs (no archive) when absent, rather than building
    # a bare stub that can never work.
    #
    # $AETHER_RACKET_INCLUDE = the built Racket's include/ dir, holding
    # chezscheme.h + racketcs.h + racketcsboot.h. The bridge STATIC-links
    # libracketcs.a ($AETHER_RACKET_LIB) into the importer and boots from the
    # images in $AETHER_RACKET_BOOT_DIR at runtime (the host_racket / _rhombus
    # integration tests gate on those and skip when unset). Here we only need
    # the headers to compile the bridge archive; SKIP (no archive) without them.
    local inc="${AETHER_RACKET_INCLUDE:-}"
    if [ -n "$inc" ] && [ -f "$inc/chezscheme.h" ] && [ -f "$inc/racketcs.h" ]; then
        echo "-I$inc"
        return 0
    fi
    return 1
}

probe_aether() {
    # The Aether-hosts-Aether bridge fork+execs a compiled child under
    # LD_PRELOAD=libaether_sandbox.so. It depends only on the in-tree
    # sandbox runtime (runtime/aether_sandbox.h) + libc — no third-party
    # dep, no pkg-config probe. Linux/macOS only (the body is guarded by
    # `#if defined(__linux__) || defined(__APPLE__)`); on other platforms
    # it compiles to an empty stub, so the build still succeeds.
    echo ""
    return 0
}

# Cross-mode module classification (only consulted when CROSS_MODE is set).
# The native probes shell pkg-config/brew/apt, which describe the HOST — wrong
# for a foreign target. So in cross mode we decide per module by tier instead:
#   tier1  — no external C dep (compiles like core): build it.
#   tier3:<hdr>:<lib> — needs a dep from CROSSBUILD_SYSROOT (Tier 2/3): build
#            iff that sysroot carries <hdr> + lib<lib>.a, else SKIP/FAIL.
#   nocross — host-language bridge that can't be cross-built here: SKIP/FAIL.
# Keyed by module name (build_module's $1). Unlisted → treated as nocross.
cross_class() {   # <module-name> -> echoes the class token
    case "$1" in
        tinyweb)               echo "tier1" ;;       # libc-only (SHA1+base64 inline)
        sqlite)                echo "tier3:sqlite3.h:sqlite3" ;;
        # Header-only dlopen bridges: they load the runtime lib via dlopen at
        # RUNTIME ("dlopen, not -l<x>") but #include the language headers at
        # COMPILE time. Contrib archives are .o-only (no link), so the sysroot
        # needs only the headers — the `tier3:<hdr>:` form (empty lib) checks
        # for the header alone. aether-crossbuild recipes/{lua,duktape}.sh stage
        # them. Cross-buildable; the target dlopen's its own liblua/libduktape.
        host_lua)              echo "tier3:lua.h:" ;;
        host_duktape)          echo "tier3:duktape.h:" ;;
        # host bridges whose headers are arch/config-GENERATED multiarch
        # dispatchers (pyconfig.h / ruby/config.h / perl's config.h) that can't
        # be portably cross-staged — ctr_notes.md Bug 6. Not cross-buildable here.
        host_python|host_perl|host_ruby|host_tcl|\
        host_tinygo|host_factor|host_racket|host_aether)
                               echo "nocross" ;;
        *)                     echo "nocross" ;;
    esac
}

# build_module <module_name> <relative_src_path> <AETHER_HAS_FLAG> <probe_fn>
# Returns: 0 OK, 1 SKIP (probe failed), 2 FAIL (compile/archive failed)
# Caller decides whether SKIP or FAIL is fatal (depends on MODULES mode).
build_module() {
    local name="$1"
    local src="$2"
    local flag="$3"
    local probe="$4"

    local incs
    if [ -n "$CROSS_MODE" ]; then
        # Target mode: classify by tier; the host probes don't apply.
        local class; class=$(cross_class "$name")
        case "$class" in
            tier1)
                incs=""
                ;;
            tier3:*)
                local hdr="${class#tier3:}"; local lib="${hdr#*:}"; hdr="${hdr%%:*}"
                if ! incs=$(cross_dep_present "$hdr" "$lib" 2>/dev/null); then
                    printf "  %-18s SKIP (dep '%s' not in CROSSBUILD_SYSROOT)\n" "$name" "$lib"
                    return 1
                fi
                ;;
            nocross|*)
                printf "  %-18s SKIP (host bridge — not cross-buildable)\n" "$name"
                return 1
                ;;
        esac
    else
        if ! incs=$($probe 2>/dev/null); then
            printf "  %-18s SKIP (dev library not found)\n" "$name"
            return 1
        fi
    fi

    # shellcheck disable=SC2086
    if ! $CC "${BASE_CFLAGS[@]}" -D"$flag" $incs \
        -c "$ROOT/$src" -o "$OUT/$name.o" 2>"$OUT/$name.err"; then
        printf "  %-18s FAIL (compile)\n" "$name"
        sed 's/^/      /' "$OUT/$name.err" | head -10
        return 2
    fi
    rm -f "$OUT/$name.err"

    # Archive. In cross mode use `zig ar` so the archive index is written by
    # the same toolchain that produced the foreign objects.
    local AR="ar"
    [ -n "$CROSS_MODE" ] && AR="zig ar"
    if ! $AR rcs "$OUT/libaether_$name.a" "$OUT/$name.o"; then
        printf "  %-18s FAIL (archive)\n" "$name"
        return 2
    fi

    # In cross mode, assert the archive is actually the TARGET arch so the
    # libaether_<name>.a name can't lie (same discipline as #1208's file check).
    # Extract a member with `zig ar` and file(1) it; skip the check only if file
    # is unavailable (never silently ship a mis-arch archive when it IS).
    if [ -n "$CROSS_MODE" ] && [ -n "$CROSS_ARCH_WANT" ] && command -v file >/dev/null 2>&1; then
        local ck="$OUT/.archck"; rm -rf "$ck"; mkdir -p "$ck"
        ( cd "$ck" && zig ar x "$OUT/libaether_$name.a" 2>/dev/null || true )
        local member; member=$(find "$ck" -name '*.o' 2>/dev/null | head -1)
        if [ -n "$member" ]; then
            local desc; desc=$(file -b "$member")
            if ! printf '%s' "$desc" | grep -q "$CROSS_ARCH_WANT"; then
                printf "  %-18s FAIL (wrong arch: '%s', want '%s')\n" "$name" "$desc" "$CROSS_ARCH_WANT"
                rm -rf "$ck"
                return 2
            fi
        fi
        rm -rf "$ck"
    fi

    printf "  %-18s OK   build/contrib/libaether_%s.a\n" "$name" "$name"
    echo -e "$name\tbuild/contrib/libaether_$name.a" >> "$OUT/MANIFEST.tmp"
    return 0
}

# Catalogue: maps a friendly --with name (or `make contrib MODULES=` entry)
# to the build_module argv for that module. Add new bridges here.
#
# Format (one per line): <short-name>|<build_module args...>
# `short-name` is what users pass in MODULES= / --with=. The build_module
# args are exactly what the build loop calls.
CATALOGUE=(
    "sqlite|sqlite       contrib/sqlite/aether_sqlite.c           AETHER_HAS_SQLITE  probe_sqlite"
    "avcodec|avcodec     contrib/avcodec/aether_avcodec.c         AETHER_HAS_AVCODEC probe_avcodec"
    "python|host_python  contrib/host/python/aether_host_python.c AETHER_HAS_PYTHON  probe_python"
    "lua|host_lua        contrib/host/lua/aether_host_lua.c       AETHER_HAS_LUA     probe_lua"
    "perl|host_perl      contrib/host/perl/aether_host_perl.c     AETHER_HAS_PERL    probe_perl"
    "ruby|host_ruby      contrib/host/ruby/aether_host_ruby.c     AETHER_HAS_RUBY    probe_ruby"
    "duktape|host_duktape  contrib/host/duktape/aether_host_duktape.c  AETHER_HAS_DUKTAPE  probe_duktape"
    "tcl|host_tcl        contrib/host/tcl/aether_host_tcl.c       AETHER_HAS_TCL     probe_tcl"
    "tinygo|host_tinygo  contrib/host/tinygo/aether_host_tinygo.c AETHER_HAS_TINYGO  probe_tinygo"
    "tinyweb|tinyweb     contrib/tinyweb/ws_handshake.c           AETHER_HAS_TINYWEB probe_tinyweb"
    "factor|host_factor  contrib/host/factor/aether_host_factor.c AETHER_HAS_FACTOR  probe_factor"
    "racket|host_racket  contrib/host/racket/aether_host_racket.c AETHER_HAS_RACKET  probe_racket"
    "aether|host_aether  contrib/host/aether/aether_host_aether.c AETHER_HAS_AETHER_HOST probe_aether"
)

# Find a catalogue line by short name. Echoes the build_module arg list
# on stdout, returns 1 if the name isn't in the catalogue.
catalogue_lookup() {
    local want="$1"
    local entry
    for entry in "${CATALOGUE[@]}"; do
        local short="${entry%%|*}"
        if [ "$short" = "$want" ]; then
            echo "${entry#*|}"
            return 0
        fi
    done
    return 1
}

# Decide what we're building. Two modes:
MODULES="${MODULES:-}"
if [ -n "$MODULES" ]; then
    MODE="explicit"
    # Comma -> space, dedupe, sort. The aether-build --with= path expects
    # build order to be deterministic so layer caching stays effective.
    REQUESTED=$(echo "$MODULES" | tr ',' '\n' | sort -u | xargs)
else
    MODE="default"
    # Default: build everything in the catalogue, skip silently if a dep
    # is missing. Same as the historical behaviour.
    REQUESTED=$(printf '%s\n' "${CATALOGUE[@]}" | cut -d'|' -f1 | xargs)
fi

echo "==================================="
echo "  Building contrib modules ($MODE mode)"
echo "==================================="
echo "  Requested: $REQUESTED"
echo ""

: > "$OUT/MANIFEST.tmp"

built=0
skipped=0
failed=0
hard_failures=""

for short in $REQUESTED; do
    args=$(catalogue_lookup "$short") || {
        echo "  $short — unknown module name (not in CATALOGUE)" >&2
        if [ "$MODE" = "explicit" ]; then
            hard_failures="$hard_failures $short(unknown)"
            failed=$((failed + 1))
        else
            skipped=$((skipped + 1))
        fi
        continue
    }
    # shellcheck disable=SC2086
    build_module $args
    rc=$?
    case "$rc" in
        0) built=$((built + 1)) ;;
        1)
            # SKIP — probe said the dep isn't here. In default mode this
            # is fine. In explicit mode the caller asked for this module,
            # so a missing dep is a hard failure.
            if [ "$MODE" = "explicit" ]; then
                hard_failures="$hard_failures $short(missing dep)"
                failed=$((failed + 1))
            else
                skipped=$((skipped + 1))
            fi
            ;;
        2)
            # FAIL — compile or archive failed. Always a hard failure
            # (no env-dependent toggle: the source itself is broken,
            # not the host's package state).
            hard_failures="$hard_failures $short(compile/archive)"
            failed=$((failed + 1))
            ;;
    esac
done

mv "$OUT/MANIFEST.tmp" "$OUT/MANIFEST"

echo ""
if [ "$MODE" = "explicit" ] && [ "$failed" -gt 0 ]; then
    echo "  $built built, $failed FAILED, $skipped skipped"
    echo ""
    echo "  Failures (explicit mode — these were requested but couldn't build):"
    echo "   $hard_failures"
    echo ""
    echo "  Manifest: $OUT/MANIFEST"
    exit 1
fi

echo "  $built built, $skipped skipped"
echo ""
echo "  Manifest: $OUT/MANIFEST"
echo "  Install:  make install-contrib"
