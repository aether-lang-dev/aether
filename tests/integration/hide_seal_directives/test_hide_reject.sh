#!/bin/sh
# Test that `hide` and `seal except` directives correctly REJECT code that
# tries to reach a name they've blocked. Each case writes a tiny .ae file
# that should fail to compile, then asserts the compiler exited non-zero.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# A directory of its own, not fixed /tmp paths: two sweeps at once (two
# checkouts, or parallel agents) overwrote each other's files (#2770).
AE_TMP="$(mktemp -d)"
trap 'rm -rf "$AE_TMP"' EXIT

pass=0
fail=0

run_reject() {
    case_name="$1"
    src_file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$src_file" -o $AE_TMP/ae_hide_out 2>/dev/null; then
        echo "  [FAIL] $case_name should have been rejected"
        fail=$((fail + 1))
    else
        echo "  [PASS] $case_name correctly rejected"
        pass=$((pass + 1))
    fi
}

# Rejected, and for the hide: the diagnostic names the hidden name, so a
# compiler that cannot parse the case does not pass it.
run_reject_hidden() {
    case_name="$1"
    src_file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$src_file" -o $AE_TMP/ae_hide_out > $AE_TMP/ae_hide_log 2>&1; then
        echo "  [FAIL] $case_name should have been rejected"
        fail=$((fail + 1))
    elif grep -q "is hidden in this scope" $AE_TMP/ae_hide_log; then
        echo "  [PASS] $case_name correctly rejected as hidden"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $case_name was rejected, but not as hidden:"
        sed 's/^/      /' $AE_TMP/ae_hide_log | head -5
        fail=$((fail + 1))
    fi
}

run_accept() {
    case_name="$1"
    src_file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$src_file" -o $AE_TMP/ae_hide_out 2>/dev/null; then
        echo "  [PASS] $case_name correctly accepted"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $case_name should have been accepted"
        fail=$((fail + 1))
    fi
}

# Case 1: direct read of a hidden variable from a nested block
cat > $AE_TMP/ae_hide_read.ae << 'EOF'
extern println(s: string)
main() {
    secret = "abc"
    {
        hide secret
        println(secret)   // E0304: secret is hidden
    }
}
EOF
run_reject "read of hidden variable" $AE_TMP/ae_hide_read.ae

# Case 2: direct write to a hidden variable from a nested block
cat > $AE_TMP/ae_hide_write.ae << 'EOF'
extern println(s: string)
main() {
    secret = "abc"
    {
        hide secret
        secret = "xyz"    // E0304: cannot declare 'secret', it is hidden
    }
}
EOF
run_reject "write to hidden variable" $AE_TMP/ae_hide_write.ae

# Case 3: seal except whitelist excludes a name that the body uses
cat > $AE_TMP/ae_seal_excluded.ae << 'EOF'
extern println(s: string)
main() {
    allowed = "ok"
    forbidden = "no"
    {
        seal except allowed, println
        println(forbidden)   // E0304: forbidden is sealed out
    }
}
EOF
run_reject "seal except excludes used name" $AE_TMP/ae_seal_excluded.ae

# Case 4: hide propagates to nested blocks
cat > $AE_TMP/ae_hide_nested.ae << 'EOF'
extern println(s: string)
main() {
    secret = "abc"
    {
        hide secret
        {
            println(secret)   // E0304: still hidden in deeper nest
        }
    }
}
EOF
run_reject "hide propagates to nested block" $AE_TMP/ae_hide_nested.ae

# Case 5: positive control — visible name in seal whitelist works
cat > $AE_TMP/ae_hide_ok.ae << 'EOF'
extern println(s: string)
main() {
    allowed = "ok"
    {
        seal except allowed, println
        println(allowed)
    }
}
EOF
run_accept "seal except whitelisted name" $AE_TMP/ae_hide_ok.ae

# Case 6: positive control — calling a visible function that USES the hidden var
cat > $AE_TMP/ae_hide_via_helper.ae << 'EOF'
extern println(s: string)
helper(s: string) { println(s) }
main() {
    secret = "abc"
    {
        hide secret
        // helper is visible (we didn't hide it). secret reaches helper
        // through helper's own lexical chain at the call site, not ours.
        helper("via helper")
    }
}
EOF
run_accept "calling visible helper while hiding the var" $AE_TMP/ae_hide_via_helper.ae

