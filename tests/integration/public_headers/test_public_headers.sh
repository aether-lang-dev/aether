#!/bin/sh
# Every installed public header must compile on its own.
#
# `std/**/*.h` and `include/*.h` ship in the install tree, so a consumer can
# include any one of them directly. A header that only compiles because some
# other header happened to be included first is broken for that consumer, and
# nothing checked it: the MSVC job compiles a handful of runtime headers under
# cl.exe as a portability probe, and stops there.
#
# This came out of removing `std/aether_std.h`, an unused umbrella header that
# covered 7 of 44 std headers and could not be completed, because two of them
# (#1433) define conflicting `HttpRequest` types. Self-containment is the part
# that IS true today, so it gets pinned here before it stops being true.
#
# When #1433 is fixed, the pair check belongs in this file too: compiling the
# HTTP client and server headers in one translation unit is the case that
# regression would break.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] public_headers: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$ROOT" || exit 1

CFLAGS_ALL=$("$AE" cflags --cflags 2>/dev/null)
if [ -z "$CFLAGS_ALL" ]; then
    echo "  [FAIL] public_headers: 'ae cflags --cflags' produced nothing"
    exit 1
fi

fail=0
checked=0
for h in $(cd std && find . -name '*.h' | sed 's|^\./||' | sort) ; do
    checked=$((checked + 1))
    printf '#include "std/%s"\nint main(void){return 0;}\n' "$h" > "$TMP/probe.c"
    if ! cc -I. $CFLAGS_ALL -c "$TMP/probe.c" -o /dev/null 2>"$TMP/err.txt"; then
        echo "  [FAIL] public_headers: std/$h does not compile on its own"
        sed 's/^/        /' "$TMP/err.txt" | head -6
        fail=$((fail + 1))
    fi
done

# The public embedder header, which ships from include/ rather than std/.
for h in include/*.h; do
    [ -f "$h" ] || continue
    checked=$((checked + 1))
    printf '#include "%s"\nint main(void){return 0;}\n' "$h" > "$TMP/probe.c"
    if ! cc -I. $CFLAGS_ALL -c "$TMP/probe.c" -o /dev/null 2>"$TMP/err.txt"; then
        echo "  [FAIL] public_headers: $h does not compile on its own"
        sed 's/^/        /' "$TMP/err.txt" | head -6
        fail=$((fail + 1))
    fi
done

# The pair that could not coexist (#1433): the HTTP client and server headers
# each published a type named HttpRequest, and they were different structs, so
# no translation unit could include both. The proxy worked around it by
# hand-declaring the client prototypes, which is worse than the collision: the
# compiler stops checking them against the real definitions. Both orders,
# because an include-order-dependent fix is not a fix.
for pair in "std/net/aether_http.h std/net/aether_http_server.h" \
            "std/net/aether_http_server.h std/net/aether_http.h"; do
    set -- $pair
    checked=$((checked + 1))
    printf '#include "%s"\n#include "%s"\nint main(void){return 0;}\n' "$1" "$2" > "$TMP/pair.c"
    if ! cc -I. $CFLAGS_ALL -c "$TMP/pair.c" -o /dev/null 2>"$TMP/err.txt"; then
        echo "  [FAIL] public_headers: cannot include $1 and $2 together"
        sed 's/^/        /' "$TMP/err.txt" | head -6
        fail=$((fail + 1))
    fi
done

if [ "$fail" -gt 0 ]; then
    echo "  public_headers: $fail of $checked headers are not self-contained"
    exit 1
fi
echo "  [PASS] public_headers: all $checked public headers compile standalone"
