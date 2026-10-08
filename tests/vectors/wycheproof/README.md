# Project Wycheproof test vectors (pinned subset)

Byte-identical vector files from **Project Wycheproof**
(<https://github.com/C2SP/wycheproof>, Apache-2.0 — full licence text in
`THIRD_PARTY_LICENSES.md`), `testvectors_v1/`, pinned at upstream commit
`3fa63dd0344abb611f1fb1d77e119938603ea230`. (Every file already present at
the previous pin `5722833ca004983abd1a91bcb6c24596d50ac0f9` was verified
byte-identical at this commit when the pin was advanced for wave 5.) The
ML-KEM encapsulation and semi-expanded decapsulation files
(`mlkem_*_encaps_test.json`, `mlkem_*_semi_expanded_decaps_test.json`) were
added later from the same commit, for the FIPS 203 input checks (#2482).

Wycheproof is the adversarial complement to the RFC/NIST known-answer
tests the crypto suites already carry: each file probes a primitive with
the edge cases behind real-world attacks (small-order points, twists,
non-canonical encodings, forged/truncated tags, malleable signatures, …).
Drivers live in `tests/integration/wycheproof/` — one per family,
encoding the valid / invalid / acceptable semantics.

To re-vendor or add a family: copy the file verbatim from a fresh clone's
`testvectors_v1/`, update the pinned commit here, and add a driver.
Never edit vector files.
