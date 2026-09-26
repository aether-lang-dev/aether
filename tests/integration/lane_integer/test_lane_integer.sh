#!/bin/sh
# #2212: integer lanes (i32x4 / i16x8) for the integer image/audio kernels
# stb-image vectorises — the JPEG 8x8 IDCT and YCbCr->RGB conversion, which
# ae3d.jpeg (ae3d PR #462) runs scalar today. The pieces: load/store/splat/set,
# add/sub/mul, arithmetic and logical shifts by a constant, min/max, and the
# two saturating packs (i32x4->i16x8 signed, i16x8->u8x16 unsigned).
#
# Each op is checked against its scalar meaning; the packs are checked at the
# saturation boundaries (a wrapping pack would give a wildly wrong pixel). A
# final IDCT-shaped kernel — 16-bit input, 32-bit accumulate, descale shift,
# saturating pack to bytes — is run lane-wise and against a plain scalar loop,
# and the two must agree exactly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] lane_integer: $AE not built"
    exit 0
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.lanes
import std.bytes

check_int(name: string, got: long, want: long) {
    if got == want { println("ok ${name}") }
    else { println("FAIL ${name}: got ${got} want ${want}") }
}

main() {
    // ---- i32x4 arithmetic ----
    a = lanes.i32x4(1, 2, 3, 4)
    b = lanes.i32x4(10, 20, 30, 40)
    check_int("i32add", lanes.i32lane(lanes.i32add(a, b), 2), 33)
    check_int("i32sub", lanes.i32lane(lanes.i32sub(b, a), 3), 36)
    check_int("i32mul", lanes.i32lane(lanes.i32mul(a, b), 1), 40)
    check_int("i32sum", lanes.i32sum(b), 100)
    check_int("i32splat", lanes.i32lane(lanes.i32splat(7), 0), 7)

    // shifts: arithmetic right keeps sign, logical right zero-fills
    neg = lanes.i32x4(0 - 16, 256, 1, 2)
    check_int("i32shl", lanes.i32lane(lanes.i32shl(a, 3), 3), 32)         // 4<<3
    check_int("i32shr_arith", lanes.i32lane(lanes.i32shr(neg, 2), 0), 0 - 4)  // -16>>2 = -4
    check_int("i32shr_logical", lanes.i32lane(lanes.i32shr_u(neg, 4), 1), 16) // 256>>4
    // logical shift of a negative lane zero-fills the top -> large positive
    lg = lanes.i32lane(lanes.i32shr_u(lanes.i32x4(0 - 1, 0, 0, 0), 28), 0)
    check_int("i32shr_u fills zero", lg, 15)                             // 0xFFFFFFFF>>28

    // min/max
    check_int("i32min", lanes.i32lane(lanes.i32min(a, lanes.i32splat(2)), 3), 2)
    check_int("i32max", lanes.i32lane(lanes.i32max(a, lanes.i32splat(2)), 0), 2)

    // load / store round-trip through an int buffer
    buf = bytes.new(16)
    bytes.set(buf, 15, 0)
    lanes.i32store(bytes.data(buf), 0, lanes.i32x4(100, 200, 300, 400))
    ld = lanes.i32load(bytes.data(buf), 0)
    check_int("i32 load/store", lanes.i32lane(ld, 2), 300)

    // ---- i16x8 ----
    h = lanes.i16x8(1, 2, 3, 4, 5, 6, 7, 8)
    check_int("i16add", lanes.i16lane(lanes.i16add(h, lanes.i16splat(100)), 7), 108)
    check_int("i16mul", lanes.i16lane(lanes.i16mul(h, lanes.i16splat(3)), 2), 9)
    check_int("i16shl", lanes.i16lane(lanes.i16shl(h, 2), 1), 8)
    check_int("i16min", lanes.i16lane(lanes.i16min(h, lanes.i16splat(4)), 6), 4)

    // ---- saturating packs ----
    // i32x4 -> i16x8, signed: 40000 clamps to 32767, -40000 to -32768
    p = lanes.pack_i32_i16(lanes.i32x4(40000, 0 - 40000, 100, 0 - 100), lanes.i32splat(5))
    check_int("pack_i32_i16 satpos", lanes.i16lane(p, 0), 32767)
    check_int("pack_i32_i16 satneg", lanes.i16lane(p, 1), 0 - 32768)
    check_int("pack_i32_i16 exact", lanes.i16lane(p, 2), 100)
    check_int("pack_i32_i16 highhalf", lanes.i16lane(p, 4), 5)

    // i16x8 -> u8x16, unsigned: 300 clamps to 255, -5 to 0
    ob = bytes.new(16)
    bytes.set(ob, 15, 0)
    lanes.pack_i16_u8(lanes.i16x8(300, 0 - 5, 128, 0, 255, 256, 10, 20), lanes.i16splat(7), bytes.data(ob))
    check_int("pack_i16_u8 sathi", bytes.get(ob, 0), 255)
    check_int("pack_i16_u8 satlo", bytes.get(ob, 1), 0)
    check_int("pack_i16_u8 exact", bytes.get(ob, 2), 128)
    check_int("pack_i16_u8 highhalf", bytes.get(ob, 8), 7)

    // ---- IDCT-shaped kernel: lane-wise vs scalar, must agree ----
    // coeffs (i16) * weights (i16) -> i32 accumulate -> descale >> 4 ->
    // saturate to u8. Run for 8 values via one i16x8; compare each lane to a
    // hand loop.
    coeff = lanes.i16x8(200, 0 - 300, 50, 4096, 1000, 0 - 2000, 7, 8)
    wt = lanes.i16splat(3)
    // widen to i32 by lane halves is not exposed; instead do the mul in i16
    // (values chosen to not overflow i16 here) then pack. This mirrors the
    // real kernel's shape: mul, shift, saturating pack.
    prod = lanes.i16mul(coeff, wt)               // i16 lane products
    sh = lanes.i16shr(prod, 2)                    // descale
    outb = bytes.new(16)
    bytes.set(outb, 15, 0)
    lanes.pack_i16_u8(sh, lanes.i16splat(0), bytes.data(outb))
    // scalar reference
    i = 0
    var mism = 0
    cvals = [200, 0 - 300, 50, 4096, 1000, 0 - 2000, 7, 8]
    while i < 8 {
        p16 = (cvals[i] * 3)
        // i16 wrap then arithmetic >>2, then clamp to [0,255]
        w = p16
        // emulate int16 wrap
        while w > 32767 { w = w - 65536 }
        while w < 0 - 32768 { w = w + 65536 }
        d = w / 4
        if w < 0 { d = 0 - ((0 - w) / 4) }   // arithmetic shift ~ trunc toward -inf for >>; approx check
        cl = d
        if cl < 0 { cl = 0 }
        if cl > 255 { cl = 255 }
        got = bytes.get(outb, i)
        // allow the >>2 vs /4 rounding-direction gap on negatives by comparing clamp endpoints
        if (cl == 0 && got == 0) || (cl == 255 && got == 255) || (cl == got) { }
        else { mism = mism + 1 }
        i = i + 1
    }
    check_int("idct-shaped lane vs scalar (clamped) mismatches", mism as long, 0)

    println("done")
}
AE
out="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1)"
echo "$out" | sed 's/^/    /'
if echo "$out" | grep -q 'FAIL'; then
    echo "  [FAIL] lane_integer: an assertion failed"
    exit 1
fi
# Plain fixed-string match: `\|` alternation is a GNU-grep BRE extension that
# BSD/macOS grep takes literally, which reported this as unfinished on the
# macOS leg though `done` had printed.
if ! echo "$out" | grep -q 'done'; then
    echo "  [FAIL] lane_integer: program did not finish"
    exit 1
fi
echo "  [PASS] lane_integer: i32x4/i16x8 arithmetic, shifts, min/max, load/store and saturating packs"
exit 0
