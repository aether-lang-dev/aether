#!/bin/sh
# Integration test for message tracing (#1333).
#
# Asserts the three properties the feature is only useful if it has:
#   1. a default build contains no tracing at all, and setting AETHER_TRACE
#      does nothing to it;
#   2. `ae build --trace` produces a binary that writes the trace, and the
#      events match the program's known message sequence IN ORDER;
#   3. the events carry message NAMES, not the bare integer ids the runtime
#      sees, which is what makes a trace readable at all.
#
# 1 is the one worth the most: the whole design rests on the shipped build
# carrying no tracing, so a test that only checked tracing works would pass
# just as happily if it were compiled into everything.
#
# The traced build is also the manifest_srcs_long_path regression: a source
# build (no libaether.a, which `ae build --trace` always is) must compile
# every source in MANIFEST no matter how long the tree's path is. The list
# lived in a fixed 8 KB buffer. 91 absolute paths need 6.0 KB under a 32-char
# prefix and 9.7 KB under a 73-char one, and on overflow the builder silently
# substituted a shorter hand-written list, so the link failed on whatever
# that list had drifted away from (std/bytes, most visibly) with no hint that
# the path length was the cause. That check lived in a test of its own that
# compiled the same probe the same way; one traced build now serves both, so
# the sweep compiles the whole runtime once here instead of twice (#2596).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
# On the MSYS2 Windows leg the binaries are ae.exe / aetherc.exe, and the
# copies below need the real names. (Asked as "is there an ae.exe": MSYS2
# answers yes for a plain `build/ae` too when only ae.exe exists.)
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ -f "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi

if [ ! -x "$AE" ]; then
    echo "  [SKIP] message_trace: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_HOME="$ROOT"

# (1) default build: tracing is not compiled in.
if ! "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/plain" >"$TMP/plain.log" 2>&1; then
    echo "  [FAIL] message_trace: default build failed"
    sed 's/^/        /' "$TMP/plain.log" | head -10
    exit 1
fi
AETHER_TRACE="$TMP/plain.jsonl" "$TMP/plain" >/dev/null 2>&1
if [ -f "$TMP/plain.jsonl" ]; then
    echo "  [FAIL] message_trace: a default build wrote a trace; tracing is compiled in"
    exit 1
fi

# The long toolchain root the traced build runs from. `ae` takes its root
# from where its own binary sits (build/ae beside build/aetherc, under a
# directory holding runtime/), ahead of AETHER_HOME, and Linux and macOS
# report that binary's path with links resolved: run through a link to the
# whole tree, or with AETHER_HOME naming one, it builds from the tree's real,
# short path. (The test that lived apart did exactly that, so it never built
# from its long root; on Windows it also copied the whole tree, .git and
# build/ included, because MSYS2's `ln -s` copies a directory.) So the root
# is a real directory holding copies of the two binaries and MANIFEST, and
# the trees the source build reads are linked in: symbolic links, or
# junctions on Windows, which need no privilege.
DEEP="$TMP/aaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbb/cccccccccccccccccccc/root"
link_dir() {
    case "$(uname -s 2>/dev/null)" in
        MINGW*|MSYS*|CYGWIN*)
            cmd //c mklink //J "$(cygpath -w "$2")" "$(cygpath -w "$1")" >/dev/null 2>&1 ;;
        *)
            ln -s "$1" "$2" 2>/dev/null ;;
    esac
}
long_root=0
if [ -f "$ROOT/build/MANIFEST" ] && mkdir -p "$DEEP/build" &&
   cp "$ROOT/build/ae$EXE" "$ROOT/build/aetherc$EXE" "$ROOT/build/MANIFEST" "$DEEP/build/" &&
   link_dir "$ROOT/runtime" "$DEEP/runtime" &&
   link_dir "$ROOT/std" "$DEEP/std" &&
   link_dir "$ROOT/include" "$DEEP/include" &&
   [ -d "$DEEP/runtime" ] && [ -d "$DEEP/std" ] && [ -d "$DEEP/include" ]; then
    long_root=1
    srcs=$(grep -vc '^#' "$ROOT/build/MANIFEST" 2>/dev/null || echo 0)
    need=$(awk -v base="${#DEEP}" '!/^#/ && NF { n += length($0) + base + 4 } END { print n+0 }' \
           "$ROOT/build/MANIFEST")
