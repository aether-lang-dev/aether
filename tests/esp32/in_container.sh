#!/bin/bash
# Runs inside espressif/idf. For each program named on the command line:
# build /gen/<name>.c into firmware, boot it in Espressif's QEMU (ESP32
# machine, 4 MB flash) and write the serial console to /out/<name>.log.
# QEMU stops when app_main returns, a panic is printed, or after 60 s.
set -u
. "$IDF_PATH/export.sh" > /dev/null 2>&1
Q=$(ls -d "$IDF_TOOLS_PATH"/tools/qemu-xtensa/*/qemu/bin | head -1)
cp -r /esp32/project /tmp/project
cd /tmp/project
for p in "$@"; do
    if ! idf.py -DAE_PROG="$p" build > "/out/$p.build.log" 2>&1; then
        echo "BUILD FAILED" > "/out/$p.log"
        continue
    fi
    (cd build && esptool.py --chip esp32 merge_bin --fill-flash-size 4MB \
        -o flash.bin @flash_args > /dev/null 2>&1)
    timeout 60 "$Q/qemu-system-xtensa" -nographic -machine esp32 -m 4M \
        -drive file=build/flash.bin,if=mtd,format=raw -nic none 2>&1 |
        awk '{ print; fflush() }
             /Returned from app_main|Guru Meditation|abort\(\) was called/ { exit }' \
        > "/out/$p.log"
done
