#!/bin/sh
# #3532: the cache key remembers each file's content hash beside its size,
# modification time and identity, and reads a file again only when one of
# those changed. Nothing it remembers may serve a stale build:
#
# 1. a same-size edit of a module the key had remembered rebuilds;
# 2. so does a same-size edit whose old modification time is then put back
#    (`touch -r`), which the identity (change time) catches;
# 3. an untouched module is still a cache hit, and the table is written
#    beside the cache entries without counting as one.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] cache_file_hash_memo: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
export AETHER_HOME="$ROOT"
fail=0

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
cat > main.ae <<'AEOF'
import helper

main() {
    println(helper.greet())
}
AEOF

write_helper() {
    cat > helper.ae <<AEOF
greet() -> string {
    return "$1"
}
AEOF
}

run_it() {
    "$AE" run main.ae 2>"$TMP/err.txt"
}

write_helper one
# Settled: the key only remembers files modified two seconds before it reads
# them, so wait that out before the run that records helper.ae.
touch -d '1 minute ago' helper.ae main.ae 2>/dev/null || sleep 3
got="$(run_it)"
[ "$got" = "one" ] || { echo "  [FAIL] cache_file_hash_memo: first run printed '$got'"; cat "$TMP/err.txt"; exit 1; }
got="$(run_it)"
[ "$got" = "one" ] || { echo "  [FAIL] cache_file_hash_memo: the cache hit printed '$got'"; fail=1; }
if [ ! -s "$TMP/cache/filehash.v1" ]; then
    echo "  [FAIL] cache_file_hash_memo: no remembered hashes in the cache directory"
    fail=1
fi

# 1. same size, new bytes.
write_helper two
got="$(run_it)"
if [ "$got" != "two" ]; then
    echo "  [FAIL] cache_file_hash_memo: a same-size edit served '$got', want 'two'"
    fail=1
fi

# 2. same size, new bytes, and the recorded modification time put back
#    exactly (`touch -r`): only the change time tells them apart.
touch -d '1 minute ago' helper.ae 2>/dev/null || sleep 3
touch -r helper.ae "$TMP/ref"
got="$(run_it)"
[ "$got" = "two" ] || { echo "  [FAIL] cache_file_hash_memo: settled run printed '$got'"; fail=1; }
write_helper six
touch -r "$TMP/ref" helper.ae
got="$(run_it)"
if [ "$got" != "six" ]; then
    echo "  [FAIL] cache_file_hash_memo: an edit with its old mtime put back served '$got', want 'six'"
    fail=1
fi

[ "$fail" = 0 ] && echo "  [PASS] cache_file_hash_memo"
exit "$fail"
