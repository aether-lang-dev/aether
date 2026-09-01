# Sourced by every instrument in this directory, first thing.
#
# The scripts are baked into the image, so editing one has no effect until the
# image is rebuilt. That failure is silent: the run succeeds and reports
# numbers from the version in the image, not the edited one. When the tree is
# mounted and differs, re-exec from the mount and say so.
lbbench_use_mounted() {           # lbbench_use_mounted <script-name> "$@"
    local name="$1"; shift
    local src=/src/benchmarks/http/lbbench
    [ -z "${LBBENCH_FROM_SRC:-}" ] || return 0
    [ -f "$src/$name" ] || return 0
    # Every file that gets copied below, not just the script being run. A
    # config is as easy to edit as a script and the effect of missing one is
    # the same silent wrong answer: nginx kept running with the worker count
    # baked into the image while the mounted config said something else.
    differs=0
    for f in "$src"/*.sh "$src"/*.conf "$src"/*.cfg; do
        [ -f "$f" ] || continue
        cmp -s "$f" "/bench/$(basename "$f")" || { differs=1; break; }
    done
    [ "$differs" -eq 0 ] && return 0
    printf '%s\n' "harness: running $name from the mounted tree, not the image" >&2
    export LBBENCH_FROM_SRC=1
    cp "$src"/*.sh "$src"/*.conf "$src"/*.cfg /bench/ 2>/dev/null || true
    chmod +x /bench/*.sh 2>/dev/null || true
    exec "/bench/$name" "$@"
}