else
    echo "  [SKIP] manifest_srcs_long_path: cannot lay out a toolchain root under a long path here"
fi

# (2) traced build: the trace exists and the program still behaves.
if [ "$long_root" = 1 ]; then
    # --verbose prints the root the build resolved, checked below. A cache of
    # its own: the build cache keys on the binaries' contents, which the
    # copies share with the tree, so a warm cache could hand back a binary
    # built before and the long root would compile nothing.
    if ! AETHER_HOME="$DEEP" AETHER_CACHE_DIR="$TMP/cache" \
            "$DEEP/build/ae$EXE" build --verbose --trace "$SCRIPT_DIR/probe.ae" \
            -o "$TMP/traced" >"$TMP/traced.log" 2>&1; then
        echo "  [FAIL] manifest_srcs_long_path: the --trace source build failed from a ${#DEEP}-char root"
        echo "         the list needs $need bytes for $srcs sources"
        # Past the [cmd] lines --verbose adds, whose flags would match too.
        grep -v '^\[' "$TMP/traced.log" | grep -E 'error|Undefined|undefined reference' \
            | sed 's/^/        /' | head -8
        exit 1
    fi
    if ! grep -q '^\[toolchain\] root: .*cccccccccccccccccccc.root' "$TMP/traced.log"; then
        echo "  [FAIL] manifest_srcs_long_path: the build did not run from the long root"
        grep '^\[toolchain\]' "$TMP/traced.log" | sed 's/^/        /'
        exit 1
    fi
elif ! "$AE" build --trace "$SCRIPT_DIR/probe.ae" -o "$TMP/traced" >"$TMP/traced.log" 2>&1; then
    echo "  [FAIL] message_trace: --trace build failed"
    sed 's/^/        /' "$TMP/traced.log" | head -20
    exit 1
fi
out=$(AETHER_TRACE="$TMP/t.jsonl" "$TMP/traced" 2>&1)
expected_out="ping 1
pong 2
ping 3"
if [ "$out" != "$expected_out" ]; then
    echo "  [FAIL] message_trace: traced build changed program output"
    echo "$out" | sed 's/^/        /'
    exit 1
fi
if [ ! -s "$TMP/t.jsonl" ]; then
    echo "  [FAIL] message_trace: --trace build wrote no trace"
    exit 1
fi

# (3) the message sequence, in order, by NAME.
seq=$(grep -o '"msg_name":"[A-Za-z]*"' "$TMP/t.jsonl" | sed 's/.*:"//;s/"//' | tr '\n' ',')
if [ "$seq" != "Ping,Pong,Ping," ]; then
    echo "  [FAIL] message_trace: send sequence is '$seq', expected 'Ping,Pong,Ping,'"
    sed 's/^/        /' "$TMP/t.jsonl" | head -12
    exit 1
fi

# Delivery and processing both have to appear, or the trace is only recording
# intent and not what the runtime actually did.
for ev in step_begin step_end; do
    if ! grep -q "\"event\":\"$ev\"" "$TMP/t.jsonl"; then
        echo "  [FAIL] message_trace: no '$ev' events in the trace"
        exit 1
    fi
done

# The summary line reports completeness; a wrapped ring must say so.
if ! grep -q '"summary":true' "$TMP/t.jsonl"; then
    echo "  [FAIL] message_trace: trace has no summary line"
    exit 1
fi

# The hand-written source list is gone; a missing MANIFEST must be reported,
# never substituted for. Anchored on the sentinel that only that list
# contained.
if grep -q 'std/collections/aether_stringseq.c "' "$ROOT/tools/ae.c"; then
    echo "  [FAIL] manifest_srcs_long_path: a hand-written source list is back in tools/ae.c"
    echo "        MANIFEST is the single source of truth; it drifts if duplicated."
    exit 1
fi

echo "  [PASS] message_trace: absent by default, and records the real sequence under --trace"
if [ "$long_root" = 1 ]; then
    echo "  [PASS] manifest_srcs_long_path: $srcs sources ($need bytes) build from a ${#DEEP}-char root"
fi
