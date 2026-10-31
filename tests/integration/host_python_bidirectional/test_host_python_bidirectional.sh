#!/bin/sh
# #904 contrib.host.python bidirectional embedding: a persistent host-owned VM
# that registers host callbacks the script calls, injects globals, runs
# code, and reads TYPED results back.
#
# Skips cleanly when libpython dev headers / runtime aren't present.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_python_bidirectional on Windows"; exit 0 ;;
esac

if ! command -v python3-config >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] host_python_bidirectional: python3 development files unavailable"
    exit 0
fi

# Headers only: the bridge dlopens libpython, and the harness is linked
# WITHOUT -lpython, so a CPython call that skipped the dlsym table fails to
# link here instead of passing quietly.
PY_CFLAGS="$(python3-config --includes 2>/dev/null)"
PY_SONAME="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME") or "")' 2>/dev/null)"

if [ -z "$PY_CFLAGS" ] || [ -z "$PY_SONAME" ]; then
    echo "  [SKIP] host_python_bidirectional: libpython build metadata unavailable"
    exit 0
fi

if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] host_python_bidirectional: libaether.a not built"
    exit 0
fi

case "$PY_SONAME" in
    /*) ;;
    *)
        for PY_DIR in \
            "$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("PYTHONFRAMEWORKPREFIX") or "")' 2>/dev/null)" \
            "$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR") or "")' 2>/dev/null)"; do
            if [ -n "$PY_DIR" ] && [ -f "$PY_DIR/$PY_SONAME" ]; then
                PY_SONAME="$PY_DIR/$PY_SONAME"
                break
            fi
        done
        ;;
esac
export AETHER_PYTHON_SONAME="$PY_SONAME"

RT_LIB=""
[ "$(uname -s 2>/dev/null)" = "Linux" ] && RT_LIB="-lrt"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

# A copy of the bridge with a planted bug, for proving the cases can fail.
SOURCE_FILE="${AETHER_PYTHON_BRIDGE_SRC:-$ROOT/contrib/host/python/aether_host_python.c}"

if ! gcc -o "$TMPDIR/t" \
    "$SOURCE_FILE" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $PY_CFLAGS -I"$ROOT" -I"$ROOT/contrib/host/python" -I"$ROOT/runtime" -DAETHER_HAS_PYTHON -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether -ldl -lm $RT_LIB -lpthread \
    -xc - 2>"$TMPDIR/cc.log" << 'CEOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#include <math.h>

#include "contrib/host/python/aether_host_python.h"
#include "runtime/aether_shared_map.h"
#include "std/bytes/aether_bytes.h"

void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
extern void* list_new(void); extern void list_add_raw(void*, void*);

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

// Runs `code` and holds its result for the reads that follow.
static int ev(void* vm, const char* code) {
    python_vm_pop_result(vm);
    return python_vm_eval(vm, code);
}

static int cb_add(void* vm) {
    python_push_int(vm, python_arg_int(vm, 1) + python_arg_int(vm, 2));
    return 1;
}

static int cb_sub(void* vm) {
    python_push_int(vm, python_arg_int(vm, 1) - python_arg_int(vm, 2));
    return 1;
}

static int cb_echo_str(void* vm) {
    python_push_str(vm, python_arg_str(vm, 1));
    return 1;
}

static int cb_echo_bool(void* vm) {
    python_push_bool(vm, python_arg_bool(vm, 1));
    return 1;
}

static int cb_echo_nil(void* vm) {
    python_push_nil(vm);
    return 1;
}

static int cb_echo_double(void* vm) {
    python_push_double(vm, python_arg_double(vm, 1) * 2.0);
    return 1;
}

static int cb_echo_bytes(void* vm) {
    void* b = python_arg_bytes(vm, 1);
    python_push_bytes(vm, b);
    if (b) aether_bytes_free(b);
    return 1;
}

// The tags of all its arguments, as "t1,t2,...".
static int cb_types(void* vm) {
    char out[128] = "";
    int n = python_arg_count(vm);
    for (int i = 1; i <= n; i++) {
        char one[8];
        snprintf(one, sizeof(one), i > 1 ? ",%d" : "%d", python_arg_type(vm, i));
        strncat(out, one, sizeof(out) - strlen(out) - 1);
    }
    python_push_str(vm, out);
    return 1;
}

// Pushes, then returns 0: the script gets None.
static int cb_push_then_zero(void* vm) {
    python_push_int(vm, 99);
    return 0;
}

static int cb_raise_err(void* vm) {
    python_push_int(vm, 1);   // a push before a raise is dropped
    return python_raise_error(vm, python_arg_str(vm, 1));
}

static int cb_nested_eval(void* vm) {
    // A nested eval on the SAME VM, which calls another host callback.
    int rc = python_vm_eval(vm, "app.math.add(2, 4)");
    int64_t inner = rc == 0 ? python_vm_result_int(vm) : -1000;
    python_vm_pop_result(vm);
    // Read after the inner callback ran: without per-call state saved and
    // restored, arg 1 would be the inner call's 2.
    python_push_int(vm, python_arg_int(vm, 1) + inner);
    return 1;
}

static void* thread_worker(void* arg) {
    (void)arg;
    void* vm = python_vm_new();
    if (!vm) return (void*)1;
    python_vm_register(vm, "app.add", (void*)cb_add);
    python_vm_set_global_int(vm, "N", 0);
    for (int i = 0; i < 300; i++) {
        char code[64];
        snprintf(code, sizeof(code), "app.add(%d, 23)", i);
        if (python_vm_eval(vm, code) != 0 || python_vm_result_int(vm) != i + 23) {
            python_vm_free(vm);
            return (void*)2;
        }
        python_vm_pop_result(vm);
    }
    python_vm_free(vm);
    return NULL;
}

static long rss_kb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        long pages = 0, res = -1;
        if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = -1;
        fclose(f);
        if (res >= 0) return res * (sysconf(_SC_PAGESIZE) / 1024);
    }
    struct rusage ru;   // no /proc (macOS): peak RSS, in bytes there
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / 1024;
}

static void churn(void* vm, int rounds) {
    void* payload = aether_bytes_from_ptr("x\0y", 3);
    for (int i = 0; i < rounds; i++) {
        python_vm_set_global_str(vm, "LOOP_VAR", "a string long enough to show if each round keeps one");
        python_vm_eval(vm, "host.echo_str(LOOP_VAR)");
        python_vm_result_str(vm);
        python_vm_pop_result(vm);
        python_vm_eval(vm, "host.echo_bytes(b'x\\x00y' * 8)");
        void* b = python_vm_result_bytes(vm);
        if (b) aether_bytes_free(b);
        python_vm_pop_result(vm);
        python_vm_eval(vm, "tracked_obj");
        python_vm_pop_result(vm);
    }
    aether_bytes_free(payload);
}

int main(void) {
    void* vm = python_vm_new();
    if (!vm) { printf("FAIL: vm_new\n"); return 1; }

    CHECK(python_vm_register(vm, "app.math.add", (void*)cb_add) == 0, "register dotted");
    CHECK(python_vm_register(vm, "app.math.sub", (void*)cb_sub) == 0, "register into an existing namespace");
    python_vm_register(vm, "host.echo_str", (void*)cb_echo_str);
    python_vm_register(vm, "host.echo_bool", (void*)cb_echo_bool);
    python_vm_register(vm, "host.echo_nil", (void*)cb_echo_nil);
    python_vm_register(vm, "host.echo_double", (void*)cb_echo_double);
    python_vm_register(vm, "host.echo_bytes", (void*)cb_echo_bytes);
    python_vm_register(vm, "host.types", (void*)cb_types);
    python_vm_register(vm, "host.push_then_zero", (void*)cb_push_then_zero);
    python_vm_register(vm, "host.raise_err", (void*)cb_raise_err);
    python_vm_register(vm, "host.nested_eval", (void*)cb_nested_eval);
    CHECK(python_vm_register(vm, "bad..name", (void*)cb_add) != 0, "an empty name part is refused");
    CHECK(python_vm_register(vm, "trailing.", (void*)cb_add) != 0, "an empty leaf is refused");

    python_vm_set_global_str(vm, "GSTR", "héllo €");
    python_vm_set_global_int(vm, "GINT", 100);
    void* keys = list_new(); list_add_raw(keys, "k1"); list_add_raw(keys, "k 2");
    python_vm_set_global_strlist(vm, "KEYS", keys);

    // Statements give None; the globals dict persists between evals.
    int rc = ev(vm, "x = app.math.add(2, 3)\nx += GINT");
    CHECK(rc == 0 && python_vm_result_type(vm) == 0, "statements: rc=%d type=%d", rc, python_vm_result_type(vm));
    rc = ev(vm, "x + 5");
    CHECK(rc == 0 && python_vm_result_type(vm) == 2 && python_vm_result_int(vm) == 110,
          "expression after statements: rc=%d type=%d val=%ld", rc, python_vm_result_type(vm), (long)python_vm_result_int(vm));
    ev(vm, "app.math.sub(9, 4) * 10 + app.math.add(1, 1)");
    CHECK(python_vm_result_int(vm) == 52, "both functions in app.math: %ld", (long)python_vm_result_int(vm));

    ev(vm, "host.echo_str(GSTR)");
    CHECK(python_vm_result_type(vm) == 3 && strcmp(python_vm_result_str(vm), "héllo €") == 0,
          "non-ascii str: %s", python_vm_result_str(vm));
    ev(vm, "KEYS == ['k1', 'k 2']");
    CHECK(python_vm_result_type(vm) == 1 && python_vm_result_bool(vm) == 1, "strlist global");

    // Each argument's tag, as the callback sees it.
    ev(vm, "host.types(None, True, 7, 2.5, 's', b'b', [1], {}, 2**64, object())");
    CHECK(strcmp(python_vm_result_str(vm), "0,1,2,8,3,7,4,4,6,6") == 0, "arg types: %s", python_vm_result_str(vm));
    ev(vm, "host.echo_double(3)");
    CHECK(python_vm_result_type(vm) == 8 && python_vm_result_double(vm) == 6.0,
          "an int arg reads as a double: %f", python_vm_result_double(vm));
    ev(vm, "app.math.add(2.9, -1.9)");
    CHECK(python_vm_result_int(vm) == 1, "a float arg reads as a truncated int: %ld", (long)python_vm_result_int(vm));
    ev(vm, "host.echo_bool(True) is True and host.echo_bool(1) is False");
    CHECK(python_vm_result_bool(vm) == 1, "arg_bool reads only a bool");
    ev(vm, "host.echo_str(5)");
    CHECK(python_vm_result_type(vm) == 3 && strcmp(python_vm_result_str(vm), "") == 0,
          "arg_str does not convert an int: %s", python_vm_result_str(vm));
    ev(vm, "host.echo_str('\\udc80')");
    CHECK(python_vm_result_type(vm) == 3 && strcmp(python_vm_result_str(vm), "") == 0,
          "an unencodable str arg reads empty, with no error left set: type=%d %s",
          python_vm_result_type(vm), python_vm_result_str(vm));

    // Each pushed type, and the callback's return count.
    ev(vm, "host.echo_bool(True)");
    CHECK(python_vm_result_type(vm) == 1 && python_vm_result_bool(vm) == 1,
          "bool result: type=%d val=%d", python_vm_result_type(vm), python_vm_result_bool(vm));
    ev(vm, "host.echo_nil()");
    CHECK(python_vm_result_type(vm) == 0, "nil result: type=%d", python_vm_result_type(vm));
    ev(vm, "host.echo_double(2.5)");
    CHECK(python_vm_result_type(vm) == 8 && fabs(python_vm_result_double(vm) - 5.0) < 1e-9,
          "double result: type=%d val=%f", python_vm_result_type(vm), python_vm_result_double(vm));
    ev(vm, "host.push_then_zero()");
    CHECK(python_vm_result_type(vm) == 0, "a callback returning 0 returns None, whatever it pushed");

    // Results.
    ev(vm, "5");
    CHECK(python_vm_result_bool(vm) == 0 && strcmp(python_vm_result_str(vm), "") == 0,
          "an int is not a bool, nor a str");
    ev(vm, "-7.9");
    CHECK(python_vm_result_type(vm) == 8 && python_vm_result_int(vm) == -7, "a float reads as a truncated int");
    ev(vm, "1e300");
    CHECK(python_vm_result_int(vm) == INT64_MAX, "a huge float saturates: %ld", (long)python_vm_result_int(vm));
    ev(vm, "2**64");
    CHECK(python_vm_result_type(vm) == 6 && python_vm_result_int(vm) == 0 && python_vm_result_double(vm) == 18446744073709551616.0,
          "2**64: type=%d int=%ld double=%f", python_vm_result_type(vm), (long)python_vm_result_int(vm), python_vm_result_double(vm));
    ev(vm, "-2**63");
    CHECK(python_vm_result_type(vm) == 2 && python_vm_result_int(vm) == INT64_MIN, "-2**63 fits");
    ev(vm, "10**400");
    CHECK(python_vm_result_type(vm) == 6 && isinf(python_vm_result_double(vm)), "10**400 reads as inf");
    ev(vm, "[1, 2, 3]");
    CHECK(python_vm_result_type(vm) == 4, "list: type=%d", python_vm_result_type(vm));
    ev(vm, "{'k': 'v'}");
    CHECK(python_vm_result_type(vm) == 4, "dict: type=%d", python_vm_result_type(vm));
    ev(vm, "object()");
    CHECK(python_vm_result_type(vm) == 6, "object: type=%d", python_vm_result_type(vm));
    ev(vm, "class Liar:\n    __class__ = bytes\nliar = Liar()");
    ev(vm, "isinstance(liar, bytes)");
    CHECK(python_vm_result_bool(vm) == 1, "the liar passes isinstance");
    rc = ev(vm, "liar");
    CHECK(rc == 0 && python_vm_result_type(vm) == 6, "but its tag is its real type: rc=%d type=%d", rc, python_vm_result_type(vm));

    // Bytes: length-aware both ways, and never confused with str.
    ev(vm, "b'a\\x00b'");
    void* b = python_vm_result_bytes(vm);
    CHECK(python_vm_result_type(vm) == 7 && b && aether_bytes_length(b) == 3 &&
          memcmp(aether_bytes_data(b), "a\0b", 3) == 0, "bytes result with an embedded zero");
    if (b) aether_bytes_free(b);
    ev(vm, "host.echo_bytes(b'x\\x00y\\x00')");
    b = python_vm_result_bytes(vm);
    CHECK(python_vm_result_type(vm) == 7 && b && aether_bytes_length(b) == 4 &&
          memcmp(aether_bytes_data(b), "x\0y\0", 4) == 0, "bytes through a callback");
    if (b) aether_bytes_free(b);
    ev(vm, "host.echo_bytes(b'')");
    b = python_vm_result_bytes(vm);
    CHECK(python_vm_result_type(vm) == 7 && b && aether_bytes_length(b) == 0, "empty bytes stay bytes");
    if (b) aether_bytes_free(b);
    ev(vm, "host.echo_bytes('text')");
    CHECK(python_vm_result_type(vm) == 0, "a str is not bytes: type=%d", python_vm_result_type(vm));
    ev(vm, "'text'");
    CHECK(python_vm_result_bytes(vm) == NULL, "a str result does not read as bytes");

    // Definitions persist.
    rc = ev(vm, "def foo(n):\n    return n * 3\n");
    CHECK(rc == 0, "def");
    ev(vm, "foo(14)");
    CHECK(python_vm_result_int(vm) == 42, "call foo");

    // An exception, then a clean eval.
    rc = ev(vm, "1 / 0");
    CHECK(rc != 0 && python_vm_result_type(vm) == 5 &&
          strcmp(python_vm_result_str(vm), "ZeroDivisionError: division by zero") == 0,
          "exception: rc=%d type=%d %s", rc, python_vm_result_type(vm), python_vm_result_str(vm));
    rc = ev(vm, "100 + 200");
    CHECK(rc == 0 && python_vm_result_int(vm) == 300, "eval after exception: rc=%d val=%ld", rc, (long)python_vm_result_int(vm));
    rc = ev(vm, "def (");
    CHECK(rc != 0 && strncmp(python_vm_result_str(vm), "SyntaxError: ", 13) == 0, "syntax error: %s", python_vm_result_str(vm));
    ev(vm, "counter = 0");
    rc = ev(vm, "counter += 1\nraise ValueError('x')");
    ev(vm, "counter");
    CHECK(python_vm_result_int(vm) == 1, "a failing statement block ran once: %ld", (long)python_vm_result_int(vm));

    // raise_error, caught and uncaught.
    rc = ev(vm, "try:\n    host.raise_err('deliberate')\nexcept RuntimeError as e:\n    err_caught = str(e)\n");
    CHECK(rc == 0, "a raise is catchable: rc=%d %s", rc, python_vm_result_str(vm));
    ev(vm, "err_caught");
    CHECK(strcmp(python_vm_result_str(vm), "deliberate") == 0, "caught message: %s", python_vm_result_str(vm));
    rc = ev(vm, "host.raise_err('uncaught')");
    CHECK(rc != 0 && strcmp(python_vm_result_str(vm), "RuntimeError: uncaught") == 0,
          "uncaught raise: rc=%d %s", rc, python_vm_result_str(vm));

    // A nested eval from inside a callback.
    rc = ev(vm, "host.nested_eval(40)");
    CHECK(rc == 0 && python_vm_result_int(vm) == 46, "nested eval: rc=%d val=%ld", rc, (long)python_vm_result_int(vm));

    // Two VMs: a global in one is not in the other.
    void* vm2 = python_vm_new();
    python_vm_set_global_str(vm, "VM1_ONLY", "secret");
    python_vm_eval(vm2, "'VM1_ONLY' in globals()");
    CHECK(python_vm_result_type(vm2) == 1 && python_vm_result_bool(vm2) == 0, "two VMs share a global");
    python_vm_pop_result(vm2);

    // A host function kept past its VM's free raises; it doesn't reach the
    // freed VM.
    void* vm3 = python_vm_new();
    python_vm_register(vm3, "app.add", (void*)cb_add);
    python_vm_eval(vm3, "import builtins\nbuiltins.aether_kept = app.add");
    python_vm_pop_result(vm3);
    python_vm_free(vm3);
    rc = python_vm_eval(vm2, "aether_kept(1, 2)");
    CHECK(rc != 0 && strstr(python_vm_result_str(vm2), "after its vm was freed") != NULL,
          "a function outliving its VM: rc=%d %s", rc, python_vm_result_str(vm2));
    python_vm_pop_result(vm2);
    python_vm_eval(vm2, "del builtins.aether_kept");
    python_vm_pop_result(vm2);
    python_vm_free(vm2);

    // The instruction limit.
    CHECK(strcmp(python_vm_set_instruction_limit(vm, 5000, 1), "") == 0, "limit set returns \"\"");
    rc = ev(vm, "while True:\n    pass\n");
    CHECK(rc != 0 && strcmp(python_vm_result_str(vm), "RuntimeError: instruction limit exceeded") == 0,
          "while True: rc=%d %s", rc, python_vm_result_str(vm));
    rc = ev(vm, "while True:\n    try:\n        while True:\n            pass\n    except BaseException:\n        pass\n");
    CHECK(rc != 0, "catching the limit does not escape it");
    rc = ev(vm, "while True:\n    host.nested_eval(1)\n");
    CHECK(rc != 0 && strstr(python_vm_result_str(vm), "instruction limit") != NULL,
          "a nested eval does not reset or remove the limit: rc=%d %s", rc, python_vm_result_str(vm));
    for (int round = 0; round < 3; round++) {
        rc = ev(vm, "sum(i for i in range(200))");
        CHECK(rc == 0 && python_vm_result_int(vm) == 19900, "round %d within the budget: %s", round, python_vm_result_str(vm));
    }
    python_vm_set_instruction_limit(vm, 0, 1);
    rc = ev(vm, "sum(i for i in range(20000))");
    CHECK(rc == 0, "disarmed: %s", python_vm_result_str(vm));

    // Two threads on their own VMs at once.
    pthread_t t1, t2;
    void *r1 = (void*)9, *r2 = (void*)9;
    if (pthread_create(&t1, NULL, thread_worker, NULL) == 0 && pthread_create(&t2, NULL, thread_worker, NULL) == 0) {
        pthread_join(t1, &r1);
        pthread_join(t2, &r2);
    }
    CHECK(r1 == NULL && r2 == NULL, "thread workers: %p %p", r1, r2);

    // Leaks: refcount and gc object count flat; RSS flat across two loops.
    rc = ev(vm,
        "import sys, gc\n"
        "class Tracked:\n"
        "    pass\n"
        "gc.collect()\n"
        "tracked_obj = Tracked()\n"
        "init_ref = sys.getrefcount(tracked_obj)\n"
        "init_objs = len(gc.get_objects())\n");
    CHECK(rc == 0, "leak setup");
    python_vm_pop_result(vm);
    churn(vm, 2000);
    long a = rss_kb(); churn(vm, 10000);
    long bb = rss_kb(); churn(vm, 10000);
    long c = rss_kb();
    ev(vm, "gc.collect()\nfinal_ref = sys.getrefcount(tracked_obj)\nfinal_objs = len(gc.get_objects())\n");
    ev(vm, "final_ref == init_ref and abs(final_objs - init_objs) < 10");
    CHECK(python_vm_result_bool(vm) == 1, "refcount or gc objects grew");
    long g1 = bb - a, g2 = c - bb, g = g1 < g2 ? g1 : g2;
    CHECK(g < 256, "memory grows: %ld kB then %ld kB over 10000 rounds", g1, g2);
    python_vm_free(vm);

    // Finalize, then init again: a new interpreter, which the map helpers
    // must be registered into afresh.
    uint64_t token = 0;
    AetherSharedMap* map = aether_shared_map_new(&token);
    aether_shared_map_put(map, "k", "v");
    rc = python_run_sandboxed_with_map(list_new(), "assert aether_map_get('k') == 'v'", token);
    CHECK(rc == 0, "the map helpers: rc=%d", rc);
    python_finalize();
    CHECK(python_init() == 0, "re-init");
    void* vm4 = python_vm_new();
    CHECK(vm4 && python_vm_eval(vm4, "6 * 7") == 0 && python_vm_result_int(vm4) == 42, "a VM after re-init");
    python_vm_free(vm4);
    rc = python_run_sandboxed_with_map(list_new(), "assert aether_map_get('k') == 'v'", token);
    CHECK(rc == 0, "the map helpers after re-init: rc=%d", rc);
    python_finalize();

    if (failures) return 1;
    printf("PASS: host_python_bidirectional\n");
    return 0;
}
CEOF
then
    echo "  [FAIL] host_python_bidirectional: harness compile/link failed"
    head -8 "$TMPDIR/cc.log" | sed 's/^/      /'
    exit 1
fi

# A broken instruction limit hangs, so the run is bounded: it fails rather
# than stalling CI.
if command -v timeout >/dev/null 2>&1; then
    OUT="$(timeout 60 "$TMPDIR/t" 2>&1)"
    STATUS=$?
    if [ $STATUS -eq 124 ]; then
        echo "  [FAIL] host_python_bidirectional: test timed out"
        exit 1
    fi
else
    OUT="$("$TMPDIR/t" 2>&1)"
fi

echo "$OUT" | grep -q "PASS: host_python_bidirectional" || { echo "  [FAIL] host_python_bidirectional"; echo "$OUT" | sed 's/^/      /'; exit 1; }
echo "  [PASS] host_python_bidirectional: callbacks, typed values, bytes, nested evals, a callback outliving its VM, the limit, threads, flat memory, re-init"
