#!/bin/sh
# #2954: execution accounts, budgets a host owns for executions that outlive
# one call. host.c prints one line per acceptance item of the issue and per
# test of the design (each line's meaning is in host.c); app.ae is an Aether
# host. Both run against the library in every form it ships in: linked into
# the host (one runtime), as a library with its own runtime (two runtime
# copies, whose accounts are one per process through the rendezvous), and
# on the shared runtime. two_copies.c is a C host with a runtime of its own
# loading the library with another: one lock and one thread per account
# across the copies, and an account made before the copies found each other
# refused by the copy that does not hold it. foreign_first.c, a host with no
# runtime of its own, loads foreign_rv.c (a rendezvous of another layout)
# before two copies, which both refuse accounts (#3437). unload.c
# unloads a copy that shared the accounts, then makes the first account,
# and many.c has 70 copies share them, past the 64 the list once held
# (#3411).
#
# Fixtures, not tests: accts.ae is a library and app.ae its importer; the
# driver builds both.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/syslibs.sh"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
export AETHER_HOME="$ROOT"
export AETHER_STACK_TRACE=0

[ -x "$AE" ] || { echo "  [SKIP] emit_lib_accounts: $AE not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
fail=0
want='isolated: A 0 outcome 4 tripped 2; B 48000 outcome 0; host 160 outcome 0
kept: hand_over outcome 0, A holds it (yes), unchanged while B ran (yes); B 1600 outcome 0
credit: B'"'"'s list freed under A (50 items) leaves B 0; A'"'"'s string freed by the host leaves A 0
narrow: the child'"'"'s 16 KiB counted against its parent (yes); past the parent'"'"'s limit 0 outcome 4; freed, parent 0 child 0
std: JSON past the allowance 0 outcome 4; list past it 0 outcome 4; outside it 3
inject: 0 outcome 5
deadline: D 0 outcome 2 tripped 1; B 249000 outcome 0 tripped 0
active: after 300 ms away, ACTIVE 249000 outcome 0, ELAPSED 0 outcome 2; ACTIVE spent 0 outcome 2
nested: outer 2 outcome 0, inner outcome 0; in use after: A
thread: another thread -1; this thread later 0
baseline: yes'
app_want='charged: true
refused: the call went past the memory cap (outcome 4, tripped 2)
freed outside the account: 20 items, the account holds 0'

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ext=dll; exe=.exe; dl="" ;;
    Darwin) ext=dylib; exe=""; dl="" ;;
    Linux) ext=so; exe=""; dl="-ldl" ;;
    *) ext=so; exe=""; dl="" ;;
esac

# $1 = label, $2 = output
check() {
    if [ "$2" != "$want" ]; then
        echo "  [FAIL] emit_lib_accounts: $1, C host:"
        echo "      got:"; printf '%s\n' "$2" | sed 's/^/        /'
        echo "      want:"; printf '%s\n' "$want" | sed 's/^/        /'
        fail=1
    fi
}

# 1. one runtime
if ! "$AETHERC" --emit=lib --with=extern "$SCRIPT_DIR/accts.ae" "$TMP/accts.c" > "$TMP/accts.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: aetherc --emit=lib failed"
    tail -5 "$TMP/accts.log" | sed 's/^/        /'
    exit 1
fi
# shellcheck disable=SC2046
if ! "$CC" -O1 -DHOST_STATIC -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
        "$TMP/accts.c" "$SCRIPT_DIR/hooks.c" "$SCRIPT_DIR/host.c" -o "$TMP/host_static" \
        $("$AE" cflags --libs) -lpthread > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: the one-runtime host did not build"
    head -10 "$TMP/cc.log" | sed 's/^/        /'
    exit 1
fi
check "one runtime" "$("$TMP/host_static" 2>"$TMP/static.err" | tr -d '\r')"

if ! "$CC" -O1 "$SCRIPT_DIR/host.c" -o "$TMP/host_dyn" $dl -lpthread > "$TMP/cc_dyn.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: the loading host did not build"
    head -10 "$TMP/cc_dyn.log" | sed 's/^/        /'
    exit 1
fi

