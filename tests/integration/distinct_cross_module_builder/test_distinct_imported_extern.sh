#!/bin/sh
# Imported C extern signatures live outside the merged program AST. Resolve
# distinct parameter AND return types there, including transitive imports.
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
export AETHER_HOME="$ROOT"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/lib/token" "$tmp/lib/bridge"
cat > "$tmp/lib/token/module.ae" <<'EOF'
exports(Token, new, token_echo)
type Token = distinct ptr
new() -> Token { return null as Token }
extern token_echo(value: Token) -> Token
EOF
cat > "$tmp/lib/bridge/module.ae" <<'EOF'
import token
exports(relay, bridge_echo)
extern bridge_echo(value: token.Token) -> token.Token
relay(value: token.Token) -> token.Token { return bridge_echo(value) }
EOF
cat > "$tmp/ok.ae" <<'EOF'
import token
import bridge
main() {
    value = token.new()
    token.echo(token.echo(value))
    bridge.relay(bridge.echo(value))
}
EOF
"$AETHERC" --check --lib "$tmp/lib" "$tmp/ok.ae"
# Even when token is reached only through bridge, its distinct definition
# must resolve in the imported extern signature.
cat > "$tmp/transitive.ae" <<'EOF'
import bridge
main() {
    value = null as Token
    bridge.relay(bridge.echo(value))
}
EOF
"$AETHERC" --check --lib "$tmp/lib" "$tmp/transitive.ae"
cat > "$tmp/reject.ae" <<'EOF'
import token
main() { token.echo(null) }
EOF
if "$AETHERC" --check --lib "$tmp/lib" "$tmp/reject.ae" > "$tmp/reject.log" 2>&1; then
    echo "  [FAIL] imported distinct extern accepted a raw pointer"
    exit 1
fi
if ! grep -q 'expected Token, got ptr' "$tmp/reject.log"; then
    cat "$tmp/reject.log"
    echo "  [FAIL] missing distinct parameter diagnostic"
    exit 1
fi
# The original CI failures: type checking needs no installed host libraries.
for lang in python tcl go; do
    "$AE" check "$ROOT/examples/host-$lang-demo.ae"
done
echo "  [PASS] imported distinct extern parameters and returns; raw pointers rejected; host demos checked"
