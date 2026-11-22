#!/bin/bash
# Runs inside espressif/idf: builds tests/esp32/contexts for each target on
# the command line, boots it in Espressif's QEMU, and writes the console to
# /out/<target>.log and the image's program headers to /out/<target>.phdr.
set -u
. "$IDF_PATH/export.sh" > /dev/null 2>&1
for t in "$@"; do
    rm -rf /tmp/p && cp -r /esp32/contexts /tmp/p && cd /tmp/p
    if ! idf.py set-target "$t" > "/out/$t.build.log" 2>&1 || ! idf.py build >> "/out/$t.build.log" 2>&1; then
        echo "BUILD FAILED" > "/out/$t.log"
        continue
    fi
    case "$t" in
        esp32c3) arch=riscv32; ro=riscv32-esp-elf-readelf ;;
        *) arch=xtensa; ro=xtensa-$t-elf-readelf ;;
    esac
    "$ro" -lW build/aether_esp32_contexts.elf > "/out/$t.phdr" 2>&1
    # The thread-local symbols and the objects that define them.
    "$ro" -sW build/aether_esp32_contexts.elf | awk '$4 == "TLS" { print $8, $3 }' > "/out/$t.tls" 2>&1
    # Thread-local input sections from the main component (Aether and the
    # host): there must be none, so no task's stack carries Aether state.
    grep -A1 -E '^ \.t(bss|data)' build/aether_esp32_contexts.map > "/out/$t.tlsmap" 2>/dev/null
    grep -c 'libmain\.a' "/out/$t.tlsmap" > "/out/$t.tlsmain"
    Q=$(ls -d "$IDF_TOOLS_PATH"/tools/qemu-$arch/*/qemu/bin | head -1)
    (cd build && esptool.py --chip "$t" merge_bin --fill-flash-size 4MB -o flash.bin @flash_args > /dev/null 2>&1)
    # kill-ok: runs only inside the Linux espressif/idf container
    timeout 240 "$Q/qemu-system-$arch" -nographic -machine "$t" -m 4M \
        -drive file=build/flash.bin,if=mtd,format=raw -nic none 2>&1 |
        awk '{ print; fflush() }
             /Returned from app_main|Guru Meditation|abort\(\) was called|RESULT churn/ { n++ }
             n && /Returned from app_main|Guru Meditation|abort\(\) was called/ { exit }' \
        > "/out/$t.log"
    cd /
done
