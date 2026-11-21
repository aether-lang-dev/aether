# REPLY: contrib.quickjs: Array.prototype.find frees an element twice when interrupted

**Re:** sae `asks/quickjs-array-find-interrupt.md`

**RESOLVED** on the branch `batch/runtime-quickjs-staticlib-spawntree` (a
release that carries it): `contrib.quickjs` compiles quickjs-ng 0.18.0 with
upstream's fix e22ce4de43 ("Clear Array.find value ownership after release",
#1826). Once sae is on such a release it can drop the JavaScript `find`,
`findIndex`, `findLast` and `findLastIndex` in `src/sae_prelude.js`.

**How it is carried.** No quickjs-ng release has the fix yet (the latest tag
is v0.18.0), and the repo does not pin untagged commits, so the lock still
names the v0.18.0 release archive and its SHA-256. The fix is the file
`contrib/quickjs/patches/0001-array-find-clear-value-after-release.patch`,
upstream's one line rebased onto the amalgamation, with the upstream commit
named in its header. `scripts/fetch-quickjs-amalgamation.sh` applies every
patch in that directory after the checksum check, with `patch` or, where that
is missing, `git apply`, and no fuzz; a patch that does not apply fails the
fetch. Every way the engine reaches a build goes through that script
(`install.sh`, `scripts/stage-release.sh` for the release archives, CI's
contrib check, the tests, and cross builds, which compile the same fetched
file), so all of them get the fix. The script's `.sha256` record now names the
patches too, so an existing checkout's amalgamation is fetched and patched
again on the next run rather than kept as it was. Drop the patch when the lock
moves to a release containing e22ce4de43.

**The test** is the ask's repro, as a case in `contrib/quickjs/test_quickjs.ae`
("a find the deadline stopped between two elements leaves its array whole"),
for `find` and `findLast`. One detail of the repro mattered: a `find` step
polls the interrupt twice, at its own top and at the callback's entry, and
every count between two clock reads is even, so whether the deadline lands on
find's own poll is fixed by the parity of the polls before it, which the
array's length decides. On macOS arm64 every even length tried (50,000 up to
210,000, 123,456 among them) failed and every odd one (199,999, 200,001,
200,003, 50,001) passed. The case runs both an even and an odd length, so one
of them lands on find's own poll wherever the count starts.

**The handle-check switch** proposed at the end of the ask was not added.
The find fault was the engine's; a stale-handle check is a debugging aid that
belongs with the debugging, and nothing here needed it.
