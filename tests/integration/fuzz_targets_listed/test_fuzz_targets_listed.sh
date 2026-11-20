#!/bin/sh
# Every fuzz target is run, and its decoder's changes reach it (#3749).
#
# tests/fuzz/select_targets.sh holds the one table of the targets and the
# paths each is built from; fuzz_ci.sh runs what it lists, and the fuzz
# workflow's pull_request paths filter decides whether a pull request fuzzes
# at all. A target missing from the table is never fuzzed on CI, and a path
# missing from the filter lets a change to its decoder through unfuzzed.
#
# Pinned:
#   1. every directory under tests/fuzz/ that has a corpus/ is a target the
#      table lists, and every listed target has a directory;
#   2. every listed target runs: run_fuzz_c.sh knows it, or it has a runner
#      of its own (run_fuzz_<target>.sh);
#   3. every path in the table is covered by the workflow's paths filter;
#   4. the selector's shards split the list: three shards hold every target
#      once;
#   5. each target's row names what it is built from: its C sources, the
#      modules its --with links and the std modules its harness imports,
#      followed through theirs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SEL="$ROOT/tests/fuzz/select_targets.sh"
WF="$ROOT/.github/workflows/fuzz.yml"
NAME=fuzz_targets_listed
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

[ -f "$SEL" ] || fail "tests/fuzz/select_targets.sh is missing"
all=" $(sh "$SEL" --all) "

for d in "$ROOT"/tests/fuzz/*/; do
    [ -d "$d/corpus" ] || continue
    t=$(basename "$d")
    case "$all" in *" $t "*) ;; *) fail "tests/fuzz/$t has a corpus and select_targets.sh does not list it" ;; esac
done
for t in $all; do
    [ -d "$ROOT/tests/fuzz/$t/corpus" ] || fail "select_targets.sh lists $t, which has no tests/fuzz/$t/corpus"
    if [ ! -f "$ROOT/tests/fuzz/$t/run_fuzz_$t.sh" ]; then
        grep -q "^    $t) " "$ROOT/tests/fuzz/run_fuzz_c.sh" || fail "nothing runs $t: no run_fuzz_$t.sh, and run_fuzz_c.sh does not know it"
    fi
done

# The workflow's pull_request paths: '<dir>/**' lines.
filters=$(sed -n "/^  pull_request:/,/^  [a-z_]*:\$/p" "$WF" | sed -n "s/^ *- '\\(.*\\)\\*\\*'\$/\\1/p")
[ -n "$filters" ] || fail "no paths filter found in fuzz.yml"
prefixes=$(sed -n "/^TABLE='/,/^'/p" "$SEL" | awk 'NF > 1 { for (i = 2; i <= NF; i++) print $i }' | sort -u)
for p in $prefixes; do
    covered=0
    for f in $filters; do
        case "$p" in "$f"*) covered=1 ;; esac
    done
    [ "$covered" = 1 ] || fail "fuzz.yml's paths filter does not cover $p, which select_targets.sh feeds a target from"
done

# 5. What a target is built from is in its row: the C sources run_fuzz_c.sh
# compiles into it, each module its --with links (std/<name>/, extern being
# no module), and every std module its Aether harness imports, followed
# through their own imports. A path is in the row when one of the row's
# prefixes, or one every target shares, starts it. tar, zip and x509 linked
# std.os, the crypto targets std.bignum and std.strbuilder, x509 std.fs,
# and a change to any of them fuzzed nothing.
shared=$(sed -n "/^SHARED='/,/'\$/p" "$SEL" | sed "s/^SHARED='//; s/'\$//")
row_of() { sed -n "/^TABLE='/,/^'/p" "$SEL" | awk -v t="$1" '$1 == t { $1 = ""; print }'; }
with_of() {
    if [ -f "$ROOT/tests/fuzz/$1/run_fuzz_$1.sh" ]; then
        sed -n 's/.*--with=\([A-Za-z0-9_,]*\).*/\1/p' "$ROOT/tests/fuzz/$1/run_fuzz_$1.sh" | head -1
    else
        grep "^    $1) " "$ROOT/tests/fuzz/run_fuzz_c.sh" | sed -n 's/.*AE_WITH="\([^"]*\)".*/\1/p'
    fi
}
sources_of() {
    awk -v t="$1" '
        $0 ~ "^    " t "\\) " { on = 1 }
        on { print; if ($0 ~ /;;/) exit }' "$ROOT/tests/fuzz/run_fuzz_c.sh" |
        grep -oE 'std/[A-Za-z0-9_./-]+\.c' | sed 's#/[^/]*$#/#'
}
# The directory of each std module the files given import, and of theirs.
imports_of() {
    todo="$*"; done_=" "
    while [ -n "$todo" ]; do
        set -- $todo; f="$1"; shift; todo="$*"
        case "$done_" in *" $f "*) continue ;; esac
        done_="$done_$f "
        for m in $(sed -n 's/^[[:space:]]*import[[:space:]][[:space:]]*std\.\([A-Za-z0-9_.]*\).*/\1/p' "$ROOT/$f" | sort -u); do
            d="std/$(printf '%s' "$m" | tr . /)"
            while [ ! -f "$ROOT/$d/module.ae" ] && [ "$d" != std ]; do d="${d%/*}"; done
            [ "$d" = std ] && continue
            echo "$d/"
            todo="$todo $d/module.ae"
        done
    done
}
for t in $all; do
    row="$(row_of "$t") $shared"
    need="$(sources_of "$t")"
    for w in $(with_of "$t" | tr ',' ' '); do
        [ "$w" = extern ] || need="$need std/$w/"
    done
    [ -f "$ROOT/tests/fuzz/$t/fuzz_$t.ae" ] && need="$need $(imports_of "tests/fuzz/$t/fuzz_$t.ae")"
    for p in $need; do
        in_row=0
        for r in $row; do
            case "$p" in "$r"*) in_row=1 ;; esac
        done
        [ "$in_row" = 1 ] || fail "$t is built from $p, and its row in select_targets.sh does not name it"
    done
done

shards="$(sh "$SEL" --all --shard 1/3) $(sh "$SEL" --all --shard 2/3) $(sh "$SEL" --all --shard 3/3)"
[ "$(printf '%s\n' $shards | sort | tr '\n' ' ')" = "$(printf '%s\n' $all | sort | tr '\n' ' ')" ] ||
    fail "the three shards do not hold every target once"

echo "  [PASS] $NAME: every fuzz target is listed, runs, and its paths reach the workflow"
