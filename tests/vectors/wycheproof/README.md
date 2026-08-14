# Project Wycheproof test vectors (pinned subset)

Byte-identical vector files from **Project Wycheproof**
(<https://github.com/C2SP/wycheproof>, Apache-2.0 — full licence text in
`THIRD_PARTY_LICENSES.md`), `testvectors_v1/`, pinned at upstream commit
`5722833ca004983abd1a91bcb6c24596d50ac0f9`.

Wycheproof is the adversarial complement to the RFC/NIST known-answer
tests the crypto suites already carry: each file probes a primitive with
the edge cases behind real-world attacks (small-order points, twists,
non-canonical encodings, forged/truncated tags, malleable signatures, …).
Drivers live in `tests/integration/wycheproof/` — one per family,
encoding the valid / invalid / acceptable semantics.

To re-vendor or add a family: copy the file verbatim from a fresh clone's
`testvectors_v1/`, update the pinned commit here, and add a driver.
Never edit vector files.