# Case 7: hide inside actor receive arm blocks outer state
cat > $AE_TMP/ae_hide_actor_receive.ae << 'EOF'
extern println(s: string)
message Ping {}
actor Server {
    state secret = "do not touch"
    state public_val = "ok"
    receive {
        Ping() -> {
            hide secret
            println(secret)   // E0304: secret is hidden
        }
    }
}
main() {
    s = spawn(Server())
    s ! Ping {}
    wait_for_idle()
}
EOF
run_reject "hide inside actor receive arm" $AE_TMP/ae_hide_actor_receive.ae

# Case 8: seal except inside actor receive arm blocks unlisted state
cat > $AE_TMP/ae_seal_actor_receive.ae << 'EOF'
extern println(s: string)
message Greet { name: string }
actor Greeter {
    state secret = "do not touch"
    state greeting = "hello"
    receive {
        Greet(name) -> {
            seal except name, greeting, println
            println(secret)   // E0304: secret is sealed out
        }
    }
}
main() {
    g = spawn(Greeter())
    g ! Greet { name: "world" }
    wait_for_idle()
}
EOF
run_reject "seal except inside actor receive arm" $AE_TMP/ae_seal_actor_receive.ae

# Case 9: hide on qualified name blocks prefix.member access. (This used
# `hide string`, which failed for the wrong reason: `string` is a reserved
# keyword, so the build stopped at E0100 before any hide check ran.)
cat > $AE_TMP/ae_hide_qualified.ae << 'EOF'
import std.fs
extern println(s: string)
main() {
    {
        hide fs
        println(fs.exists("/tmp"))   // rejected: fs is hidden
    }
}
EOF
run_reject "hide blocks qualified name access" $AE_TMP/ae_hide_qualified.ae

# Cases 10-13: a hidden or sealed-out module prefix stays blocked in every
# block nested inside the hiding one, as a bare name does (case 4). The
# qualified check used to look only at the innermost scope, so one level
# of nesting walked straight past `hide fs`.
cat > $AE_TMP/ae_hide_qualified_nested.ae << 'EOF'
import std.fs
extern println(s: string)
main() {
    {
        hide fs
        {
            println(fs.exists("/tmp"))
        }
    }
}
EOF
run_reject "hide blocks qualified access in a nested block" $AE_TMP/ae_hide_qualified_nested.ae

cat > $AE_TMP/ae_hide_qualified_if.ae << 'EOF'
import std.fs
check() -> int {
    hide fs
    if 1 == 1 {
        return fs.exists("/tmp")
    }
    return 0
}
main() { check() }
EOF
run_reject "hide blocks qualified access in an if body" $AE_TMP/ae_hide_qualified_if.ae

cat > $AE_TMP/ae_hide_qualified_closure.ae << 'EOF'
import std.fs
extern println(s: string)
main() {
    {
        hide fs
        g = || { println(fs.exists("/tmp")) }
        g()
    }
}
EOF
run_reject "hide blocks qualified access in a closure" $AE_TMP/ae_hide_qualified_closure.ae

cat > $AE_TMP/ae_seal_qualified_nested.ae << 'EOF'
import std.fs
extern println(s: string)
main() {
    {
        seal except println
        {
            println(fs.exists("/tmp"))
        }
    }
}
EOF
run_reject "seal except blocks qualified access in a nested block" $AE_TMP/ae_seal_qualified_nested.ae

# Cases 14-15: a hide covers its own block and what is nested in it,
# nothing else; a seal lets its whitelist through at any depth.
cat > $AE_TMP/ae_hide_qualified_sibling.ae << 'EOF'
import std.fs
extern println(s: string)
main() {
    {
        hide fs
        println("no fs here")
    }
    {
        println(fs.exists("/tmp"))
    }
    println(fs.exists("/tmp"))
}
EOF
run_accept "hide on a qualified prefix does not leak to sibling or outer blocks" $AE_TMP/ae_hide_qualified_sibling.ae

