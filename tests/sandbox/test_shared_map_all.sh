#!/bin/bash
# Test shared map across all available host modules
# Skips languages where dev packages aren't installed

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PRELOAD="$ROOT/build/libaether_sandbox.so"
PASS=0
FAIL=0

. "$ROOT/tests/sandbox/detect_host_langs.sh"

check() {
    local lang="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -q "$expected"; then
        echo "  [PASS] $lang: $expected"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] $lang: expected '$expected' in output"
        echo "         got: $actual"
        FAIL=$((FAIL+1))
    fi
}

skip() { echo "  [SKIP] $1: dev package not installed"; }

echo "============================================"
echo "  Shared Map Tests — All Host Modules"
echo "============================================"
echo ""

# --- Lua ---
echo "=== Lua ==="
if [ $LUA_AVAILABLE -eq 0 ]; then skip "Lua"; else
LUA_OUT=$(gcc -o /tmp/test_map_lua \
    "$ROOT/contrib/host/lua/aether_host_lua.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $LUA_CFLAGS -I"$ROOT/runtime" -DAETHER_HAS_LUA \
    -L"$ROOT/build" -laether $LUA_LIBS -ldl -lm -lrt \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdint.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
extern int lua_run_sandboxed_with_map(void*, const char*, uint64_t);
extern void lua_finalize(void);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    void* p = list_new();
    list_add(p,"env"); list_add(p,"*");
    list_add(p,"fs_read"); list_add(p,"/usr/*");
    list_add(p,"fs_read"); list_add(p,"/etc/*");
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    aether_shared_map_put(m, "name", "Alice");
    aether_shared_map_put(m, "age", "30");
    lua_run_sandboxed_with_map(p,
        "local n = aether_map_get('name')\n"
        "local a = aether_map_get('age')\n"
        "local s = aether_map_get('secret')\n"
        "aether_map_put('greeting', 'hello ' .. n)\n"
        "aether_map_put('name', 'TAMPERED')\n"
        "print('lua:name=' .. n .. ',age=' .. a .. ',secret=' .. (s or 'nil'))\n"
    , t);
    aether_shared_map_revoke_token(t);
    printf("lua:result=%s\n", aether_shared_map_get(m, "greeting"));
    printf("lua:untampered=%s\n", aether_shared_map_get(m, "name"));
    aether_shared_map_free(m); lua_finalize(); list_free(p);
    return 0;
}
CEOF
LD_PRELOAD="$PRELOAD" /tmp/test_map_lua 2>/dev/null)
check "Lua read" "lua:name=Alice,age=30,secret=nil" "$LUA_OUT"
check "Lua write" "lua:result=hello Alice" "$LUA_OUT"
check "Lua frozen" "lua:untampered=Alice" "$LUA_OUT"
fi

# --- JS (Duktape) ---
echo ""
echo "=== JS (Duktape) ==="
if [ $JS_AVAILABLE -eq 0 ]; then skip "JS"; else
JS_OUT=$(gcc -o /tmp/test_map_js \
    "$ROOT/contrib/host/duktape/aether_host_duktape.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $JS_CFLAGS -I"$ROOT/runtime" -DAETHER_HAS_DUKTAPE \
    -L"$ROOT/build" -laether $JS_LIBS -ldl -lm -lrt \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdint.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
extern int duktape_run_sandboxed_with_map(void*, const char*, uint64_t);
extern void duktape_finalize(void);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    void* p = list_new();
    list_add(p,"env"); list_add(p,"*");
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    aether_shared_map_put(m, "name", "Bob");
    aether_shared_map_put(m, "count", "7");
    duktape_run_sandboxed_with_map(p,
        "var n = aether_map_get('name');\n"
        "var c = aether_map_get('count');\n"
        "var s = aether_map_get('secret');\n"
        "aether_map_put('doubled', String(Number(c) * 2));\n"
        "aether_map_put('name', 'TAMPERED');\n"
        "print('js:name=' + n + ',count=' + c + ',secret=' + (s !== undefined ? s : 'nil'));\n"
    , t);
    aether_shared_map_revoke_token(t);
    printf("js:result=%s\n", aether_shared_map_get(m, "doubled"));
    printf("js:untampered=%s\n", aether_shared_map_get(m, "name"));
    aether_shared_map_free(m); duktape_finalize(); list_free(p);
    return 0;
}
CEOF
/tmp/test_map_js 2>/dev/null)
check "JS read" "js:name=Bob,count=7,secret=nil" "$JS_OUT"
check "JS write" "js:result=14" "$JS_OUT"
check "JS frozen" "js:untampered=Bob" "$JS_OUT"
fi