# $1 = label, $2 = extra build flags
build_lib() {
    mkdir -p "$TMP/$1"
    # shellcheck disable=SC2086
    (cd "$TMP/$1" && "$AE" build --emit=lib $2 --with=extern "$SCRIPT_DIR/accts.ae" \
        --extra "$SCRIPT_DIR/hooks.c" -o "$TMP/$1/libaccts.$ext" > "$TMP/$1/build.log" 2>&1)
}

# $1 = label, $2 = library dir: the Aether program importing it in binary form
run_app() {
    cp "$SCRIPT_DIR/app.ae" "$2/app.ae"
    got="$(cd "$2" && "$AE" build app.ae -o app > app_build.log 2>&1 && \
           PATH="$2:$ROOT/build/shared:$PATH" \
           LD_LIBRARY_PATH="$2:$ROOT/build/shared${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
           DYLD_LIBRARY_PATH="$2:$ROOT/build/shared${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
           ./app$exe 2>"$2/app.err" | tr -d '\r')"
    if [ "$got" != "$app_want" ]; then
        echo "  [FAIL] emit_lib_accounts: $1, Aether host:"
        echo "      got:"; printf '%s\n' "$got" | sed 's/^/        /'
        echo "      want:"; printf '%s\n' "$app_want" | sed 's/^/        /'
        tail -5 "$2/app_build.log" "$2/app.err" 2>/dev/null | sed 's/^/        /'
        fail=1
    fi
}

# 2. the library's own runtime: two copies in the Aether host
if ! build_lib own ""; then
    echo "  [FAIL] emit_lib_accounts: ae build --emit=lib failed"
    tail -5 "$TMP/own/build.log" | sed 's/^/        /'
    exit 1
fi
check "its own runtime" "$("$TMP/host_dyn" "$TMP/own/libaccts.$ext" 2>"$TMP/dyn.err" | tr -d '\r')"
run_app "its own runtime" "$TMP/own"

# $1 = label, $2 = what it printed, $3 = what it should
expect() {
    if [ "$2" != "$3" ]; then
        echo "  [FAIL] emit_lib_accounts: $1:"
        echo "      got:"; printf '%s\n' "$2" | sed 's/^/        /'
        echo "      want:"; printf '%s\n' "$3" | sed 's/^/        /'
        fail=1
    fi
}

# Two copies in a C host: the program's (linked in) and the library's.
# shellcheck disable=SC2046
if ! "$CC" -O1 -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
        "$SCRIPT_DIR/two_copies.c" -o "$TMP/two_copies" \
        $("$AE" cflags --libs) -lpthread $dl > "$TMP/cc_two.log" 2>&1 ||
   ! "$CC" -shared -fPIC -I"$ROOT/runtime" "$SCRIPT_DIR/foreign_rv.c" -o "$TMP/foreign_rv.$ext" > "$TMP/cc_rv.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: the two-copies host did not build"
    head -10 "$TMP/cc_two.log" "$TMP/cc_rv.log" | sed 's/^/        /'
    exit 1
fi
expect "two copies" "$("$TMP/two_copies" "$TMP/own/libaccts.$ext" 2>"$TMP/two.err" | tr -d '\r')" \
"shared: this copy's 1000 bytes charged 1000; the library's past the allowance 0 outcome 4, tripped 2
thread: in use here, refused on another through this copy -1, through the library's -1; holds 0"
expect "two copies, an account made before the library loaded" \
    "$("$TMP/two_copies" "$TMP/own/libaccts.$ext" early 2>"$TMP/early.err" | tr -d '\r')" \
    "early: the library's copy shares the account: yes"
# The program's copy, which exports nothing on Windows and on Linux here (no
# -rdynamic), found by the library's by its record (#3445).
expect "two copies, an account put in use before the library loaded" \
    "$("$TMP/two_copies" "$TMP/own/libaccts.$ext" in_use 2>"$TMP/in_use.err" | tr -d '\r')" \
    "in use: the library's allocations are charged to the account in use: yes"
# Accounts of another layout loaded first, then two copies, in a host with no
# runtime of its own (#3437): where executables export their symbols, a
# host's own accounts are the first a copy finds.
cp "$TMP/own/libaccts.$ext" "$TMP/libaccts_c.$ext"
# shellcheck disable=SC2086
if ! "$CC" -O1 "$SCRIPT_DIR/foreign_first.c" -o "$TMP/foreign_first" $dl > "$TMP/cc_ff.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: the foreign-first host did not build"
    head -10 "$TMP/cc_ff.log" | sed 's/^/        /'
    exit 1
