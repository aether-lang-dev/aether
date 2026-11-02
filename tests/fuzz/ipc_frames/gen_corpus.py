#!/usr/bin/env python3
"""Write the seed corpus for the std.ipc frame-reader fuzz target.

Each seed is: byte 0 = max / 16 (max = 1 + b0 * 16), byte 1 = chunks - 1,
then chunks - 1 cut bytes (fraction of the remaining stream, out of 255),
then the stream. Edit this script, not the files.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "corpus")


def frame(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


def seed(name: str, max_div16: int, cuts: list[int], stream: bytes) -> None:
    head = bytes([max_div16 & 255, (len(cuts)) & 7]) + bytes(c & 255 for c in cuts)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(head + stream)


CBOR_SMALL = bytes.fromhex("a201020304")      # {1: 2, 3: 4}
CBOR_TEXT = bytes.fromhex("63616263")         # "abc"
CBOR_BYTES_NUL = bytes.fromhex("4100")        # h'00'
CBOR_INDEF = bytes.fromhex("9f0102ff")        # [_ 1, 2]
CBOR_BAD = bytes.fromhex("ff")                # a lone break


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        os.remove(os.path.join(OUT, old))

    seed("empty_stream", 16, [], b"")
    seed("one_frame_whole", 16, [], frame(CBOR_SMALL))
    seed("one_frame_empty_payload", 16, [], frame(b""))
    seed("two_frames_whole", 16, [], frame(CBOR_TEXT) + frame(CBOR_INDEF))
    seed("frame_with_nul_and_newline", 16, [], frame(b"a\x00b\nc\xffd\r"))
    seed("split_in_header", 16, [100], frame(CBOR_SMALL))
    seed("split_in_payload", 16, [180], frame(CBOR_SMALL))
    seed("split_at_boundary", 16, [128], frame(CBOR_TEXT) + frame(CBOR_TEXT))
    seed("split_every_byte", 16, [40, 80, 120, 160, 200, 240, 250],
         frame(CBOR_BYTES_NUL))
    seed("header_only", 16, [], struct.pack(">I", 3))
    seed("header_short", 16, [], b"\x00\x00")
    seed("payload_short", 16, [], struct.pack(">I", 8) + b"abc")
    seed("over_max_then_frame", 0, [], frame(b"x" * 2) + frame(b"y"))
    seed("exactly_max", 1, [], frame(b"z" * 17))
    seed("one_over_max", 1, [], frame(b"z" * 18))
    seed("huge_header", 16, [], b"\xff\xff\xff\xff" + b"hello")
    seed("top_bit_header", 16, [], b"\x7f\xff\xff\xff" + b"hello")
    seed("bad_cbor_payload", 16, [], frame(CBOR_BAD))
    seed("many_small_frames", 16, [64, 128, 192],
         b"".join(frame(bytes([i])) for i in range(40)))
    seed("max_frame_4096", 255, [], frame(b"\x40" + b"\x00" * 4000))


if __name__ == "__main__":
    main()