# --- Python ---
echo ""
echo "=== Python ==="
if [ $PY_AVAILABLE -eq 0 ]; then skip "Python"; else
PY_OUT=$(gcc -o /tmp/test_map_py \
    "$ROOT/contrib/host/python/aether_host_python.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $PY_CFLAGS -I"$ROOT/runtime" -DAETHER_HAS_PYTHON \
    -L"$ROOT/build" -laether $PY_LIBS -ldl -lm -lrt \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdint.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
extern int python_run_sandboxed_with_map(void*, const char*, uint64_t);
extern void python_finalize(void);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    void* p = list_new();
    list_add(p,"env"); list_add(p,"*");
    list_add(p,"fs_read"); list_add(p,"/usr/*");
    list_add(p,"fs_read"); list_add(p,"/lib/*");
    list_add(p,"fs_read"); list_add(p,"/etc/*");
    list_add(p,"fs_read"); list_add(p,"/home/*");
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    aether_shared_map_put(m, "name", "Charlie");
    aether_shared_map_put(m, "factor", "5");
    python_run_sandboxed_with_map(p,
        "n = aether_map_get('name')\n"
        "f = aether_map_get('factor')\n"
        "s = aether_map_get('secret')\n"
        "aether_map_put('product', str(int(f) * 10))\n"
        "aether_map_put('name', 'TAMPERED')\n"
        "print(f'py:name={n},factor={f},secret={s}')\n"
    , t);
    aether_shared_map_revoke_token(t);
    printf("py:result=%s\n", aether_shared_map_get(m, "product"));
    printf("py:untampered=%s\n", aether_shared_map_get(m, "name"));
    aether_shared_map_free(m); python_finalize(); list_free(p);
    return 0;
}
CEOF
LD_PRELOAD="$PRELOAD" /tmp/test_map_py 2>/dev/null)
check "Python read" "py:name=Charlie,factor=5,secret=None" "$PY_OUT"
check "Python write" "py:result=50" "$PY_OUT"
check "Python frozen" "py:untampered=Charlie" "$PY_OUT"
fi

# --- Perl ---
echo ""
echo "=== Perl ==="
if [ $PERL_AVAILABLE -eq 0 ]; then skip "Perl"; else
PERL_OUT=$(gcc -o /tmp/test_map_perl \
    "$ROOT/contrib/host/perl/aether_host_perl.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $PERL_CFLAGS -I"$ROOT/runtime" -DAETHER_HAS_PERL \
    -L"$ROOT/build" -laether $PERL_LIBS -lrt \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdint.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
extern int aether_perl_run_sandboxed_with_map(void*, const char*, uint64_t);
extern void aether_perl_finalize(void);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    void* p = list_new();
    list_add(p,"env"); list_add(p,"*");
    list_add(p,"fs_read"); list_add(p,"/usr/*");
    list_add(p,"fs_read"); list_add(p,"/etc/*");
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    aether_shared_map_put(m, "name", "Dana");
    aether_shared_map_put(m, "score", "88");
    aether_perl_run_sandboxed_with_map(p,
        "my $n = aether_map_get('name');\n"
        "my $s = aether_map_get('score');\n"
        "my $x = aether_map_get('secret');\n"
        "aether_map_put('grade', 'A');\n"
        "aether_map_put('name', 'TAMPERED');\n"
        "print \"perl:name=$n,score=$s,secret=\" . ($x // 'nil') . \"\\n\";\n"
    , t);
    aether_shared_map_revoke_token(t);
    printf("perl:untampered=%s\n", aether_shared_map_get(m, "name"));
    aether_shared_map_free(m); aether_perl_finalize(); list_free(p);
    return 0;
}
CEOF
LD_PRELOAD="$PRELOAD" /tmp/test_map_perl 2>/dev/null)
check "Perl read" "perl:name=Dana,score=88,secret=nil" "$PERL_OUT"
check "Perl frozen" "perl:untampered=Dana" "$PERL_OUT"
fi