cat > $AE_TMP/ae_seal_qualified_whitelisted.ae << 'EOF'
import std.fs
extern println(s: string)
main() {
    {
        seal except println, fs
        {
            println(fs.exists("/tmp"))
        }
    }
}
EOF
run_accept "seal except lets a whitelisted prefix through nested blocks" $AE_TMP/ae_seal_qualified_whitelisted.ae

# Cases 16-19: DSL receiver fallback is still an outward lookup. A hidden or
# sealed-out bare name must not reach the receiver-prefixed function, while a
# name admitted by the seal remains available through that fallback.
mkdir -p $AE_TMP/dsl_helper
cat > $AE_TMP/dsl_helper/module.ae << 'EOF'
exports(run, script)

builder run(label: string) {}

script(_ctx: ptr, name: string) {}
EOF

cat > $AE_TMP/ae_hide_dsl_receiver.ae << 'EOF'
import dsl_helper
main() {
    dsl_helper.run("direct") {
        hide script
        script("blocked")
    }
}
EOF
run_reject "hide blocks DSL receiver fallback" $AE_TMP/ae_hide_dsl_receiver.ae

cat > $AE_TMP/ae_seal_dsl_receiver_nested.ae << 'EOF'
import dsl_helper
main() {
    dsl_helper.run("nested") {
        seal except println
        {
            script("blocked")
        }
    }
}
EOF
run_reject "seal except blocks nested DSL receiver fallback" $AE_TMP/ae_seal_dsl_receiver_nested.ae

cat > $AE_TMP/ae_seal_dsl_receiver_allowed.ae << 'EOF'
import dsl_helper
main() {
    dsl_helper.run("allowed") {
        seal except script
        script("ok")
    }
}
EOF
run_accept "seal except admits whitelisted DSL receiver fallback" $AE_TMP/ae_seal_dsl_receiver_allowed.ae

cat > $AE_TMP/ae_hide_dsl_receiver_outer.ae << 'EOF'
import dsl_helper
main() {
    {
        hide script
        {
            dsl_helper.run("outer") {
                script("blocked")
            }
        }
    }
}
EOF
run_reject "hide blocks DSL receiver fallback across nested scopes" $AE_TMP/ae_hide_dsl_receiver_outer.ae

# Cases 20-22: tuple destructuring is a write to every named slot. It must
# not re-bind an outer name that the current scope hid or sealed out.
cat > $AE_TMP/ae_hide_destructure.ae << 'EOF'
pair() -> (string, int) { return "changed", 1 }
main() {
    secret = "original"
    {
        hide secret
        secret, _ = pair()
    }
}
EOF
run_reject "hide blocks tuple-destructure write" $AE_TMP/ae_hide_destructure.ae

cat > $AE_TMP/ae_seal_destructure.ae << 'EOF'
pair() -> (string, int) { return "changed", 1 }
main() {
    secret = "original"
    {
        seal except pair
        secret, _ = pair()
    }
}
EOF
run_reject "seal except blocks tuple-destructure write" $AE_TMP/ae_seal_destructure.ae

cat > $AE_TMP/ae_seal_local_destructure.ae << 'EOF'
pair() -> (string, int) { return "changed", 1 }
main() {
    {
        seal except pair
        local = "original"
        local, fresh = pair()
    }
}
EOF
run_accept "seal except permits local tuple-destructure bindings" $AE_TMP/ae_seal_local_destructure.ae

# One member of a module: `hide ns.member` and `import ns hiding (member)`
# deny that member and leave the rest of the module visible (sae's kernel
# imports all of ui but ui.open_url). A module of our own, beside the files.
mkdir -p $AE_TMP/member/shell $AE_TMP/member/kernel
cat > $AE_TMP/member/shell/module.ae << 'EOF'
open_url(u: string) -> int { return 7 }
window(t: string) -> int { return 3 }
EOF

cat > $AE_TMP/member/hide_member_call.ae << 'EOF'
import shell
main() {
    {
        hide shell.open_url
        println("${shell.open_url("u")}")
    }
}
EOF
run_reject_hidden "hide ns.member blocks a call to that member" $AE_TMP/member/hide_member_call.ae

