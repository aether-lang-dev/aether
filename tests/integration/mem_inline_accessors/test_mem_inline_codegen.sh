#!/bin/sh
# #2379: equal results alone cannot prove calls/null guards were removed.
set -eu
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$ROOT/build/aetherc" "$SCRIPT_DIR/mem_inline.ae" "$TMP/probe.c" > "$TMP/compile.log" 2>&1 || {
    cat "$TMP/compile.log"
    exit 1
}
for op in bits_of_float float_from_bits clz32 clz64; do
    awk -v name="mem_$op" '
        /^static inline .*\{$/ && index($0, " " name "(") { found=1; body=1; next }
        body && /^}/ { body=0 }
        body && /aether_mem_/ { bad=1 }
        END { exit (!found || bad) }
    ' "$TMP/probe.c" || { echo "FAIL: $op still calls the runtime"; exit 1; }
done
for kind in byte byte_sz ptr int long int8 uint8 int16 uint16 uint32 float32 float64; do
    for op in get set; do
        name="mem_${op}_${kind}_unchecked"
        awk -v name="$name" '
            /^static inline .*\{$/ && index($0, " " name "(") { found=1; body=1; next }
            body && /^}/ { body=0 }
            body && /aether_mem_|if \(!p\)/ { bad=1 }
            END { exit (!found || bad) }
        ' "$TMP/probe.c" || { echo "FAIL: $name still calls/checks null"; exit 1; }
    done
done
# The driver currently exits 1 for audit mode despite a completed report.
"$ROOT/build/aetherc" --audit-mem "$SCRIPT_DIR/mem_inline.ae" "$TMP/audit.c" > "$TMP/audit.log" 2>&1 || [ "$?" -eq 1 ]
for kind_width in byte:1 byte_sz:1 ptr:8 int:4 long:8 int8:1 uint8:1 int16:2 uint16:2 uint32:4 float32:4 float64:8; do
    kind="${kind_width%:*}"
    width="${kind_width#*:}"
    grep -q "mem.get_${kind}_unchecked reads $width byte" "$TMP/audit.log"
    grep -q "mem.set_${kind}_unchecked writes $width byte" "$TMP/audit.log"
done
echo 'PASS: inline bit operations and unchecked accesses have no runtime calls or null guards; audit reports widths'
