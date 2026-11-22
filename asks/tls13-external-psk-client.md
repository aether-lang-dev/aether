# Release a TLS 1.3 external-PSK client API

2026-11-22. Confirmed missing from the installed Aether 0.935.0 release
(commit 9294afefe748123b542181d793a9055841446113).

## What is blocked

The esp32 platform's native Aether UI phone controller connects to an ESP32-C3
smart plug using TLS1.3 external PSK with ephemeral key exchange. There is no
certificate authority or browser certificate path. Device credentials stay in
the native host's local-only Keychain; Rexx guests receive no key/network access.

The controller works with a local Aether patch, but cannot move to an unmodified
0.935.0 SDK: tls13_client exports resumption connections, not connect_psk or
connect_socket_psk. External PSK is not a resumption ticket. The controller must
not substitute one, disable authentication or downgrade to TLS1.2.

## Reproduction

From /Users/paul/scm/esp32, using the complete installed release:

```sh
AETHER_HOME=/Users/paul/.aether/versions/v0.935.0 \
  /Users/paul/.aether/versions/v0.935.0/bin/ae build \
  clients/native/ios/transport.ae --emit=csrc --with=fs,net,os,extern \
  -o /tmp/ControllerTLS
```

Observed:

```text
error[E0303]: 'connect_socket_psk' is not exported from module 'tls13_client'
```

The call is in clients/native/ios/tlsimpl/module.ae. No device or real PSK is
needed to reproduce the compile failure. The installed ae and aetherc must both
be0.935.0; this Mac's unrelated /usr/local/bin/ae is still0.798.0.

## Requested API and contract

Expose supported external-PSK connections through std.cryptography.tls13_client:

```text
connect_psk(host, port, identity: byte[], key: byte[])
connect_socket_psk(sock: @keep ptr, identity: byte[], key: byte[])
```

Return the existing client connection/error shape, with explicit socket ownership
on all success/failure paths. A first bounded implementation can require a
32-byte SHA256 PSK and a nonempty identity of at most64 bytes.

- Use the external "ext binder" label; retain "res binder" for resumption.
- Require PSK-DHE, authenticate the peer, and refuse certificate-only fallback,
  wrong keys/identities and TLS1.2. No external 0RTT is required.
- Obtain fresh ephemeral entropy internally and fail closed on entropy failure.
- Handle the supported key-exchange retry path and wipe owned key material.
- Specify whether caller buffers are borrowed/copied and how cancellation works.

## Existing implementation and evidence

Local implementation: commit dd7e3aa06bd8ace94675a8888e11371d120032f9 on
codex/esp32-external-psk in this repository. Its base was Aether 0.917.0; it has
not been pushed or released. A carry-forward worktree based on 0.935.0 is at
/Users/paul/Documents/Codex/2026-11-04/new-chat/work/aether-external-psk-v0935.
That carry-forward is not yet validated and is not evidence of release support.

The original implementation passed independent Python3.13/OpenSSL PSK peers,
X25519 and P256 retry, host/socket variants, invalid input and wrong key/identity,
certificate-only/TLS1.2 refusal, and existing resumption/0RTT regressions. Replacing
its external binder label with the resumption label made the positive test fail.
It also connected from the native Mac and physical iPhone to the unchanged S60.
Those are 0.917-based results, not 0.935 results.

Relevant upstream tests in the local commit:
 tests/integration/crypto_tls13_external_psk/.
Downstream protocol tests and native adapters:
 /Users/paul/scm/esp32/clients/native/ios/.

## Acceptance

Release the API with independent peer tests, explicit ownership/secret handling,
negative authentication tests and unchanged resumption behaviour. Then the
controller can pin a released SDK instead of maintaining a private TLS branch.
