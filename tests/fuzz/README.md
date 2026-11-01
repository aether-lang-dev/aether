# Fuzzing Aether

This directory contains coverage-guided fuzz harnesses for security-critical
components of the Aether codebase.

## CBOR Fuzzer (`tests/fuzz/cbor/`)

`std.cbor` decodes messages sent between sandboxed children and their parents,
making it a primary attack surface. The fuzz harness tests the pure Aether
implementation (`std/cbor/module.ae`) using libFuzzer and sanitizers (ASan, UBSan).

### Files

- **`fuzz_cbor.ae`**: the target, built `--emit=obj`, exporting
  `fuzz_target(data: ptr, len: int)`. It takes the input whole, zeros
  included (`bytes.string_from_ptr`), and runs the oracle: parse with the
  defaults; round-trip a parsed value through `encode_deterministic` (it must
  re-parse and re-encode to the same bytes); parse again with small limits
  taken from the first byte, where anything those stricter options accept the
  defaults must accept, as the same value. Oracle failures `abort()`; an
  ordinary parse error is a pass.
- **`fuzz_cbor.c`**: `LLVMFuzzerTestOneInput`, which calls `aether_fuzz_target`.
- **`gen_corpus.py`** writes **`corpus/`** from hex, so the seeds can be read
  in review: RFC 8949 Appendix A and some of Appendix F (malformed), the byte
  inputs of `std/cbor/test_cbor_hardening.ae`, an empty input, a truncated
  head, a byte string holding 0x00, arrays nested 255 and 257 deep, duplicate
  keys, indefinite lengths and length bombs. Edit the script, not the files.
- **`cbor.dict`**: the head bytes of every major type and length width, the
  break byte, the simple and float heads, and tags 2, 3, 24 and 55799.
- **`run_fuzz_cbor.sh`**: builds and runs the harness under libFuzzer, ASan
  and UBSan for `FUZZ_SECONDS` (default 60). It first checks that `41 00`
  reaches the decoder as 2 bytes. It fails, saying why, when clang has no
  libFuzzer; it never skips.
- **`prove_mutants.sh`**: plants two bugs in a copy of `std/cbor/module.ae`
  (a local `std/cbor/` shadows the stdlib's) and shows the fuzzer finds each:
  - **M1**: the nesting depth check removed (a stack overflow);
  - **M2**: the check that a string's declared length fits in the bytes left
    removed (an oversized allocation or an out-of-bounds read).

### Running the fuzzer

```sh
make fuzz-cbor                                   # 60 seconds
FUZZ_SECONDS=1800 sh tests/fuzz/cbor/run_fuzz_cbor.sh
```

It is not part of `make test` or CI. The binary, the working corpus (which
grows from run to run) and any reproducer stay in `build/fuzz/cbor/`;
`FUZZ_OUT` moves them.

### Reproducing and minimising a crash

A crash, leak, timeout or oracle failure writes `crash-<sha1>` (or `leak-`,
`timeout-`, `oom-`) into `build/fuzz/cbor/`, and the script names it.

```sh
build/fuzz/cbor/fuzz_cbor build/fuzz/cbor/crash-<sha1>
build/fuzz/cbor/fuzz_cbor -minimize_crash=1 -runs=100000 \
    -exact_artifact_path=min.cbor build/fuzz/cbor/crash-<sha1>
```

Add the minimised input to `gen_corpus.py`, and a regression case to
`std/cbor/test_cbor_hardening.ae`.

### AFL++ (optional, untested here)

`afl-clang-fast -fsanitize=fuzzer` links AFL++'s own driver for an
`LLVMFuzzerTestOneInput` harness:

```sh
AE_CC="afl-clang-fast -g -O1" build/ae build tests/fuzz/cbor/fuzz_cbor.ae \
    --with=extern --emit=obj -o fuzz_cbor_ae.o
afl-clang-fast -fsanitize=fuzzer,address,undefined -g -O1 \
    tests/fuzz/cbor/fuzz_cbor.c fuzz_cbor_ae.o $(build/ae cflags) -o fuzz_cbor_afl
afl-fuzz -i tests/fuzz/cbor/corpus -o afl_out -x tests/fuzz/cbor/cbor.dict -- ./fuzz_cbor_afl
```
