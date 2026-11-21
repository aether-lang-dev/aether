# Apply a plain unified diff with no outside tool, for machines that have
# neither patch(1) nor git (a bare MSYS2 shell, which the Windows release job
# runs scripts/stage-release.sh in).
#
#   awk -v dir=DIR -f scripts/apply-unified-patch.awk FILE.patch
#
# Paths in the diff lose their first component (as -p1) and are taken under
# DIR. A hunk applies only where its context and removed lines are exactly the
# file's lines at the place the hunk names, shifted by what earlier hunks
# added or removed: no fuzz and no searching, so a patch written for one
# release cannot land somewhere else in another. Exit 1, changing nothing, if
# any hunk does not fit. Text before the first "--- " line is the patch's own
# description. Not handled, and refused: new or deleted files, renames,
# binary patches and "\ No newline at end of file".

function fail(msg) {
    print "apply-unified-patch: " msg > "/dev/stderr"
    failed = 1
    exit 1
}

function strip(p,   i) {
    sub(/\t.*$/, "", p)
    i = index(p, "/")
    if (i == 0) fail("path '" p "' has no leading component to strip")
    return substr(p, i + 1)
}

# Read FILE into line[1..n]; a trailing carriage return stays part of the line.
function load(file,   l, n) {
    n = 0
    while ((getline l < file) > 0) line[++n] = l
    close(file)
    return n
}

function finish(   i, k, at, h, out) {
    if (file == "") return
    n = load(dir "/" file)
    if (n == 0) fail(file ": missing or empty under " dir)
    shift = 0
    for (h = 1; h <= nh; h++) {
        at = hstart[h] + shift
        for (k = 1; k <= oldn[h]; k++)
            if (line[at + k - 1] != old[h, k])
                fail(file ": hunk " h " does not fit at line " (at + k - 1))
        # splice: lines before, the hunk's new lines, lines after
        m = 0
        for (i = 1; i < at; i++) res[++m] = line[i]
        for (k = 1; k <= newn[h]; k++) res[++m] = new[h, k]
        for (i = at + oldn[h]; i <= n; i++) res[++m] = line[i]
        for (i = 1; i <= m; i++) line[i] = res[i]
        n = m
        shift += newn[h] - oldn[h]
    }
    out = dir "/" file
    printf "" > out
    for (i = 1; i <= n; i++) print line[i] > out
    close(out)
    applied++
    file = ""; nh = 0
}

BEGIN { file = ""; nh = 0; inhunk = 0; applied = 0; failed = 0 }

/^--- / && !inhunk_left() {
    finish()
    getline nextl
    if (nextl !~ /^\+\+\+ /) fail("'--- ' without a '+++ ' line after it")
    a = substr($0, 5); b = substr(nextl, 5)
    if (a ~ /^\/dev\/null/ || b ~ /^\/dev\/null/) fail("new and deleted files are not handled")
    file = strip(b)
    next
}

function inhunk_left() { return inhunk && (wantold > 0 || wantnew > 0) }

/^@@ / {
    if (file == "") fail("a hunk before any file")
    if (inhunk_left()) fail(file ": hunk " nh " is shorter than its header says")
    if (match($0, /^@@ -[0-9]+(,[0-9]+)? \+[0-9]+(,[0-9]+)? @@/) == 0) fail("bad hunk header: " $0)
    hdr = substr($0, 5, RLENGTH - 7)
    split(hdr, parts, " ")
    o = substr(parts[1], 1); nn = substr(parts[2], 2)
    wantold = 1; wantnew = 1; ostart = o
    if (index(o, ",")) { split(o, t, ","); ostart = t[1]; wantold = t[2] }
    if (index(nn, ",")) { split(nn, t, ","); wantnew = t[2] }
    nh++
    hstart[nh] = ostart + 0
    if (wantold == 0) hstart[nh]++      # an insertion names the line before it
    oldn[nh] = 0; newn[nh] = 0
    inhunk = 1
    next
}

inhunk_left() {
    c = substr($0, 1, 1); body = substr($0, 2)
    if (c == "\\") fail("'\\ No newline at end of file' is not handled")
    if (c == " " || $0 == "") { old[nh, ++oldn[nh]] = body; new[nh, ++newn[nh]] = body; wantold--; wantnew-- }
    else if (c == "-") { old[nh, ++oldn[nh]] = body; wantold-- }
    else if (c == "+") { new[nh, ++newn[nh]] = body; wantnew-- }
    else fail(file ": unexpected line in hunk " nh ": " $0)
    next
}

/^(diff |index |rename |similarity |new file|deleted file|Binary |GIT binary)/ {
    if ($0 ~ /^(rename |new file|deleted file|Binary |GIT binary)/) fail("not a plain text change: " $0)
    next
}

END {
    if (failed) exit 1
    if (inhunk_left()) fail(file ": hunk " nh " is shorter than its header says")
    finish()
    if (applied == 0) fail("no file was patched")
}
