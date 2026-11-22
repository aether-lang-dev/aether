#!/bin/sh
# Task execution contexts on an emulated ESP32 (docs/design/
# task-execution-contexts.md): tests/esp32/contexts/main/host_esp.c runs two
# FreeRTOS tasks entering one cooperative Aether runtime, each with its own
# context and account, beside a native task, under Espressif's QEMU, for the
# ESP32 (Xtensa), ESP32-S3 (Xtensa) and ESP32-C3 (RISC-V). Each image must
# have no thread-local section from Aether, so no task carries Aether state
# on its stack.
#
#   sh tests/esp32/test_esp32_contexts.sh [targets]   (default: esp32 esp32c3 esp32s3)
#
# Skips without podman or docker; ESP32_IDF_IMAGE and ESP32_ENGINE as in
# test_esp32.sh.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
IMAGE="${ESP32_IDF_IMAGE:-docker.io/espressif/idf:v6.1}"
TARGETS="${*:-esp32 esp32c3 esp32s3}"
ENGINE="${ESP32_ENGINE:-}"
[ -n "$ENGINE" ] || for e in podman docker; do
    if command -v "$e" > /dev/null 2>&1; then ENGINE="$e"; break; fi
done
[ -n "$ENGINE" ] || { echo "  [SKIP] esp32 contexts: neither podman nor docker is installed"; exit 0; }
[ -x "$AETHERC" ] || { echo "  [FAIL] esp32 contexts: $AETHERC not built"; exit 1; }

tmp="$(mktemp -d)"
# ESP32_KEEP=dir keeps the consoles, build logs and program headers there.
trap '[ -n "${ESP32_KEEP:-}" ] && cp -r "$tmp/out/." "$ESP32_KEEP/" 2>/dev/null; rm -rf "$tmp" || true' EXIT
mkdir -p "$tmp/gen" "$tmp/out"
chmod 777 "$tmp/out"
AETHER_HOME="$ROOT" "$AETHERC" --emit=lib "$ROOT/tests/integration/coop_task_contexts/ctxlib.ae" \
    "$tmp/gen/ctxlib.c" > "$tmp/gen.log" 2>&1 || { echo "  [FAIL] esp32 contexts: aetherc failed"; exit 1; }
"$ENGINE" run --rm -v "$ROOT:/aether:ro" -v "$SCRIPT_DIR:/esp32:ro" -v "$tmp/gen:/gen:ro" -v "$tmp/out:/out" \
    "$IMAGE" bash /esp32/contexts_in_container.sh $TARGETS > "$tmp/engine.log" 2>&1

fail=0
for t in $TARGETS; do
    log="$tmp/out/$t.log"
    out="$(tr -d '\r' < "$log" 2>/dev/null | grep '^RESULT')"
    # Thread-local sections Aether (or this host) brought: none allowed.
    # ESP-IDF's own (a 16-byte segment on the C3) are not Aether's.
    tls="$(cat "$tmp/out/$t.tlsmain" 2>/dev/null || echo missing)"
    if [ "$(head -1 "$log" 2>/dev/null)" = "BUILD FAILED" ]; then
        echo "  [FAIL] esp32 contexts $t: the build failed"
        grep -E 'error:|undefined reference' "$tmp/out/$t.build.log" | head -10 | sed 's/^/        /'
        fail=1; continue
    fi
    if printf '%s\n' "$out" | grep -q '^RESULT actors: the owner.s ask 42$' &&
       printf '%s\n' "$out" | grep -q '^RESULT two tasks: I 20 rounds, 0 wrong; W some rounds, 0 wrong$' &&
       printf '%s\n' "$out" | grep -q '^RESULT churn: 200 attach/detach, 50 ended attached, 0 wrong, heap back' &&
       [ "$tls" = 0 ]; then
        echo "  [PASS] esp32 contexts $t: $(printf '%s\n' "$out" | grep -E 'native task|context:' | sed 's/^RESULT //' | tr '\n' ';') no thread-local section of Aether's; $(printf '%s\n' "$out" | grep churn | sed 's/^RESULT //')"
    else
        echo "  [FAIL] esp32 contexts $t (Aether's thread-local sections: $tls)"
        tr -d '\r' < "$log" 2>/dev/null | grep -E 'RESULT|Guru|abort|aether:|Backtrace' | head -15 | sed 's/^/        |/'
        [ -n "$out" ] || tr -d '\r' < "$log" 2>/dev/null | tail -20 | sed 's/^/        |/'
        fail=1
    fi
done
[ $fail -eq 0 ]
