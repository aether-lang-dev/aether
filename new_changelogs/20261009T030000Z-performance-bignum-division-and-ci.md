- **`std.bignum` divides with Knuth's Algorithm D.** `divide`, `remainder`
  and `mod` worked a bit at a time and allocated a few bignums per quotient
  bit, so every reduction of a product went through thousands of
  allocations. They now take one 32-bit limb of the quotient per step, with
  no allocation in the loop. A P-521 signature check went from about 4.6 s
  to about 60 ms, an Ed448 check from about 2.5 s to about 25 ms (#2595).
- **P-256 and P-384 reduce with the generic `bignum.mod`.** Their
  special-form folds were written to avoid the old division and are now the
  slower path: over the Wycheproof P1363 vectors, P-256 checks take 3.8 s
  instead of 21.3 s and P-384 8.5 s instead of 17.0 s. The folds are gone.
- **The Wycheproof drivers check more vectors.** P-521 samples every 10th
  vector instead of every 40th, as P-256 and P-384 do, and Ed448 checks all
  87 instead of every 4th. Every vector of every curve passes.
- **The documentation's code blocks are built in parallel.**
  `tests/scripts/check_doc_blocks.py` built and ran each block in turn,
  4.5 minutes of every Windows CI job; it now runs `NPROC` at a time, each
  in a directory of its own, and still reports in source order (#2594).
