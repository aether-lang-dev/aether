# Sourced by every instrument in this directory, first thing.
#
# The scripts are baked into the image, so editing one does nothing until the
# image is rebuilt. That failure is silent and it is the worst kind: the run
# succeeds and reports numbers from the instrument you thought you had just
# fixed, which is how a fix to the pid handling below got believed once before
# it was running. When the tree is mounted and differs, re-exec from the mount.
lbbench_use_mounted() {           # lbbench_use_mounted <script-name> "$@"
    local name="$1"; shift
    local src=/src/benchmarks/http/lbbench
    [ -z "${LBBENCH_FROM_SRC:-}" ] || return 0
    [ -f "$src/$name" ] || return 0
    cmp -s "$src/$name" "/bench/$name" && return 0
    printf '%s\n' "harness: running $name from the mounted tree, not the image" >&2
    export LBBENCH_FROM_SRC=1
    cp "$src"/*.sh "$src"/*.conf "$src"/*.cfg /bench/ 2>/dev/null || true
    chmod +x /bench/*.sh 2>/dev/null || true
    exec "/bench/$name" "$@"
}
