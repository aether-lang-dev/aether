# The pure TLS client's five entry points are emitted non-static into every TU, so two TUs that both use `std.http.client` cannot be linked

**From:** the selaenium line (2026-09-13) · **Where it bit:** `selaenium`'s
native Aether client — `aeb aether/.tests.ae` fails at the link step.

**Affects:** ae 0.666.0. Hard failure on any modern ld (this is `-fno-common`
territory, not a warning).

## Symptom

```console
$ aeb aether/.tests.ae
/usr/bin/ld: /tmp/ccWCNztD.o: in function `aether_pure_tls_client_available':
..._webdriver_ae_generated.c:(.text+0x341f0): multiple definition of
  `aether_pure_tls_client_available'; /tmp/ccjeCe4J.o:test_live_test.c:(.text+0x2a070):
  first defined here
/usr/bin/ld: ... multiple definition of `aether_pure_tls_client_connect' ...
/usr/bin/ld: ... multiple definition of `aether_pure_tls_client_send' ...
/usr/bin/ld: ... multiple definition of `aether_pure_tls_client_recv' ...
/usr/bin/ld: ... multiple definition of `aether_pure_tls_client_close' ...
collect2: error: ld returned 1 exit status
```

Exactly five symbols, always these five.

## Cause

The build compiles two translation units and links them — a program TU and a
module TU regenerated via `aetherc --emit=lib`. **Both** transitively import
`std.http.client`, which pulls the pure TLS client.

The two TUs share **305** function definitions between them, and 300 of those
link fine. The difference is how they are emitted:

```console
# a runtime function — only DECLARED in the TU, resolved from libaether
$ grep -E '^\w+[ *]+aether_string_data\(.*\) \{' test_live_test.c
(no match — declaration only)

# the pure TLS client — DEFINED, non-static, in the TU
$ grep -nE '^\w+[ *]+aether_pure_tls_client_send\(.*\) \{' test_live_test.c
19674:int aether_pure_tls_client_send(void* pcp, void* buf, int len) {
```

```console
$ grep -c '^static int aether_pure_tls' test_live_test.c ..._generated.c
0
0
```

So these five are inlined into each TU with external linkage. One TU is fine;
two collide.

## Why it is easy to miss

Neither source file in selaenium imports the TLS client directly — it arrives
transitively through `std.http.client`, so nothing in the user's code mentions
it. And a single-TU build of either file succeeds, so it only appears when
something links two.

## Suggested fix

Emit these five `static`, like the other ~300 shared definitions, since they are
already being inlined per-TU. Alternatively give them the same treatment as the
runtime (`aether_string_data` and friends): declare in the TU and define once in
`libaether`. The first is the smaller change and matches what is already done
for everything else that lands in more than one TU.

## Note for the aeb side

The second TU here comes from aeb's auto-regen (`aetherc --emit=lib` on a
module in a `lib(...)` dir) in `aether.program_test`; the node does not declare
`regen(...)`. If the intended model is one TU per program, the aeb behaviour is
worth a look too — but the non-static emission is what makes linking two TUs
impossible in the first place, and that is fixable here.
