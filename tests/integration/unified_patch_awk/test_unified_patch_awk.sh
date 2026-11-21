#!/bin/sh
# scripts/apply-unified-patch.awk applies a plain unified diff with no outside
# tool. scripts/fetch-quickjs-amalgamation.sh falls back to it where neither
# patch(1) nor git is installed: a bare MSYS2 shell, which the Windows CI and
# release jobs run `make test-release-archive` in. There the stage step could
# not apply contrib/quickjs/patches/*.patch and failed the job.
#
# It must be exact: a hunk applies only at the place it names, a patch that
# does not fit changes nothing and fails, and what it writes is what patch(1)
# writes.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=unified_patch_awk
APPLY="$ROOT/scripts/apply-unified-patch.awk"
. "$ROOT/tests/lib/same_bytes.sh"

command -v awk >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no awk"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0
fail() { echo "  FAIL: $1"; fails=$((fails + 1)); }

mk() {
    mkdir -p "$TMP/$1/sub"
    printf 'one\ntwo\n    free(val);\n    free(index);\nfive\nsix\nseven\neight\nnine\nten\n' > "$TMP/$1/sub/f.c"
    printf 'keep\n' > "$TMP/$1/g.txt"
}

# Two hunks in one file (an insertion, then a replacement and a deletion
# further down, whose place the first hunk shifted), a second file, and a
# description before the first "---" line.
cat > "$TMP/good.patch" <<'PATCH'
A description that is not part of the diff.
Text before the first file header is ignored.

diff --git a/sub/f.c b/sub/f.c
--- a/sub/f.c
+++ b/sub/f.c
@@ -2,3 +2,4 @@
 two
     free(val);
+    val = 0;
     free(index);
@@ -7,3 +8,2 @@
-seven
-eight
+SEVEN
 nine
--- a/g.txt
+++ b/g.txt
@@ -1 +1,2 @@
 keep
+added
PATCH
mk a
if awk -v dir="$TMP/a" -f "$APPLY" "$TMP/good.patch" 2> "$TMP/a.err"; then
    printf 'one\ntwo\n    free(val);\n    val = 0;\n    free(index);\nfive\nsix\nSEVEN\nnine\nten\n' > "$TMP/want.c"
    same_bytes "$TMP/a/sub/f.c" "$TMP/want.c" || fail "two hunks: the file is not what the diff describes"
    printf 'keep\nadded\n' > "$TMP/want.txt"
    same_bytes "$TMP/a/g.txt" "$TMP/want.txt" || fail "the second file was not patched"
else
    fail "a good patch was refused: $(cat "$TMP/a.err")"
fi

# The same patch again no longer fits: refused, and nothing changes.
cp "$TMP/a/sub/f.c" "$TMP/before.c"
if awk -v dir="$TMP/a" -f "$APPLY" "$TMP/good.patch" 2> "$TMP/again.err"; then
    fail "a patch that no longer fits was applied a second time"
fi
grep -q "does not fit" "$TMP/again.err" || fail "the refusal does not say the hunk does not fit"
same_bytes "$TMP/a/sub/f.c" "$TMP/before.c" || fail "a refused patch changed the file"

# No searching: the right lines at the wrong place are refused.
mk b
sed 's/^@@ -2,3 +2,4 @@/@@ -3,3 +3,4 @@/' "$TMP/good.patch" > "$TMP/moved.patch"
if awk -v dir="$TMP/b" -f "$APPLY" "$TMP/moved.patch" 2>/dev/null; then
    fail "a hunk naming the wrong line was applied where its text happened to be"
fi

# What it does not handle is refused, not half done.
mk c
printf -- '--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1 @@\n+x\n' > "$TMP/newfile.patch"
awk -v dir="$TMP/c" -f "$APPLY" "$TMP/newfile.patch" 2>/dev/null && fail "a new-file patch was accepted"
[ -e "$TMP/c/new.txt" ] && fail "a new-file patch made a file"

# The same result as patch(1), where there is one.
if command -v patch >/dev/null 2>&1; then
    mk d
    (cd "$TMP/d" && patch -s -p1 -F0 -N < "$TMP/good.patch") >/dev/null 2>&1 &&
        { same_bytes "$TMP/d/sub/f.c" "$TMP/a/sub/f.c" || fail "patch(1) and the awk applier disagree"; }
fi

# The real thing: every patch contrib.quickjs carries applies with it, to a
# copy of the amalgamation as fetched, where one is here to copy. (The fetch
# script's own choice of tool is by what is installed; this is the path a
# machine with neither patch nor git takes.)
AMAL="$ROOT/contrib/quickjs/amalgamation"
if [ -f "$AMAL/quickjs-amalgam.c" ] && grep -q 'val = JS_UNDEFINED;' "$AMAL/quickjs-amalgam.c"; then
    mkdir -p "$TMP/q"
    # Undo the one carried change to get the file as released, then apply.
    awk '{ if (prev ~ /^        JS_FreeValue\(ctx, val\);$/ && $0 ~ /^        val = JS_UNDEFINED;$/ && !done) { done = 1; prev = $0; next } print; prev = $0 }' \
        "$AMAL/quickjs-amalgam.c" > "$TMP/q/quickjs-amalgam.c"
    if same_bytes "$TMP/q/quickjs-amalgam.c" "$AMAL/quickjs-amalgam.c"; then
        echo "  (the carried change was not found to undo; the amalgamation case is not run)"
    else
        for p in "$ROOT"/contrib/quickjs/patches/*.patch; do
            [ -f "$p" ] || continue
            awk -v dir="$TMP/q" -f "$APPLY" "$p" 2> "$TMP/q.err" || fail "$(basename "$p") does not apply with the awk applier: $(cat "$TMP/q.err")"
        done
        same_bytes "$TMP/q/quickjs-amalgam.c" "$AMAL/quickjs-amalgam.c" || fail "the awk applier's amalgamation differs from the fetched one"
    fi
fi

if [ "$fails" -eq 0 ]; then
    echo "  [PASS] $NAME: plain unified diffs apply exactly, with no patch(1) and no git"
    exit 0
fi
exit 1
