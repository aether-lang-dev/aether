# Security policy

## Reporting a vulnerability

Report it privately through GitHub: on the repository's **Security** tab,
choose **Report a vulnerability**. Please do not open a public issue, pull
request or discussion for a problem that someone could exploit before a fix
ships.

A useful report says:

- what is affected: the compiler, the runtime, a std module, `ae`, the
  installers or a release artifact, and the version (`ae version`);
- the platform;
- how to reproduce it, ideally with the smallest program or input that does;
- what an attacker gains (code execution, a sandbox escape, reading or
  writing outside a grant, a crash or unbounded memory from a peer's input).

The maintainers confirm the problem, fix it on `main`, ship it in the next
release, and publish a GitHub security advisory that credits the reporter
unless they ask not to be named.

## Supported versions

Aether releases often, and fixes land in the next release. Only the latest
release is supported: upgrade with `ae upgrade` or the installer.

## What counts

In scope, among others:

- memory safety bugs in generated C, the runtime or std;
- escapes from the sandbox (`docs/containment-sandbox.md` says what each mode
  holds and what it does not);
- parsers that read untrusted input (HTTP/1, HTTP/2, WebSocket, TLS, X.509,
  JSON, CBOR, XML, YAML, msgpack, tar, zip, URLs) crashing, looping or
  growing without bound on a peer's bytes;
- installers or `ae upgrade` accepting a download they should refuse;
- CI or release workflows that could run untrusted code with write access.

A program that is unsafe by its own choice (`@c_include` code, raw pointers,
an explicit `grant_*` that covers what it then reads) is not a vulnerability
in Aether.

## How the project protects itself

- Every GitHub Action is pinned to a full commit SHA. The repository requires
  it, and `tests/scripts/check_action_pins.sh` checks it in
  `make check-tests`. Dependabot keeps the pins current.
- Workflows declare least-privilege `permissions:` and cannot approve pull
  requests.
- Secret scanning with push protection, and Dependabot security updates, are
  on.
- The decoders that read untrusted input are fuzzed under libFuzzer, ASan and
  UBSan on the PRs that change them, and on a schedule (`tests/fuzz`).
- Memory checks run on every PR: ASan, LSan, UBSan and Valgrind on Linux, and
  the leaks gate on macOS.
