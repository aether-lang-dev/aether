#!/usr/bin/env bash
# contrib_build.sh — build static libs for contrib modules.
#
# Two modes:
#
# 1. Default (MODULES unset) — best-effort: probe every contrib module
#    (sqlite, avcodec and the host bridges left here, stunt and aether). For each available dep, compile + archive.
#    Skip + tally any whose dev libs aren't installed. Same shape as
#    historical behaviour; convenient for dev boxes.
#
# 2. MODULES=<list> (comma-separated) — explicit, fail-hard: build ONLY
#    the named modules; any requested module that can't compile is a
#    hard failure (exit 1), not a silent skip: a caller that asked for a
#    module expects it to build. Silent SKIP there was the v0.209.0
#    footgun (ctr_notes.md Bug 6).
#
# Examples:
#   MODULES=sqlite                    # build only sqlite; fail if it can't
#   MODULES=sqlite,stunt              # build sqlite AND host_stunt
#   (unset)                           # build-all, tolerate skips
#
# The language bridges (Lua, Python, Tcl, Perl, Ruby, Duktape, QuickJS, Go,
# TinyGo, Java, Factor, Racket, Rhombus) moved to aether-guest-languages and
# are built there.
#
# Writes build/contrib/MANIFEST listing one line per built archive:
#     <module>\t<archive_path>[\t<installed name>]
# The optional third column is the name to install it under, when that is not
# libaether_<module>.a (the vendored SQLite, which stays libsqlite3.a).
# `make install-contrib` reads this manifest to know what to ship.
#
# Called from `make contrib` (default mode) or
# `make contrib MODULES=<list>` (explicit mode).

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# CONTRIB_OUT: another directory to build into, for a test that must not
# touch the build/contrib the rest of the sweep links against.
OUT="${CONTRIB_OUT:-$ROOT/build/contrib}"
mkdir -p "$OUT"

# Each run builds in a directory of its own and publishes what it built with a
# rename, which replaces a file in one step (#2780). Tests in the sweep run
# this script while other tests link against build/contrib, and it used to
# write there in place: an archive could be read half rewritten, two runs
# overwrote each other's objects and manifest, and a link that fell between
# the sqlite veneer and the vendored libsqlite3.a it calls into got macOS's
# system library, which has no sqlite3_enable_load_extension (the 0.807.0
# and 0.808.0 macOS release builds failed on it).
STAGE="$(mktemp -d "$OUT/.stage.XXXXXX")" || exit 1
trap 'rm -rf "$STAGE"' EXIT
# publish NAME: puts $STAGE/NAME in place as $OUT/NAME, in one rename.
publish() { mv -f "$STAGE/$1" "$OUT/$1"; }

CC="${CC:-cc}"
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
CROSS_MACH_WANT=""     # and, for an ELF target, its machine's
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
            # A relocatable object carries no OS mark (zig leaves EI_OSABI at
            # SYSV, and the FreeBSD note is the linker's), so `file` names no
            # FreeBSD in a .o: what can be checked is ELF and the machine.
            CROSS_ARCH_WANT="ELF"
            ;;
        *linux*)   CROSS_ARCH_WANT="ELF" ;;
        *macos*)   CROSS_ARCH_WANT="Mach-O" ;;
        # Windows (Tier A, self-contained like linux): zig bundles the full
        # MinGW-w64 target — no sysroot. A contrib .o member is a PE/COFF object.
        *windows*) CROSS_ARCH_WANT="COFF" ;;
    esac
    if [ "$CROSS_ARCH_WANT" = ELF ]; then
        case "$CONTRIB_TARGET" in
            x86_64-*)  CROSS_MACH_WANT="x86-64" ;;
            aarch64-*) CROSS_MACH_WANT="aarch64" ;;
        esac
    fi

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

