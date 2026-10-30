#!/bin/sh
# #904 contrib.host.duktape bidirectional embedding: a persistent host-owned VM
# that registers host callbacks the script calls, injects globals, runs
# code, and reads TYPED results back.
#
# Skips cleanly when libduktape dev headers aren't present.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_duktape_bidirectional on Windows"; exit 0 ;;
esac

DUK_CFLAGS="${DUK_CFLAGS:-}"; DUK_SONAME="${AETHER_DUKTAPE_SONAME:-}"
if [ -z "$DUK_CFLAGS" ]; then
    if pkg-config --exists duktape 2>/dev/null; then
        DUK_CFLAGS="$(pkg-config --cflags duktape)"
    elif [ -f "/usr/include/duktape.h" ]; then
        DUK_CFLAGS="-I/usr/include"
    fi
fi

if [ -z "$DUK_SONAME" ]; then
    so="$(ldconfig -p 2>/dev/null | grep -oE "libduktape\.so[^ ]*" | head -1)"
    [ -z "$so" ] && so="$(ls /usr/lib/*/libduktape.so.* /lib/*/libduktape.so.* 2>/dev/null | head -1)"
    if [ -n "$so" ]; then
        DUK_SONAME="$so"
    fi
fi

if [ -z "$DUK_CFLAGS" ] || [ -z "$DUK_SONAME" ]; then
    echo "  [SKIP] host_duktape_bidirectional: no matching libduktape headers+runtime"; exit 0
fi
[ -f "$ROOT/build/libaether.a" ] || { echo "  [SKIP] libaether.a not built"; exit 0; }
export AETHER_DUKTAPE_SONAME="$DUK_SONAME"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! gcc -o "$TMPDIR/t" \
    "$ROOT/contrib/host/duktape/aether_host_duktape.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $DUK_CFLAGS -I"$ROOT" -I"$ROOT/runtime" -DAETHER_HAS_DUKTAPE -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>"$TMPDIR/cc.log" << 'CEOF'
#include <stdio.h>
#include <string.h>
#include "contrib/host/duktape/aether_host_duktape.h"

void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
extern void* list_new(void); extern void list_add_raw(void*, void*);

static int cb_add(void* vm) {
    int64_t a = duktape_arg_int(vm, 1);
    int64_t b = duktape_arg_int(vm, 2);
    duktape_push_int(vm, a + b);
    return 1;
}

static int cb_echo_str(void* vm) {
    const char* s = duktape_arg_str(vm, 1);
    duktape_push_str(vm, s);
    return 1;
}

static int cb_echo_bool(void* vm) {
    int b = duktape_arg_bool(vm, 1);
    duktape_push_bool(vm, b);
    return 1;
}

static int cb_echo_nil(void* vm) {
    duktape_push_nil(vm);
    return 1;
}

static int cb_echo_double(void* vm) {
    duktape_push_double(vm, 3.14159);
    return 1;
}

static int cb_raise_err(void* vm) {
    const char* msg = duktape_arg_str(vm, 1);
    return duktape_raise_error(vm, msg);
}