# --- Ruby ---
echo ""
echo "=== Ruby ==="
if [ $RUBY_AVAILABLE -eq 0 ]; then skip "Ruby"; else
RUBY_OUT=$(gcc -o /tmp/test_map_ruby \
    "$ROOT/contrib/host/ruby/aether_host_ruby.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $RUBY_CFLAGS -I"$ROOT/runtime" -DAETHER_HAS_RUBY \
    -L"$ROOT/build" -laether $RUBY_LIBS -ldl -lm -lrt \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdint.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
extern int ruby_run_sandboxed_with_map(void*, const char*, uint64_t);
extern void ruby_finalize_host(void);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    void* p = list_new();
    list_add(p,"env"); list_add(p,"*");
    list_add(p,"fs_read"); list_add(p,"/usr/*");
    list_add(p,"fs_read"); list_add(p,"/etc/*");
    list_add(p,"fs_read"); list_add(p,"/lib/*");
    list_add(p,"fs_read"); list_add(p,"/home/*");
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    aether_shared_map_put(m, "name", "Eve");
    aether_shared_map_put(m, "level", "5");
    ruby_run_sandboxed_with_map(p,
        "n = aether_map_get('name')\n"
        "l = aether_map_get('level')\n"
        "s = aether_map_get('secret')\n"
        "aether_map_put('rank', 'senior')\n"
        "aether_map_put('name', 'TAMPERED')\n"
        "puts \"ruby:name=#{n},level=#{l},secret=#{s || 'nil'}\"\n"
    , t);
    aether_shared_map_revoke_token(t);
    printf("ruby:untampered=%s\n", aether_shared_map_get(m, "name"));
    aether_shared_map_free(m); ruby_finalize_host(); list_free(p);
    return 0;
}
CEOF
LD_PRELOAD="$PRELOAD" /tmp/test_map_ruby 2>/dev/null)
check "Ruby read" "ruby:name=Eve,level=5,secret=nil" "$RUBY_OUT"
check "Ruby frozen" "ruby:untampered=Eve" "$RUBY_OUT"
fi

# --- Java (cross-process via shm) ---
echo ""
echo "=== Java ==="
if [ $JAVA_AVAILABLE -eq 0 ] || [ ! -f "$ROOT/build/aether-sandbox.jar" ]; then skip "Java"; else
JAVA_OUT=$(gcc -o /tmp/test_map_java \
    "$ROOT/runtime/aether_shared_map.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    -I"$ROOT/runtime" \
    -L"$ROOT/build" -laether -ldl -lm -lrt \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    aether_shared_map_put(m, "user", "Frank");
    aether_shared_map_put(m, "threshold", "99");
    aether_shared_map_freeze_inputs(m);
    char* shm = aether_shared_map_to_shm(m);
    char* cwd = getcwd(NULL, 0);
    char jar[512], agent[512];
    snprintf(jar, sizeof(jar), "%s/build/aether-sandbox.jar", cwd);
    snprintf(agent, sizeof(agent), "-javaagent:%s", jar);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("AETHER_MAP_SHM", shm, 1);
        execlp("java", "java", "--enable-native-access=ALL-UNNAMED",
            agent, "-cp", jar, "aether.sandbox.SandboxTest", NULL);
        _exit(127);
    }
    int st; waitpid(pid, &st, 0);
    aether_shared_map_read_outputs_from_shm(m, shm);
    const char* result = aether_shared_map_get(m, "result");
    const char* status = aether_shared_map_get(m, "status");
    const char* user = aether_shared_map_get(m, "user");
    printf("java:result=%s\n", result ? result : "nil");
    printf("java:status=%s\n", status ? status : "nil");
    printf("java:untampered=%s\n", user);
    aether_shared_map_unlink_shm(shm);
    free(shm); free(cwd); aether_shared_map_free(m);
    return 0;
}
CEOF
/tmp/test_map_java 2>/dev/null)
check "Java read+write" "java:result=processed Frank" "$JAVA_OUT"
check "Java status" "java:status=ok" "$JAVA_OUT"
check "Java frozen" "java:untampered=Frank" "$JAVA_OUT"
fi