# --- SQLite: the pinned amalgamation, or the system library (#1372) ---------
# contrib.sqlite prefers the amalgamation pinned in contrib/sqlite/
# amalgamation.lock, compiled into build/contrib/libsqlite3.a beside the
# veneer. `-laether_sqlite -lsqlite3` (the module's @link) then resolves
# -lsqlite3 to that archive, because the -L for build/contrib is searched
# before the system directories -- so the built program carries SQLite and
# needs no libsqlite3 on the machine that runs it, and in cross mode no
# libsqlite3 staged for the target either.
#
# The amalgamation is fetched on demand (checksum-verified, never committed).
# When it cannot be had -- offline, no curl -- the veneer builds against the
# system sqlite3 as it always did. SQLITE_SYSTEM=1 forces that path.
SQLITE_AMAL_DIR="$ROOT/contrib/sqlite/amalgamation"
SQLITE_VENDORED=""
SQLITE_CFLAGS=""
sqlite_requested() {
    [ -z "${MODULES:-}" ] && return 0
    case ",$MODULES," in *,sqlite,*) return 0 ;; esac
    return 1
}
if [ "${SQLITE_SYSTEM:-}" != "1" ] && sqlite_requested; then
    if sh "$ROOT/scripts/fetch-sqlite-amalgamation.sh" "$SQLITE_AMAL_DIR" >/dev/null; then
        SQLITE_VENDORED=1
        # shellcheck disable=SC1090
        SQLITE_CFLAGS="$(. "$ROOT/contrib/sqlite/amalgamation.lock"; printf '%s' "$SQLITE_CFLAGS")"
    else
        echo "  sqlite: amalgamation unavailable -- falling back to the system library" >&2
    fi
fi

# Compile sqlite3.c and archive it as build/contrib/libsqlite3.a. The object
# is the expensive part (a ~260k-line TU, most of a minute at -O2), so it is
# cached in $SQLITE_OBJ_CACHE_DIR under a name derived from everything that
# changes it: the amalgamation's content, the compiler's identity, every flag,
# and the target. A different input is a different file, so a hit is always
# the right object and a miss always recompiles. CI persists the directory
# with actions/cache the way it does the miniaudio object.
SQLITE_OBJ_CACHE_DIR="${SQLITE_OBJ_CACHE_DIR:-$ROOT/.ci-cache/sqlite}"
sha256_text() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d' ' -f1
    else shasum -a 256 | cut -d' ' -f1; fi
}
build_vendored_sqlite() {
    local AR="$1"
    local flags="${BASE_CFLAGS[*]} -w $SQLITE_CFLAGS -I$SQLITE_AMAL_DIR"
    local key
    key=$( { cat "$SQLITE_AMAL_DIR/sqlite3.c" "$SQLITE_AMAL_DIR/sqlite3.h" | sha256_text
             $CC --version 2>/dev/null | head -1
             printf '%s\n%s\n' "$flags" "$CONTRIB_TARGET"; } | sha256_text )
    local cached="$SQLITE_OBJ_CACHE_DIR/sqlite3-${key:0:16}.o"
    local obj="$STAGE/sqlite3.o"
    if [ -f "$cached" ]; then
        cp "$cached" "$obj"
        printf "  %-18s cached object %s\n" "sqlite3" "${cached#"$ROOT"/}"
    else
        printf "  %-18s compiling the amalgamation (once per input set)\n" "sqlite3"
        # shellcheck disable=SC2086
        if ! $CC "${BASE_CFLAGS[@]}" -w $SQLITE_CFLAGS -I"$SQLITE_AMAL_DIR" \
            -c "$SQLITE_AMAL_DIR/sqlite3.c" -o "$obj" 2>"$STAGE/sqlite3.err"; then
            printf "  %-18s FAIL (compile)\n" "sqlite3"
            sed 's/^/      /' "$STAGE/sqlite3.err" | head -10
            return 2
        fi
        mkdir -p "$SQLITE_OBJ_CACHE_DIR"
        cp "$obj" "$cached.tmp.$$" && mv "$cached.tmp.$$" "$cached"
    fi
    if ! $AR rcs "$STAGE/libsqlite3.a" "$obj"; then
        printf "  %-18s FAIL (archive)\n" "sqlite3"
        return 2
    fi
    publish libsqlite3.a
    printf "  %-18s OK   build/contrib/libsqlite3.a (SQLite %s, vendored)\n" "sqlite3" \
        "$(sed -n 's/^SQLITE_VERSION=//p' "$ROOT/contrib/sqlite/amalgamation.lock")"
    # Third column: the name to install it under. Every other archive installs
    # as libaether_<name>.a; this one must stay libsqlite3.a for -lsqlite3.
    echo -e "sqlite3\tbuild/contrib/libsqlite3.a\tlibsqlite3.a" >> "$STAGE/MANIFEST"
    return 0
}

# probe_<module> echoes dev-include flags on stdout when the dep is
# available. Returns 0 if available, 1 if not.