cat > $AE_TMP/member/hide_member_rest.ae << 'EOF'
import shell
main() {
    println("${shell.open_url("u")}")
    {
        hide shell.open_url
        println("${shell.window("t")}")
    }
}
EOF
run_accept "hide ns.member leaves the module's other members" $AE_TMP/member/hide_member_rest.ae

cat > $AE_TMP/member/hide_member_value.ae << 'EOF'
import shell
main() {
    hide shell.open_url
    f = shell.open_url
}
EOF
run_reject_hidden "hide ns.member blocks the member taken as a value" $AE_TMP/member/hide_member_value.ae

cat > $AE_TMP/member/hide_ns_value.ae << 'EOF'
import shell
main() {
    hide shell
    f = shell.window
}
EOF
run_reject_hidden "hide ns blocks a member taken as a value" $AE_TMP/member/hide_ns_value.ae

cat > $AE_TMP/member/hide_member_alias.ae << 'EOF'
import shell as sh
main() {
    hide shell.open_url
    println("${sh.open_url("u")}")
}
EOF
run_reject_hidden "hide ns.member holds through an import alias" $AE_TMP/member/hide_member_alias.ae

cat > $AE_TMP/member/hiding_call.ae << 'EOF'
import shell hiding (open_url)
main() {
    println("${shell.open_url("u")}")
}
EOF
run_reject_hidden "import hiding blocks the member" $AE_TMP/member/hiding_call.ae

cat > $AE_TMP/member/hiding_closure.ae << 'EOF'
import shell hiding (open_url)
run(code: fn) { call(code, null) }
main() {
    run() callback {
        x = shell.open_url("u")
    }
}
EOF
run_reject_hidden "import hiding blocks the member in a trailing block" $AE_TMP/member/hiding_closure.ae

cat > $AE_TMP/member/hiding_actor.ae << 'EOF'
import shell hiding (open_url)
actor A {
    state n = 0
    receive {
        Ping() -> { n = shell.open_url("a") }
    }
}
main() { println("x") }
EOF
run_reject_hidden "import hiding blocks the member in a receive arm" $AE_TMP/member/hiding_actor.ae

cat > $AE_TMP/member/hiding_alias.ae << 'EOF'
import shell as sh hiding (open_url)
main() {
    println("${sh.open_url("u")}")
}
EOF
run_reject_hidden "import as alias hiding blocks the member" $AE_TMP/member/hiding_alias.ae

cat > $AE_TMP/member/hiding_rest.ae << 'EOF'
import shell hiding (open_url)
twice() -> int {
    if shell.window("a") == 3 { return 6 }
    return 0
}
main() {
    println("${shell.window("t")} ${twice()}")
}
EOF
run_accept "import hiding leaves the module's other members" $AE_TMP/member/hiding_rest.ae

cat > $AE_TMP/member/hiding_select.ae << 'EOF'
import shell (window) hiding (open_url)
main() { println("x") }
EOF
run_reject "import hiding is not combined with a selective list" $AE_TMP/member/hiding_select.ae

# The kernel module hides open_url from itself; the program that imports
# it may still call shell.open_url, since the hide is the kernel file's.
# kernel finds shell where the compile runs, so these two run from there.
cd $AE_TMP/member
cat > $AE_TMP/member/kernel/module.ae << 'EOF'
import shell hiding (open_url)
boot() -> int { return shell.window("k") }
EOF
cat > $AE_TMP/member/hiding_per_file.ae << 'EOF'
import kernel
import shell
main() { println("${kernel.boot()} ${shell.open_url("u")}") }
EOF
run_accept "import hiding is the importing file's alone" $AE_TMP/member/hiding_per_file.ae

cat > $AE_TMP/member/kernel/module.ae << 'EOF'
import shell hiding (open_url)
boot() -> int { return shell.open_url("k") }
EOF
run_reject_hidden "import hiding in a module blocks the member there" $AE_TMP/member/hiding_per_file.ae
cd "$ROOT"

# Cleanup
rm -f $AE_TMP/ae_hide_*.ae $AE_TMP/ae_seal_*.ae $AE_TMP/ae_hide_out $AE_TMP/ae_hide_log

echo ""
echo "Hide/seal rejection tests: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then exit 1; fi
