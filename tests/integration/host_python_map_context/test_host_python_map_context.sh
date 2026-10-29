#!/bin/sh
# The in-process Python host keeps the shared-map token scoped to each
# run_sandboxed_with_map call. A nested run must restore the outer token, and
# the outer return must leave no token active.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] host_python_map_context on Windows"
        exit 0
        ;;
esac

if ! command -v python3-config >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] host_python_map_context: python3 development files unavailable"
    exit 0
fi

PY_CFLAGS="$(python3-config --embed --cflags 2>/dev/null || python3-config --cflags 2>/dev/null)"
PY_LIBS="$(python3-config --embed --ldflags 2>/dev/null || python3-config --ldflags 2>/dev/null)"
PY_SONAME="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME") or "")' 2>/dev/null)"
if [ -z "$PY_CFLAGS" ] || [ -z "$PY_LIBS" ] || [ -z "$PY_SONAME" ]; then
    echo "  [SKIP] host_python_map_context: libpython build metadata unavailable"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] host_python_map_context: libaether.a not built"
    exit 0
fi
export AETHER_PYTHON_SONAME="$PY_SONAME"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if ! gcc -o "$TMPDIR/test_python_map_context" \
    "$ROOT/contrib/host/python/aether_host_python.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $PY_CFLAGS -I"$ROOT" -I"$ROOT/runtime" \
    -DAETHER_HAS_PYTHON -DAETHER_HAS_SANDBOX -rdynamic \
    -L"$ROOT/build" -laether $PY_LIBS -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>"$TMPDIR/cc.log" <<'CEOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contrib/host/python/aether_host_python.h"
#include "runtime/aether_shared_map.h"

extern void *list_new(void);
extern void list_free(void *);

void aether_args_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

void *_aether_ctx_stack[64];
int _aether_ctx_depth = 0;

static const char *shown(const char *value)
{
    return value ? value : "(null)";
}

int main(void)
{
    uint64_t outer_token = 0;
    uint64_t inner_token = 0;
    AetherSharedMap *outer = aether_shared_map_new(&outer_token);
    AetherSharedMap *inner = aether_shared_map_new(&inner_token);
    void *perms = list_new();
    char code[4096];

    if (!outer || !inner || !perms)
    {
        fprintf(stderr, "FAIL setup\n");
        return 1;
    }

    aether_shared_map_put(outer, "scope", "outer");
    aether_shared_map_put(inner, "scope", "inner");

    int written = snprintf(
        code, sizeof(code),
        "import ctypes\n"
        "nested = ctypes.PyDLL(None).python_run_sandboxed_with_map\n"
        "nested.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64]\n"
        "nested.restype = ctypes.c_int\n"
        "assert aether_map_get('scope') == 'outer'\n"
        "rc = nested(None, b\"assert aether_map_get('scope') == 'inner'\\naether_map_put('inner_result', 'ok')\\n\", %llu)\n"
        "assert rc == 0\n"
        "restored = aether_map_get('scope')\n"
        "aether_map_put('outer_result', 'missing' if restored is None else restored)\n",
        (unsigned long long)inner_token);
    if (written < 0 || (size_t)written >= sizeof(code))
    {
        fprintf(stderr, "FAIL generated Python source\n");
        return 1;
    }

    int run_rc = python_run_sandboxed_with_map(perms, code, outer_token);
    const char *inner_result = aether_shared_map_get(inner, "inner_result");
    const char *outer_result = aether_shared_map_get(outer, "outer_result");
    int cleared_rc = python_run(
        "assert aether_map_get('scope') is None, 'map token survived outer return'");

    printf("run=%d inner=%s outer=%s cleared=%d\n",
        run_rc, shown(inner_result), shown(outer_result), cleared_rc);

    int ok = run_rc == 0 && inner_result && outer_result &&
        cleared_rc == 0 && strcmp(inner_result, "ok") == 0 &&
        strcmp(outer_result, "outer") == 0;

    aether_shared_map_revoke_token(inner_token);
    aether_shared_map_revoke_token(outer_token);
    aether_shared_map_free(inner);
    aether_shared_map_free(outer);
    python_finalize();
    list_free(perms);

    if (!ok)
    {
        fprintf(stderr, "FAIL Python host did not restore the outer map context\n");
        return 1;
    }
    puts("PASS Python host restored and cleared nested map contexts");
    return 0;
}
CEOF
then
    echo "  [SKIP] host_python_map_context: harness compile/link failed"
    head -12 "$TMPDIR/cc.log" | sed 's/^/      /'
    exit 0
fi

"$TMPDIR/test_python_map_context"
