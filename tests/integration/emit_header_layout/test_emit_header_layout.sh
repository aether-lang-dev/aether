#!/bin/sh
# A C host built against the --emit-header file reads the fields a message
# was sent with (#2517).
#
# The header declared a message's fields in declaration order while the
# generated .c packs ints first, then pointers, then the rest, so a host
# compiled against the header wrote every field but the first of an
# interleaved message at the wrong offset. The header was also empty (#996
# gated its contents on the --emit=csrc catalog header), and the typed send
# helper sent a multi-field message with no payload. probe.ae's actor prints
# what it reads from the messages shim.c sends through the header.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] emit_header_layout: $AE not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AETHERC" --emit-header "$TMPDIR/probe.h" \
        "$SCRIPT_DIR/probe.ae" "$TMPDIR/probe.c" >"$TMPDIR/emit.log" 2>&1; then
    echo "  [FAIL] emit_header_layout: aetherc --emit-header failed"
    sed 's/^/    /' "$TMPDIR/emit.log" | head -15
    exit 1
fi

if ! grep -q "} Mixed;" "$TMPDIR/probe.h"; then
    echo "  [FAIL] emit_header_layout: the header has no Mixed struct"
    sed 's/^/    /' "$TMPDIR/probe.h" | head -30
    exit 1
fi

# The host is compiled as docs/c-embedding.md says a host is: against the
# header, with the include flags `ae cflags --cflags` prints plus the tree
# root the header's own includes are relative to (as public_headers does).
# The shim includes "probe.h" from its own directory.
cp "$SCRIPT_DIR/shim.c" "$TMPDIR/shim.c"
CFLAGS_ALL=$("$AE" cflags --cflags 2>/dev/null)
if ! "${CC:-cc}" -I"$ROOT" $CFLAGS_ALL -c "$TMPDIR/shim.c" -o "$TMPDIR/shim.o" >"$TMPDIR/host.log" 2>&1; then
    echo "  [FAIL] emit_header_layout: the host does not compile against the header"
    sed 's/^/    /' "$TMPDIR/host.log" | head -15
    exit 1
fi
if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$TMPDIR/shim.o" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] emit_header_layout: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] emit_header_layout: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

for want in "mixed a=7 s=seven b=8 f=2.5 c=9" \
            "mixed a=70 s=seventy b=80 f=0.25 c=90" \
            "bump by=5"; do
    if ! grep -q "^$want" "$TMPDIR/run.log"; then
        echo "  [FAIL] emit_header_layout: expected \"$want\""
        sed 's/^/    /' "$TMPDIR/run.log" | head -30
        exit 1
    fi
done

echo "  [PASS] emit_header_layout"