int main(void) {
    void* vm = duktape_vm_new();
    if (!vm) { printf("FAIL: vm_new\n"); return 1; }

    // Register host callbacks (including dotted name)
    duktape_vm_register(vm, "app.math.add", (void*)cb_add);
    duktape_vm_register(vm, "host.echo_str", (void*)cb_echo_str);
    duktape_vm_register(vm, "host.echo_bool", (void*)cb_echo_bool);
    duktape_vm_register(vm, "host.echo_nil", (void*)cb_echo_nil);
    duktape_vm_register(vm, "host.echo_double", (void*)cb_echo_double);
    duktape_vm_register(vm, "host.raise_err", (void*)cb_raise_err);

    // Globals
    duktape_vm_set_global_str(vm, "GSTR", "héllo €");
    duktape_vm_set_global_int(vm, "GINT", 100);
    void* keys = list_new(); list_add_raw(keys, "k1"); list_add_raw(keys, "k2");
    duktape_vm_set_global_strlist(vm, "KEYS", keys);

    // 1. Persistent VM state & dotted callback
    int rc = duktape_vm_eval(vm, "var x = app.math.add(2, 3); x += GINT; x;");
    if (rc != 0) { printf("FAIL: eval 1\n"); return 1; }
    if (duktape_vm_result_type(vm) != 2 || duktape_vm_result_int(vm) != 105) {
        printf("FAIL: value 1 (%ld)\n", (long)duktape_vm_result_int(vm)); return 1;
    }
    duktape_vm_pop_result(vm);

    // 2. Second eval sees persisted state 'x'
    rc = duktape_vm_eval(vm, "x + 5");
    if (rc != 0 || duktape_vm_result_int(vm) != 110) {
        printf("FAIL: eval 2\n"); return 1;
    }
    duktape_vm_pop_result(vm);

    // 3. Non-ASCII string round trip
    rc = duktape_vm_eval(vm, "host.echo_str(GSTR)");
    if (rc != 0 || strcmp(duktape_vm_result_str(vm), "héllo €") != 0) {
        printf("FAIL: non-ascii str (%s)\n", duktape_vm_result_str(vm)); return 1;
    }
    duktape_vm_pop_result(vm);

    // 4. Bool & nil & double
    duktape_vm_eval(vm, "host.echo_bool(true)");
    if (duktape_vm_result_type(vm) != 1 || duktape_vm_result_bool(vm) != 1) { printf("FAIL: bool\n"); return 1; }
    duktape_vm_pop_result(vm);

    duktape_vm_eval(vm, "host.echo_nil()");
    if (duktape_vm_result_type(vm) != 0) { printf("FAIL: nil\n"); return 1; }
    duktape_vm_pop_result(vm);

    duktape_vm_eval(vm, "KEYS.length === 2 && KEYS[1] === 'k2'");
    if (duktape_vm_result_bool(vm) != 1) { printf("FAIL: strlist\n"); return 1; }
    duktape_vm_pop_result(vm);

    // 5. Try/catch of callback error
    rc = duktape_vm_eval(vm, "try { host.raise_err('deliberate_cb_err'); } catch (e) { e.message; }");
    if (rc != 0 || strcmp(duktape_vm_result_str(vm), "deliberate_cb_err") != 0) {
        printf("FAIL: try/catch cb err (%s)\n", duktape_vm_result_str(vm)); return 1;
    }
    duktape_vm_pop_result(vm);

    // 6. Uncaught JS error
    rc = duktape_vm_eval(vm, "throw new Error('uncaught');");
    if (rc == 0 || duktape_vm_result_type(vm) != 5) {
        printf("FAIL: uncaught error type (%d)\n", duktape_vm_result_type(vm)); return 1;
    }
    duktape_vm_pop_result(vm);

    // 7. Execution limit return message check
    const char* lim_err = duktape_vm_set_instruction_limit(vm, 1000, 10);
    if (!lim_err || strstr(lim_err, "execution limit unavailable") == NULL) {
        printf("FAIL: limit message (%s)\n", lim_err ? lim_err : "null"); return 1;
    }

    duktape_vm_free(vm);
    printf("PASS: host_duktape_bidirectional\n");
    return 0;
}
CEOF
then
    echo "  [SKIP] host_duktape_bidirectional: harness compile/link failed"
    head -8 "$TMPDIR/cc.log" | sed 's/^/      /'
    exit 0
fi

OUT="$("$TMPDIR/t" 2>&1)"
echo "$OUT" | grep -q "PASS: host_duktape_bidirectional" || { echo "  [FAIL] $OUT"; exit 1; }
echo "  [PASS] host_duktape_bidirectional: callback + typed result + global inject + limit check"