# --- Factor (embed-api fork; gated on env, not a dev package) ---
echo ""
echo "=== Factor ==="
if [ -z "$AETHER_FACTOR_SONAME" ] || [ -z "$AETHER_FACTOR_IMAGE" ] || \
   [ ! -f "$AETHER_FACTOR_SONAME" ] || [ ! -f "$AETHER_FACTOR_IMAGE" ]; then
    echo "  [SKIP] Factor: \$AETHER_FACTOR_SONAME / \$AETHER_FACTOR_IMAGE unset"
    echo "         (needs the aether-lang-dev/factor-language fork's libfactor + image)"
else
FACTOR_OUT=$(gcc -o /tmp/test_map_factor \
    "$ROOT/contrib/host/factor/aether_host_factor.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    -I"$ROOT" -I"$ROOT/runtime" -DAETHER_HAS_FACTOR -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>/dev/null << 'CEOF'
#include <stdio.h>
#include <stdint.h>
#include "aether_shared_map.h"
extern void* list_new(void); extern void list_add(void*, void*); extern void list_free(void*);
extern int factor_run_sandboxed_with_map(void*, const char*, uint64_t);
extern void factor_finalize(void);
void aether_args_init(int a, char** v){(void)a;(void)v;}
void* _aether_ctx_stack[64]; int _aether_ctx_depth = 0;
int main() {
    void* p = list_new();
    uint64_t t; AetherSharedMap* m = aether_shared_map_new(&t);
    // Hand Factor English text; it map-reduces a word-frequency histogram into
    // the shared map under keys it DISCOVERS at runtime (the words). Aether
    // never named those keys upfront — it reads them all back via the map.
    aether_shared_map_put(m, "text", "the cat sat on the mat the cat");
    factor_run_sandboxed_with_map(p,
        "USING: splitting sequences math math.parser namespaces kernel ;\n"
        "IN: scratchpad\n"
        "\"text\" aether-map-get \" \" split\n"
        "[ dup aether-map-get [ string>number ] [ 0 ] if* "
        "1 + number>string aether-map-put ] each\n"
    , t);
    aether_shared_map_revoke_token(t);
    // Discovered-key readback: count includes the runtime-created word keys.
    printf("factor:the=%s\n", aether_shared_map_get(m, "the"));
    printf("factor:cat=%s\n", aether_shared_map_get(m, "cat"));
    printf("factor:mat=%s\n", aether_shared_map_get(m, "mat"));
    printf("factor:keys=%d\n", aether_shared_map_count(m));
    aether_shared_map_free(m); factor_finalize(); list_free(p);
    return 0;
}
CEOF
AETHER_FACTOR_SONAME="$AETHER_FACTOR_SONAME" AETHER_FACTOR_IMAGE="$AETHER_FACTOR_IMAGE" \
    /tmp/test_map_factor 2>/dev/null)
check "Factor map-reduce the" "factor:the=3" "$FACTOR_OUT"
check "Factor map-reduce cat" "factor:cat=2" "$FACTOR_OUT"
check "Factor discovered keys" "factor:keys=6" "$FACTOR_OUT"
fi

# --- Summary ---
echo ""
echo "============================================"
echo "  $PASS passed, $FAIL failed"
echo "============================================"

rm -f /tmp/test_map_lua /tmp/test_map_js /tmp/test_map_py /tmp/test_map_perl /tmp/test_map_ruby /tmp/test_map_java /tmp/test_map_factor
exit $FAIL