fi
expect "two copies, accounts of another layout loaded first" \
    "$("$TMP/foreign_first" "$TMP/foreign_rv.$ext" "$TMP/own/libaccts.$ext" "$TMP/libaccts_c.$ext" 2>"$TMP/foreign.err" | tr -d '\r')" \
    "foreign: the first copy refused, the second refused; a call still works: 160 outcome 0"
expect "two copies, each copy names the layout once" \
    "$(grep -c "cannot share execution accounts" "$TMP/foreign.err")" "2"

# #3411: a copy that shared the accounts and is unloaded is not written
# into when the first account is made afterwards.
cp "$TMP/own/libaccts.$ext" "$TMP/libaccts_b.$ext"
# shellcheck disable=SC2086
if ! "$CC" -O1 "$SCRIPT_DIR/unload.c" -o "$TMP/unload" $dl > "$TMP/cc_unload.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: the unload host did not build"
    head -10 "$TMP/cc_unload.log" | sed 's/^/        /'
    exit 1
fi
expect "a copy unloaded after it shared the accounts" \
    "$("$TMP/unload" "$TMP/own/libaccts.$ext" "$TMP/libaccts_b.$ext" 2>"$TMP/unload.err" | tr -d '\r')" \
    "unload: B's call 160, unloaded 0; an account made and freed after: yes"

# #3411: more copies than the list once held (64), the last one charged.
mkdir -p "$TMP/many"
copies=""
i=1
while [ $i -lt 70 ]; do
    cp "$TMP/own/libaccts.$ext" "$TMP/many/libaccts_$i.$ext"
    copies="$copies $TMP/many/libaccts_$i.$ext"
    i=$((i + 1))
done
# shellcheck disable=SC2086
if ! "$CC" -O1 "$SCRIPT_DIR/many.c" -o "$TMP/many_host" $dl > "$TMP/cc_many.log" 2>&1; then
    echo "  [FAIL] emit_lib_accounts: the many-copies host did not build"
    head -10 "$TMP/cc_many.log" | sed 's/^/        /'
    exit 1
fi
# shellcheck disable=SC2086
expect "70 copies sharing the accounts" \
    "$("$TMP/many_host" "$TMP/own/libaccts.$ext" $copies 2>"$TMP/many.err" | tr -d '\r')" \
    "many: 70 copies joined; the last one's string charged: yes, freed: yes"
rm -rf "$TMP/many"

# 3. one shared runtime
if ls "$ROOT/build/shared/" 2>/dev/null | grep -q "aether"; then
    if ! build_lib shared "--shared-runtime"; then
        echo "  [FAIL] emit_lib_accounts: ae build --emit=lib --shared-runtime failed"
        tail -5 "$TMP/shared/build.log" | sed 's/^/        /'
        exit 1
    fi
    rt="$(ls "$ROOT/build/shared/"aether.dll "$ROOT/build/shared/"libaether.dylib \
          "$ROOT/build/shared/"libaether.so 2>/dev/null | head -1)"
    check "--shared-runtime" "$(PATH="$ROOT/build/shared:$PATH" \
        LD_LIBRARY_PATH="$ROOT/build/shared${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        DYLD_LIBRARY_PATH="$ROOT/build/shared${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$TMP/host_dyn" "$TMP/shared/libaccts.$ext" "$rt" 2>"$TMP/shared.err" | tr -d '\r')"
    run_app "--shared-runtime" "$TMP/shared"
    shared_note=", and on the shared runtime"
else
    shared_note=" (no shared runtime in build/shared: that leg skipped)"
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] emit_lib_accounts: two accounts on one thread keep their allowances, deadlines and blocks apart, a child narrows its parent, frees credit the account charged, and an Aether or C host's account charges a library with its own runtime (one lock, one thread per account across the copies, a copy of another layout refused, an unloaded copy never written into, 70 copies charged): one runtime, the library's own$shared_note"
fi
exit $fail