probe_sqlite() {
    # The vendored amalgamation's header, so the veneer compiles against the
    # same SQLite it links -- ahead of any system sqlite3.h.
    # AETHER_SQLITE_VENDORED tells the veneer the full API is there (macOS's
    # system library lacks sqlite3_enable_load_extension).
    if [ -n "$SQLITE_VENDORED" ]; then
        echo "-I$SQLITE_AMAL_DIR -DAETHER_SQLITE_VENDORED=1"
        return 0
    fi
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

probe_stunt() {
    # stunt, the test double (contrib/host/stunt): the bridge dlopens
    # libstunt at runtime and needs only the in-tree runtime headers + libc.
    echo ""
    return 0
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
        # The vendored amalgamation needs nothing from the target; without
        # it, a libsqlite3 staged in CROSSBUILD_SYSROOT does.
        sqlite)                if [ -n "$SQLITE_VENDORED" ]; then echo "vendored"
                               else echo "tier3:sqlite3.h:sqlite3"; fi ;;
        # stunt's bridge has no dependency at all: it compiles like core.
        host_stunt)            echo "tier1" ;;
        # The Aether host's fork+exec body is Linux/macOS only; not
        # cross-built here.
        host_aether)           echo "nocross" ;;
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
            vendored)
                incs="-I$SQLITE_AMAL_DIR"
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
        -c "$ROOT/$src" -o "$STAGE/$name.o" 2>"$STAGE/$name.err"; then
        printf "  %-18s FAIL (compile)\n" "$name"
        sed 's/^/      /' "$STAGE/$name.err" | head -10
        return 2
    fi

    # Archive. In cross mode use `zig ar` so the archive index is written by
    # the same toolchain that produced the foreign objects.
    local AR="ar"
    [ -n "$CROSS_MODE" ] && AR="zig ar"
    if ! $AR rcs "$STAGE/libaether_$name.a" "$STAGE/$name.o"; then
        printf "  %-18s FAIL (archive)\n" "$name"
        return 2
    fi

    # In cross mode, assert the archive is actually the TARGET arch so the
    # libaether_<name>.a name can't lie (same discipline as #1208's file check).
    # Extract a member with `zig ar` and file(1) it; skip the check only if file
    # is unavailable (never silently ship a mis-arch archive when it IS).
    if [ -n "$CROSS_MODE" ] && [ -n "$CROSS_ARCH_WANT" ] && command -v file >/dev/null 2>&1; then
        local ck="$STAGE/.archck"; rm -rf "$ck"; mkdir -p "$ck"
        ( cd "$ck" && zig ar x "$STAGE/libaether_$name.a" 2>/dev/null || true )
        local member; member=$(find "$ck" -name '*.o' 2>/dev/null | head -1)
        if [ -n "$member" ]; then
            local desc; desc=$(file -b "$member")
            if ! printf '%s' "$desc" | grep -q "$CROSS_ARCH_WANT" \
               || ! printf '%s' "$desc" | grep -q "$CROSS_MACH_WANT"; then
                printf "  %-18s FAIL (wrong arch: '%s', want '%s %s')\n" "$name" "$desc" "$CROSS_ARCH_WANT" "$CROSS_MACH_WANT"
                rm -rf "$ck"
                return 2
            fi
        fi
        rm -rf "$ck"
    fi

    # A veneer compiled against the vendored SQLite calls into it, so the
    # vendored library is in place before the veneer is (#2780).
    if [ "$name" = "sqlite" ] && [ -n "$SQLITE_VENDORED" ]; then
        build_vendored_sqlite "$AR" || return 2
    fi
    publish "libaether_$name.a"
    printf "  %-18s OK   build/contrib/libaether_%s.a\n" "$name" "$name"
    echo -e "$name\tbuild/contrib/libaether_$name.a" >> "$STAGE/MANIFEST"
    # A libsqlite3.a left by an earlier vendored build would shadow the system
    # library this veneer was compiled against; it goes once the veneer that
    # does not need it is in place.
    if [ "$name" = "sqlite" ] && [ -z "$SQLITE_VENDORED" ]; then
        rm -f "$OUT/libsqlite3.a"
    fi
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
    "aether|host_aether  contrib/host/aether/aether_host_aether.c AETHER_HAS_AETHER_HOST probe_aether"
    "stunt|host_stunt    contrib/host/stunt/aether_host_stunt.c   AETHER_HAS_STUNT   probe_stunt"
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
    # Comma -> space, dedupe, sort, so the build order is deterministic.
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

: > "$STAGE/MANIFEST"

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

publish MANIFEST

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
