#!/bin/sh
# Aether on the ESP32 (Xtensa LX6, ~320 KB DRAM), under emulation.
#
# Generates C for a few programs with this tree's aetherc, builds each into
# ESP32 firmware with ESP-IDF's xtensa-esp32-elf-gcc, boots it in Espressif's
# QEMU and checks what it prints on the serial console. Everything ESP-side
# runs in the official espressif/idf image (it carries the toolchain and
# qemu-xtensa), with podman or docker, whichever is installed.
#
#   sh tests/esp32/test_esp32.sh          (or: make ci-esp32)
#
# Skips when neither container engine is present. ESP32_IDF_IMAGE overrides
# the image, ESP32_ENGINE the engine.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
IMAGE="${ESP32_IDF_IMAGE:-docker.io/espressif/idf:v6.1}"

ENGINE="${ESP32_ENGINE:-}"
[ -n "$ENGINE" ] || for e in podman docker; do
    if command -v "$e" > /dev/null 2>&1; then ENGINE="$e"; break; fi
done
[ -n "$ENGINE" ] || { echo "  [SKIP] esp32: neither podman nor docker is installed"; exit 0; }
[ -x "$AETHERC" ] || { echo "  [FAIL] esp32: $AETHERC not built"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
mkdir -p "$tmp/gen" "$tmp/out"
chmod 777 "$tmp/out"

pass=0
fail=0
ok() { echo "  [PASS] esp32: $1"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] esp32: $1"; fail=$((fail + 1)); }

# program  source  line it must print
PROGRAMS="hello:examples/basics/hello.ae:Hello, Aether!
counter:examples/actors/counter.ae:Current value: 18
test_coop_chain:tests/syntax/test_coop_chain.ae:Chain test passed!
test_platform_caps:tests/syntax/test_platform_caps.ae:All platform caps tests passed!"

names=""
for row in $(printf '%s\n' "$PROGRAMS" | cut -d: -f1,2); do
    name="${row%%:*}"
    src="${row#*:}"
    if "$AETHERC" "$ROOT/$src" "$tmp/gen/$name.c" > "$tmp/$name.gen.log" 2>&1; then
        names="$names $name"
    else
        bad "$name: aetherc failed"
        sed 's/^/        /' "$tmp/$name.gen.log" | head -10
    fi
done

# The repo is mounted read-only; the image's own user writes only /out.
"$ENGINE" run --rm \
    -v "$ROOT:/aether:ro" -v "$SCRIPT_DIR:/esp32:ro" \
    -v "$tmp/gen:/gen:ro" -v "$tmp/out:/out" \
    "$IMAGE" bash /esp32/in_container.sh $names > "$tmp/engine.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    bad "the $ENGINE run of $IMAGE failed ($rc)"
    tail -20 "$tmp/engine.log" | sed 's/^/        /'
fi

printf '%s\n' "$PROGRAMS" | while IFS= read -r line; do
    name="${line%%:*}"
    want="${line#*:*:}"
    echo "$name|$want"
done > "$tmp/expect"
while IFS='|' read -r name want; do
    case " $names " in *" $name "*) ;; *) continue ;; esac
    log="$tmp/out/$name.log"
    # The console, from app_main on: CRs stripped (QEMU's serial is CRLF).
    out="$(tr -d '\r' < "$log" 2>/dev/null | sed -n '/Calling app_main/,$p')"
    if [ "$(head -1 "$log" 2>/dev/null)" = "BUILD FAILED" ]; then
        bad "$name: the ESP-IDF build failed"
        grep -E 'error:|undefined reference|overflow|does not fit' "$tmp/out/$name.build.log" |
            head -10 | sed 's/^/        /'
    elif printf '%s\n' "$out" | grep -qF "$want" &&
         printf '%s\n' "$out" | grep -q 'Returned from app_main'; then
        size="$(grep -o 'aether_esp32.bin binary size 0x[0-9a-f]*' "$tmp/out/$name.build.log" | awk '{print $4}')"
        ok "$name runs on the emulated ESP32 ($(printf '%d' "$size" 2>/dev/null) bytes of firmware): $want"
    else
        bad "$name did not print \"$want\" and return from app_main"
        printf '%s\n' "$out" | tail -15 | sed 's/^/        |/'
    fi
done < "$tmp/expect"

echo ""
echo "esp32: $pass passed, $fail failed"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
