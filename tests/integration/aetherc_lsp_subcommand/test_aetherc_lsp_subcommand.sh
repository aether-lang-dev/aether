#!/bin/sh
# Regression: `aetherc lsp` boots the embedded language server, runs
# the LSP message loop, and shuts down cleanly on EOF. Issue #327 —
# embedding the LSP into aetherc replaces the standalone aether-lsp
# binary as the canonical entry point; the standalone is kept as a
# transitional alias and is exercised separately.
#
# Scope of this test is the *embedding glue*: subcommand dispatch,
# server bootstrap, and shutdown. The JSON-RPC parser inside
# `lsp_read_message` is shared with the standalone aether-lsp and
# has its own coverage; we don't re-test it here.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] aetherc_lsp_subcommand: $AETHERC not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Logging is opt-in (a server that wrote a log into every project it was
# started in was littering), so the test asks for one by path. EOF on stdin
# closes the read loop and the server exits cleanly.
cd "$tmpdir" || exit 1
AETHER_LSP_LOG="$tmpdir/aether-lsp.log" \
    "$AETHERC" lsp </dev/null >"$tmpdir/lsp.stdout" 2>"$tmpdir/lsp.stderr"
rc=$?

# Clean exit (EOF makes the loop's read return NULL, server breaks
# out and frees its state).
if [ $rc -ne 0 ]; then
    echo "  [FAIL] aetherc_lsp_subcommand: exited with code $rc"
    echo "  ---- stderr ----"
    cat "$tmpdir/lsp.stderr"
    exit 1
fi

# The server logs "starting" at boot and "shutting down" at exit. If
# both are present we know lsp_server_create + lsp_server_run +
# lsp_server_free were all reached — the subcommand wired the same
# code path the standalone binary uses.
if [ ! -f "$tmpdir/aether-lsp.log" ]; then
    echo "  [FAIL] aetherc_lsp_subcommand: AETHER_LSP_LOG produced no file; server never reached lsp_log"
    exit 1
fi

# Without the variable there must be NO log: an editor's working directory is
# not the server's to write into.
quiet_dir="$tmpdir/quiet"
mkdir -p "$quiet_dir"
( cd "$quiet_dir" && "$AETHERC" lsp </dev/null >/dev/null 2>&1 )
if [ -f "$quiet_dir/aether-lsp.log" ]; then
    echo "  [FAIL] aetherc_lsp_subcommand: wrote a log with AETHER_LSP_LOG unset"
    exit 1
fi
if ! grep -q "starting" "$tmpdir/aether-lsp.log"; then
    echo "  [FAIL] aetherc_lsp_subcommand: log missing 'starting' entry"
    cat "$tmpdir/aether-lsp.log"
    exit 1
fi
if ! grep -q "shutting down" "$tmpdir/aether-lsp.log"; then
    echo "  [FAIL] aetherc_lsp_subcommand: log missing 'shutting down' entry"
    cat "$tmpdir/aether-lsp.log"
    exit 1
fi

echo "  [PASS] aetherc_lsp_subcommand: subcommand boots embedded LSP and shuts down cleanly"
exit 0
