#!/usr/bin/env python3
"""Print a wasm module's exported names, one per line.

The export SECTION is the authority. Grepping the binary for a symbol name
proves nothing: the name can appear in the linking/name sections while the
function is not exported at all, which is exactly the false pass this test
exists to avoid.
"""
import sys

d = open(sys.argv[1], "rb").read()
if d[:4] != b"\x00asm":
    sys.exit("not a wasm module")

def uleb(p):
    r = s = 0
    while True:
        b = d[p]; p += 1
        r |= (b & 0x7F) << s
        if not b & 0x80:
            return r, p
        s += 7

p = 8
while p < len(d):
    sid = d[p]; p += 1
    size, p = uleb(p)
    end = p + size
    if sid == 7:                      # export section
        n, p = uleb(p)
        for _ in range(n):
            ln, p = uleb(p)
            print(d[p:p + ln].decode()); p += ln
            p += 1                    # kind
            _, p = uleb(p)            # index
        break
    p = end
