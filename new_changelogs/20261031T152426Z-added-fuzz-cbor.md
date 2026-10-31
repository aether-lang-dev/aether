- **A coverage-guided fuzz harness for `std.cbor`'s decoder, the repo's
  first.** `make fuzz-cbor` builds `tests/fuzz/cbor/` with libFuzzer, ASan and
  UBSan and runs it for `FUZZ_SECONDS` (default 60). Besides crashes and leaks
  it checks that a parsed value re-encodes deterministically to bytes that
  parse and re-encode the same, and that whatever stricter decode options
  accept, the defaults accept as the same value. The seeds (RFC 8949 Appendix
  A, the hardening tests' inputs, deep nesting, duplicate keys, length bombs)
  are written from hex by `gen_corpus.py`. `prove_mutants.sh` shows the fuzzer
  finds a removed depth check and a removed length check. Not part of `make
  test` or CI.
