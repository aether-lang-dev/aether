# Writing security-sensitive code in Aether

Rules for code where getting it wrong is a vulnerability rather than a bug:
crypto, TLS, authentication, anything comparing a secret or consuming
randomness.

Every rule here is written from a real defect found in this tree, and each one
is the kind that **passes its tests**. That is the point: a handshake that
completes, a hash that verifies, a benchmark that speeds up are all compatible
with a serious flaw. Correctness testing does not catch these; the rule has to
be applied deliberately.

## Never substitute a default for missing entropy

If the CSPRNG fails, **fail**. Do not return zeros, a counter, a timestamp, or
a cached value.

```aether,fragment
// WRONG -- a helper that looks defensive and is catastrophic
fn rand_into(n: int) -> ptr {
    s, sn, err = cryptography.random_bytes(n)
    b = bytes.new(n)
    if string.equals(err, "") == 1 {
        bytes.copy_from_string(b, 0, s, n)
    }
    return b            // on failure: n zero bytes, silently
}
```

That shipped in a TLS server draft, and its three callers used the result as
the server random, the **ECDSA P-256 signing nonce**, and the **X25519 private
key**. A predictable ECDSA nonce discloses the signing key outright — one
signature is enough. The handshake completes; the test passes; the key is gone.

```aether,fragment
// RIGHT -- null, and every caller refuses
fn rand_into(n: int) -> ptr {
    s, sn, err = cryptography.random_bytes(n)
    if string.equals(err, "") != 1 { return null }
    if sn < n { return null }
    ...
}
```

The general form: **a security primitive has no safe default.** Where ordinary
code degrades gracefully, this code stops. Check the length returned as well as
the error — a short read is a failure.

## Compare secrets in constant time

Any comparison of a MAC, tag, token or password hash must take the same time
regardless of where the first difference is. Accumulate differences; never
return early.

```aether,fragment
// RIGHT
fn bytes_equal(a: ptr, b: ptr, n: int) -> int {
    diff = 0
    i = 0
    while i < n {
        diff = diff | ((bytes.get(a, i) ^ bytes.get(b, i)) & 0xFF)
        i = i + 1
    }
    if diff == 0 { return 1 }
    return 0
}
```

A `while` loop that `return 0`s on the first mismatch leaks the length of the
matching prefix, which is enough to recover a tag byte by byte over enough
attempts. This applies to `string.equals` on a secret too — it is a plain
comparison and is fine for a route name, wrong for a token.

## Fail closed, and name what failed

Every verification path ends in a refusal, not a fallthrough. The error should
say which check failed, because the alternative is a dropped connection the
peer cannot diagnose.

```aether,fragment
if fin_ok == 0 {
    ... free everything ...
    return null, "client Finished MAC verification failed"
}
```

Two habits that follow:

- **Free on the failure path too.** A refusal that leaks is still a refusal,
  but a long-lived server refusing repeatedly is a memory-exhaustion vector.
- **State the constraints you cannot meet.** A TLS server that only signs with
  ECDSA P-256 must say so, or an RSA certificate produces a plausible-looking
  server that fails at the signature step and the peer sees only a closed
  socket. Put it in the module header, not just the commit message.

## A library must not print

No `println` in `std` or `contrib` module code. Errors are already returned
through the `(value, err)` convention; printing them as well means a handshake
path writes a line **per connection** in production, and the operator cannot
turn it off.

Nine debug `println` calls reached review in one TLS server draft. They were
useful while writing it, and every one of them was a leak of internal state to
stdout at scale.

If you need progress output while developing, delete it before review, or gate
it behind an explicit debug flag the caller opts into.

## Do not invent crypto, and check what already exists

`std.cryptography` carries the primitives (AES, ChaCha20-Poly1305, P-256/384/521,
Ed25519, X25519, RSA, SHA-2/3, HKDF, ML-KEM, and the TLS 1.3 record, key
schedule, handshake and certificate layers). Before writing a construction,
check whether the piece exists — and whether the piece you need is the *inverse*
of something already present. The TLS server work was one layer, not a stack,
because the record layer and key schedule were already role-neutral.

Where you must write something new:

- **Test against a third party, not only against yourself.** Two halves of one
  codebase agreeing proves they share a wire format; it does not prove the
  format is right. `tests/integration/crypto_tls13_server_hs` parses a real
  ClientHello captured from `openssl s_client`, and the TLS vertical-slice test
  ends with `curl` against the pure server, for exactly this reason.
- **Use known-answer vectors where they exist.** RFC 8448 for TLS 1.3, RFC 7748
  for X25519, the Wycheproof corpus for the rest.
- **Test the negative cases.** A verifier that has never been shown a bad input
  is untested. Bad MAC, wrong host, expired certificate, malformed message,
  truncated input.

## Nonces, IVs and counters

- A nonce is **per message**, never reused with the same key. For ECDSA that is
  a hard requirement (nonce reuse across two signatures recovers the key); for
  AEAD, reuse breaks confidentiality and authenticity both.
- Do not derive a nonce from something the peer controls.
- Where a counter is the nonce, the overflow case is a failure, not a wrap.

## Review checklist

For any diff touching crypto, TLS or authentication:

- [ ] Every randomness call checks both the error **and** the length
- [ ] No security-relevant default on a failure path (no zeros, no reuse)
- [ ] MAC/tag/token comparisons are constant-time
- [ ] Every verification failure returns an error naming the check
- [ ] Failure paths free what they allocated
- [ ] No `println` in library code
- [ ] Tested against a third-party implementation, not only against ourselves
- [ ] Negative cases tested: bad MAC, wrong identity, malformed input
- [ ] Constraints that fail as a dropped connection are documented in the
      module header
