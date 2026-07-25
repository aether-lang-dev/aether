# `ae` cross-builds ship without a working `std.cryptography` (HMAC) even when `CROSSBUILD_SYSROOT` supplies libcrypto.a/libssl.a

**From:** the aeo line (2026-07-25) · **Where it bit:** a fresh arm64 fleet
node (Raspberry Pi 5, Debian 13), running an `ae`-cross-built `aeo-agent`
(`--target=aarch64-linux`). Surfaced as a **security** symptom downstream (see
"Why this is urgent").

## Symptom

An `aeo-agent` cross-compiled for `aarch64-linux` has a non-functional
`hmac_sha256_hex`: at runtime the call returns its error path (empty hex), i.e.
`std.cryptography`'s HMAC is the unavailable/stub build, not a working one. The
same source built **native x86_64** (the release `AE_CC="gcc -static"` path)
links a working HMAC — so it's specific to the **zig cross path**, and it spans
the whole cross matrix (freebsd / windows / aarch64), not just arm64.

Concretely, on this dev box (`ae 0.442.0`, zig 0.13.0):

    # cross-build for aarch64-linux WITH the crossbuild sysroot that HAS the libs
    export CROSSBUILD_SYSROOT=.../aether-crossbuild/sysroots/aarch64-linux-gnu
    ls $CROSSBUILD_SYSROOT/lib/            # -> libcrypto.a  libssl.a   (both present)
    ae build bin/aeo-agent.ae -o agent-arm --target=aarch64-linux
    # build still prints:
    #   Note: '...' uses std.http. Cross binaries are built without OpenSSL / zlib / ...
    strings agent-arm | grep -i 'openssl unavailable'   # -> present (the stub path)

Setting `CROSSBUILD_SYSROOT` to a sysroot that genuinely contains `libcrypto.a`
+ `libssl.a` did **not** change the outcome — the "built without OpenSSL" note
still fires and the stub strings remain. So either the Tier-2 probe in
`tools/ae.c` (the `crossbuild_libs` block that appends `-lssl -lcrypto` when
`$CROSSBUILD_SYSROOT/lib/libssl.a` exists) is not being reached for this
target/invocation, or the target-agnostic "cross => no OpenSSL" note/policy
short-circuits it before the probe. (I did not finish tracing which; the A/B is
conclusive that the sysroot's presence alone doesn't wire crypto for
`aarch64-linux`.)

Note this is specifically about **`std.cryptography` (HMAC)** — a target-agnostic
Tier-2 lib. It is a peer of the openssl/nghttp2/zlib/pcre2 story the
`CROSSBUILD_SYSROOT` probe already handles for real; HMAC just needs the same
wiring to actually fire on the cross path.

## Why this is urgent (the downstream that caught it)

aeo's agent channel is HMAC-authed (`lib/agent_auth`: constant-time HMAC of a
fixed challenge under a per-agent secret). With HMAC returning empty, the two
MACs compared were both `""` and matched — every token authenticated. A wrong
**and** an empty token both returned `report <node> up` (HTTP 200) over the live
`/dispatch` channel on the cross-built agent. The native x86_64 asset (working
HMAC) correctly returned 401.

aeo has since made its own verifier **fail closed** when the MAC can't be
computed (a crypto-blind agent is now inert, not wide-open — the correct
posture). So this is no longer a security hole for aeo. **But** it means every
cross-built agent is now *inert* (authenticates nobody) until this is fixed —
the freebsd/windows/arm64 release assets can't run the authed channel at all.
The fail-open was the acute problem; the fail-closed-but-unusable state is the
standing one this ask resolves.

## Asks

1. **Make the `CROSSBUILD_SYSROOT` Tier-2 probe actually fire for
   `std.cryptography` on the cross path.** When the sysroot supplies
   `libssl.a`/`libcrypto.a`, the cross binary should link a **working**
   `hmac_sha256_hex` (HMAC needs only libcrypto), independent of whether TLS/http
   is wanted. Today the "cross => built without OpenSSL" note appears to win even
   with the libs staged.

2. **Separate HMAC availability from TLS availability in the note/policy.** The
   "built without OpenSSL" message conflates the std.http TLS path with the
   std.cryptography HMAC path. A cross build can (and for auth, must) have working
   HMAC even if it deliberately has no TLS client. At minimum, don't let the
   TLS-oriented note suppress HMAC linking.

3. **Fail the build loud, not the runtime silent (or at least warn precisely).**
   A program that imports `std.cryptography` and gets a **stub** `hmac_sha256_hex`
   is a footgun: the call returns empty at runtime with no build-time signal that
   the primitive is inert. Either (a) hard-error when a cross target imports
   std.cryptography but no crypto lib is available, or (b) emit a precise
   per-feature warning ("std.cryptography.hmac will be UNAVAILABLE in this cross
   build — no libcrypto") rather than the generic http note. A silent stub that
   returns empty is exactly what turned into a fail-open auth bug downstream.

## Cross-refs

Same family as the `g_link_reqs` native-link-declaration discussion
(aether#1259) — both are "cross/link plumbing doesn't carry a module's native
crypto dep to where it's needed." This one is narrower and has a concrete
security consequence, so worth fixing on its own even if #1259's broader
mechanism lands later.
