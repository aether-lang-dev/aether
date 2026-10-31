#!/bin/sh
# #904 contrib.host.tcl bidirectional embedding: a persistent interpreter per
# VM, host callbacks registered as Tcl commands, injected globals, and typed
# results, plus what is particular to Tcl: a callback's eval nested in a
# script's call, run at global level and inside the outer eval's limits; the
# command limit and the time limit (only the second stops `while 1 {}`); the
# refusal of a VM from a thread other than its own; and memory that stays
# flat across many rounds.
#
# Skips cleanly when the Tcl headers or libtcl aren't present.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_tcl_bidirectional on Windows"; exit 0 ;;
esac

# Headers are found separately from TCL_CFLAGS: pkg-config prints no flags
# when tcl.h sits in a default include directory.
TCL_CFLAGS="${TCL_CFLAGS:-}"; TCL_SONAME="${AETHER_TCL_SONAME:-}"; TCL_HEADERS=0
if [ -n "$TCL_CFLAGS" ]; then
    TCL_HEADERS=1
elif pkg-config --exists tcl 2>/dev/null; then
    TCL_CFLAGS="$(pkg-config --cflags-only-I tcl)"; TCL_HEADERS=1
elif [ -f /usr/include/tcl.h ] || [ -f /usr/local/include/tcl.h ]; then
    TCL_HEADERS=1
fi

if [ -z "$TCL_SONAME" ]; then
    so="$(echo 'puts libtcl$tcl_version.so' | tclsh 2>/dev/null)"
    [ -n "$so" ] && TCL_SONAME="$so"
fi

if [ "$TCL_HEADERS" -ne 1 ] || [ -z "$TCL_SONAME" ]; then
    echo "  [SKIP] host_tcl_bidirectional: no matching Tcl headers+runtime"; exit 0
fi
[ -f "$ROOT/build/libaether.a" ] || { echo "  [SKIP] libaether.a not built"; exit 0; }
export AETHER_TCL_SONAME="$TCL_SONAME"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! gcc -o "$TMPDIR/t" \
    "${AETHER_TCL_BRIDGE_SRC:-$ROOT/contrib/host/tcl/aether_host_tcl.c}" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $TCL_CFLAGS -I"$ROOT" -I"$ROOT/runtime" -DAETHER_HAS_TCL -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>"$TMPDIR/cc.log" << 'CEOF'
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "contrib/host/tcl/aether_host_tcl.h"

void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
extern void* list_new(void); extern void list_add_raw(void*, void*);

enum { TNIL, TBOOL, TINT, TSTR, TTABLE, TERR, TOTHER, TFLOAT };

static int failures = 0;
#define CHECK(cond, ...) do { if (getenv("TRACE")) printf("at %d\n", __LINE__); if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int cb_add(void* vm) {
    tcl_push_int(vm, tcl_arg_int(vm, 1) + tcl_arg_int(vm, 2));
    return 1;
}
static int cb_echo_str(void* vm) { tcl_push_str(vm, tcl_arg_str(vm, 1)); return 1; }
static int cb_bool(void* vm) { tcl_push_bool(vm, tcl_arg_bool(vm, 1)); return 1; }
static int cb_nil(void* vm) { tcl_push_nil(vm); return 1; }
static int cb_double(void* vm) { tcl_push_double(vm, tcl_arg_double(vm, 1) * 2); return 1; }
static int cb_nothing(void* vm) { tcl_push_int(vm, 99); return 0; }
static int cb_raise(void* vm) { return tcl_raise_error(vm, tcl_arg_str(vm, 1)); }

/* Each argument's type tag, as a string of digits. */
static int cb_types(void* vm) {
    char buf[32]; int n = tcl_arg_count(vm);
    for (int i = 0; i < n && i < 31; i++) buf[i] = (char)('0' + tcl_arg_type(vm, i + 1));
    buf[n < 31 ? n : 31] = '\0';
    tcl_push_str(vm, buf);
    return 1;
}

/* Evaluates on its own VM in the middle of the script's call, then reads
 * its own argument again: the nested call must leave it intact. Returns
 * arg1 * 1000 + the nested result + the level the nested eval ran at. */
static int cb_nested(void* vm) {
    int64_t before = tcl_arg_int(vm, 1);
    int64_t inner = -1, level = -1;
    if (tcl_vm_eval(vm, "app.math.add 20 3") == 0) inner = tcl_vm_result_int(vm);
    tcl_vm_pop_result(vm);
    if (tcl_vm_eval(vm, "info level") == 0) level = tcl_vm_result_int(vm);
    tcl_vm_pop_result(vm);
    int64_t after = tcl_arg_int(vm, 1);
    if (after != before) return tcl_raise_error(vm, "argument clobbered by the nested call");
    tcl_push_int(vm, after * 1000 + inner + level * 100);
    return 1;
}

/* Tcl turns `return` and a stray `break` into success and error only at the
 * outermost level; nested in a callback the bridge has to. Returns
 * "<rc>:<result>/<rc>:<type>/<rc>:<type>/<rc>:<type>". */
static int cb_nested_codes(void* vm) {
    char buf[64];
    int rc1 = tcl_vm_eval(vm, "return 5");
    int64_t v = tcl_vm_result_int(vm);
    tcl_vm_pop_result(vm);
    int rc2 = tcl_vm_eval(vm, "break");
    int t = tcl_vm_result_type(vm);
    tcl_vm_pop_result(vm);
    int rc3 = tcl_vm_eval(vm, "return -code error nope");
    int t3 = tcl_vm_result_type(vm);
    tcl_vm_pop_result(vm);
    int rc4 = tcl_vm_eval(vm, "return -level 2 up");
    int t4 = tcl_vm_result_type(vm);
    tcl_vm_pop_result(vm);
    snprintf(buf, sizeof buf, "%d:%lld/%d:%d/%d:%d/%d:%d", rc1, (long long)v, rc2, t, rc3, t3, rc4, t4);
    tcl_push_str(vm, buf);
    return 1;
}

/* A small nested eval, for the limit cases. */
static int cb_nest_small(void* vm) {
    tcl_vm_eval(vm, "set ::ticks [expr {[info exists ::ticks] ? $::ticks + 1 : 1}]");
    tcl_vm_pop_result(vm);
    return 0;
}

static void* other_thread(void* vm) {
    int rc = tcl_vm_eval(vm, "set x 1");
    static char out[200];
    snprintf(out, sizeof out, "%d|%d|%s", rc, tcl_vm_result_type(vm), tcl_vm_result_str(vm));
    return out;
}

static long rss_kb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    long pages = 0, res = 0;
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = -1;
    fclose(f);
    return res < 0 ? -1 : res * (sysconf(_SC_PAGESIZE) / 1024);
}

static void churn(void* vm, int rounds) {
    for (int i = 0; i < rounds; i++) {
        tcl_vm_set_global_str(vm, "CHURN", "a string long enough to show if each round keeps one");
        tcl_vm_eval(vm, "host.echo_str \"$CHURN [app.math.add 1 2]\"");
        tcl_vm_result_str(vm);
        tcl_vm_pop_result(vm);
        tcl_vm_eval(vm, "host.raise boom");
        tcl_vm_pop_result(vm);
        tcl_vm_eval(vm, "list a b [expr {1.5 * 2}]");
        /* not popped: the next eval must release it */
    }
    tcl_vm_pop_result(vm);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void* vm = tcl_vm_new();
    if (!vm) { printf("FAIL: vm_new\n"); return 1; }
    const char* names[] = {"app.math.add", "host.echo_str", "host.bool", "host.nil",
                           "host.double", "host.nothing", "host.raise", "host.types",
                           "host.nested", "host.nest_small", "ns::inner::add", "host.nested_codes"};
    int (*fns[])(void*) = {cb_add, cb_echo_str, cb_bool, cb_nil, cb_double, cb_nothing,
                           cb_raise, cb_types, cb_nested, cb_nest_small, cb_add, cb_nested_codes};
    for (int i = 0; i < 12; i++) CHECK(tcl_vm_register(vm, names[i], (void*)fns[i]) == 0, "register %s", names[i]);

    tcl_vm_set_global_str(vm, "GSTR", "héllo €");
    tcl_vm_set_global_int(vm, "GINT", 100);
    void* keys = list_new(); list_add_raw(keys, "k1"); list_add_raw(keys, "k 2");
    tcl_vm_set_global_strlist(vm, "KEYS", keys);

    /* Callback, global, state kept between evals. */
    CHECK(tcl_vm_eval(vm, "set x [expr {[app.math.add 2 3] + $GINT}]") == 0, "eval 1");
    CHECK(tcl_vm_result_type(vm) == TINT && tcl_vm_result_int(vm) == 105, "105, got %d/%lld", tcl_vm_result_type(vm), (long long)tcl_vm_result_int(vm));
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "expr {$x + 5}") == 0 && tcl_vm_result_int(vm) == 110, "state kept");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "ns::inner::add 4 5") == 0 && tcl_vm_result_int(vm) == 9, "namespaced command");
    tcl_vm_pop_result(vm);

    /* Typed results. */
    tcl_vm_eval(vm, "host.echo_str $GSTR");
    CHECK(tcl_vm_result_type(vm) == TSTR && strcmp(tcl_vm_result_str(vm), "héllo €") == 0, "non-ASCII string: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "host.double 1.25");
    CHECK(tcl_vm_result_type(vm) == TFLOAT && tcl_vm_result_double(vm) == 2.5, "double: %d %g", tcl_vm_result_type(vm), tcl_vm_result_double(vm));
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "host.bool yes");
    CHECK(tcl_vm_result_bool(vm) == 1 && tcl_vm_result_int(vm) == 1, "bool pushes 1");
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "host.nil");
    CHECK(tcl_vm_result_type(vm) == TNIL && strcmp(tcl_vm_result_str(vm), "") == 0, "nil is the empty value");
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "host.nothing");
    CHECK(tcl_vm_result_type(vm) == TNIL, "a callback returning 0 returns nothing, whatever it pushed");
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "list a b");
    CHECK(tcl_vm_result_type(vm) == TTABLE && strcmp(tcl_vm_result_str(vm), "a b") == 0, "list");
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "expr {2**70}");
    CHECK(tcl_vm_result_type(vm) == TOTHER && tcl_vm_result_int(vm) == INT64_MAX, "bignum saturates: %lld", (long long)tcl_vm_result_int(vm));
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "expr {-(2**70)}");
    CHECK(tcl_vm_result_int(vm) == INT64_MIN, "negative bignum saturates");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "return 7") == 0 && tcl_vm_result_int(vm) == 7, "top-level return is a result");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "return -code error top") == -1 && tcl_vm_result_type(vm) == TERR, "top-level return -code error");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "return -level 2 up") == -1 && strstr(tcl_vm_result_str(vm), "bad code"), "top-level return -level 2");
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "expr {[llength $KEYS] == 2 && [lindex $KEYS 1] eq {k 2}}");
    CHECK(tcl_vm_result_bool(vm) == 1, "string list global");
    tcl_vm_pop_result(vm);
    tcl_vm_eval(vm, "host.types [expr {5}] [expr {2.5}] [list a b] {} hello");
    CHECK(strcmp(tcl_vm_result_str(vm), "27403") == 0, "argument types: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);

    /* Errors. */
    CHECK(tcl_vm_eval(vm, "catch {host.raise deliberate} m; set m") == 0 && strcmp(tcl_vm_result_str(vm), "deliberate") == 0, "raise caught: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "host.raise uncaught") == -1 && tcl_vm_result_type(vm) == TERR && strcmp(tcl_vm_result_str(vm), "uncaught") == 0, "raise uncaught");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "break") == -1 && tcl_vm_result_type(vm) == TERR, "break outside a loop is an error");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "expr {1 +}") == -1 && tcl_vm_result_type(vm) == TERR, "syntax error");
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "expr {6 * 7}") == 0 && tcl_vm_result_type(vm) == TINT && tcl_vm_result_int(vm) == 42, "an eval after errors succeeds");
    tcl_vm_pop_result(vm);

    /* A nested eval from a callback a proc made: at global level, and the
     * outer callback's argument intact. */
    CHECK(tcl_vm_eval(vm, "proc p {} { host.nested 7 }; p") == 0, "nested eval: %s", tcl_vm_result_str(vm));
    CHECK(tcl_vm_result_int(vm) == 7023, "nested: want 7023, got %lld", (long long)tcl_vm_result_int(vm));
    tcl_vm_pop_result(vm);

    tcl_vm_eval(vm, "host.nested_codes");
    CHECK(strcmp(tcl_vm_result_str(vm), "0:5/-1:5/-1:5/-1:5") == 0, "nested return and break: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);

    /* Command limit: stops a loop of commands, cannot be caught, is per
     * outermost eval, and a nested eval does not renew it. */
    CHECK(strcmp(tcl_vm_set_instruction_limit(vm, 2000, 1), "") == 0, "set instruction limit");
    CHECK(tcl_vm_eval(vm, "while 1 {incr i}") == -1 && strstr(tcl_vm_result_str(vm), "command count limit"), "command limit: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);
    CHECK(tcl_vm_eval(vm, "catch {while 1 {incr i}}; set survived 1") == -1, "the command limit cannot be caught");
    tcl_vm_pop_result(vm);
    for (int r = 0; r < 4; r++) {
        CHECK(tcl_vm_eval(vm, "for {set j 0} {$j < 300} {incr j} {incr k}") == 0, "round %d within the budget: %s", r, tcl_vm_result_str(vm));
        tcl_vm_pop_result(vm);
    }
    CHECK(tcl_vm_eval(vm, "while 1 {host.nest_small}") == -1, "a nested eval does not renew the budget");
    tcl_vm_pop_result(vm);
    tcl_vm_set_instruction_limit(vm, 0, 1);

    /* Time limit: the only one that stops a loop of no commands. */
    CHECK(strcmp(tcl_vm_set_time_limit(vm, 300, 1), "") == 0, "set time limit");
    CHECK(tcl_vm_eval(vm, "while 1 {}") == -1 && strstr(tcl_vm_result_str(vm), "time limit"), "time limit: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);
    tcl_vm_set_time_limit(vm, 0, 1);
    CHECK(tcl_vm_eval(vm, "after 400; expr 1") == 0, "time limit removed: %s", tcl_vm_result_str(vm));
    tcl_vm_pop_result(vm);

    /* Another thread is refused, without touching Tcl, and the VM works
     * afterwards on its own. */
    pthread_t th; char* out = NULL;
    pthread_create(&th, NULL, other_thread, vm);
    pthread_join(th, (void**)&out);
    CHECK(strcmp(out, "-1|5|tcl vm used from a thread other than the one that created it") == 0, "other thread: %s", out);
    CHECK(tcl_vm_eval(vm, "expr {1 + 1}") == 0 && tcl_vm_result_int(vm) == 2, "own thread after a refusal");
    tcl_vm_pop_result(vm);

    /* Two VMs share nothing. */
    void* vm2 = tcl_vm_new();
    CHECK(vm2 != NULL, "second vm");
    CHECK(tcl_vm_eval(vm2, "list [info exists GINT] [llength [info commands app.math.add]]") == 0 && strcmp(tcl_vm_result_str(vm2), "0 0") == 0, "isolated: %s", tcl_vm_result_str(vm2));
    tcl_vm_pop_result(vm2);
    tcl_vm_free(vm2);

    /* Memory: two loops, the smaller growth bounded. A reference kept per
     * round grows a loop by several hundred kilobytes. */
    churn(vm, 20000);
    long a = rss_kb(); churn(vm, 20000);
    long b = rss_kb(); churn(vm, 20000);
    long c = rss_kb();
    long g1 = b - a, g2 = c - b, g = g1 < g2 ? g1 : g2;
    CHECK(g < 128, "memory grows: %ld kB then %ld kB over 20000 rounds", g1, g2);

    tcl_vm_free(vm);
    if (failures) return 1;
    printf("PASS: host_tcl_bidirectional\n");
    return 0;
}
CEOF
then
    # Headers and runtime are present, so a build failure is a real failure.
    echo "  [FAIL] host_tcl_bidirectional: harness compile/link failed"
    head -8 "$TMPDIR/cc.log" | sed 's/^/      /'
    exit 1
fi

OUT="$(timeout 60 "$TMPDIR/t" 2>&1)"
echo "$OUT" | grep -q "PASS: host_tcl_bidirectional" || { echo "  [FAIL] host_tcl_bidirectional"; echo "$OUT" | sed 's/^/      /'; exit 1; }
echo "  [PASS] host_tcl_bidirectional: callbacks, typed values, nested evals, both limits, thread check, flat memory"
