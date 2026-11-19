# Fuzzing Aether

This directory contains coverage-guided fuzz harnesses for security-critical
components of the Aether codebase.

## CBOR Fuzzer (`tests/fuzz/cbor/`)

`std.cbor` decodes messages sent between sandboxed children and their parents,
making it a primary attack surface. The fuzz harness tests the pure Aether
implementation (`std/cbor/module.ae`) using libFuzzer and sanitizers (ASan, UBSan).

### Files

- **`fuzz_cbor.ae`**: the target, built `--emit=obj`, exporting
  `fuzz_target(data: ptr, len: int)`. It takes the input whole, zeros
  included (`bytes.string_from_ptr`), and runs the oracle: parse with the
  defaults; round-trip a parsed value through `encode_deterministic` (it must
  re-parse and re-encode to the same bytes); parse again with small limits
  taken from the first byte, where anything those stricter options accept the
  defaults must accept, as the same value. Oracle failures `abort()`; an
  ordinary parse error is a pass.
- **`fuzz_cbor.c`**: `LLVMFuzzerTestOneInput`, which calls `aether_fuzz_target`.
- **`gen_corpus.py`** writes **`corpus/`** from hex, so the seeds can be read
  in review: RFC 8949 Appendix A and some of Appendix F (malformed), the byte
  inputs of `std/cbor/test_cbor_hardening.ae`, an empty input, a truncated
  head, a byte string holding 0x00, arrays nested 255 and 257 deep, duplicate
  keys, indefinite lengths and length bombs. Edit the script, not the files.
- **`cbor.dict`**: the head bytes of every major type and length width, the
  break byte, the simple and float heads, and tags 2, 3, 24 and 55799.
- **`run_fuzz_cbor.sh`**: builds and runs the harness under libFuzzer, ASan
  and UBSan for `FUZZ_SECONDS` (default 60). It first checks that `41 00`
  reaches the decoder as 2 bytes. It fails, saying why, when clang has no
  libFuzzer; it never skips.
- **`prove_mutants.sh`**: plants two bugs in a copy of `std/cbor/module.ae`
  (a local `std/cbor/` shadows the stdlib's) and shows the fuzzer finds each:
  - **M1**: the nesting depth check removed (a stack overflow);
  - **M2**: the check that a string's declared length fits in the bytes left
    removed (an oversized allocation or an out-of-bounds read).

### Running the fuzzer

```sh
make fuzz-cbor                                   # 60 seconds
FUZZ_SECONDS=1800 sh tests/fuzz/cbor/run_fuzz_cbor.sh
```

It is not part of `make test`; CI runs it in `make fuzz-ci` (see below). The
binary, the working corpus (which grows from run to run) and any reproducer
stay in `build/fuzz/cbor/`; `FUZZ_OUT` moves them.

### Reproducing and minimising a crash

A crash, leak, timeout or oracle failure writes `crash-<sha1>` (or `leak-`,
`timeout-`, `oom-`) into `build/fuzz/cbor/`, and the script names it.

```sh
build/fuzz/cbor/fuzz_cbor build/fuzz/cbor/crash-<sha1>
build/fuzz/cbor/fuzz_cbor -minimize_crash=1 -runs=100000 \
    -exact_artifact_path=min.cbor build/fuzz/cbor/crash-<sha1>
```

Add the minimised input to `gen_corpus.py`, and a regression case to
`std/cbor/test_cbor_hardening.ae`.

### AFL++ (optional, untested here)

`afl-clang-fast -fsanitize=fuzzer` links AFL++'s own driver for an
`LLVMFuzzerTestOneInput` harness:

```sh
AE_CC="afl-clang-fast -g -O1" build/ae build tests/fuzz/cbor/fuzz_cbor.ae \
    --with=extern --emit=obj -o fuzz_cbor_ae.o
afl-clang-fast -fsanitize=fuzzer,address,undefined -g -O1 \
    tests/fuzz/cbor/fuzz_cbor.c fuzz_cbor_ae.o $(build/ae cflags) -o fuzz_cbor_afl
afl-fuzz -i tests/fuzz/cbor/corpus -o afl_out -x tests/fuzz/cbor/cbor.dict -- ./fuzz_cbor_afl
```

## Frame-reader fuzzer (`tests/fuzz/ipc_frames/`)

`std.ipc`'s `read_frame` and `read_frame_timeout` take length-prefixed
frames from a sandboxed child over the socket `sandbox.spawn_socket` keeps
(#3503). The reader keeps no state between calls, so what each call returns
is a pure function of the bytes available when it is made. The harness
delivers a fuzzer-chosen byte stream over an `os.socketpair` in chunks cut at
fuzzer-chosen offsets, with a fuzzer-chosen `max`, and checks every answer
against a model of the documented contract (`std/ipc/module.ae`): the whole
payload and nothing of the next frame; `TIMED_OUT_BETWEEN_FRAMES` with
nothing consumed when no byte of a frame is there; `TIMED_OUT_INSIDE_FRAME`
with the part consumed when some of one is; "longer than max" with exactly
the header consumed; and the three end-of-stream errors once the writer is
gone. Each payload the reader hands back goes through `cbor.parse`, which
may fail (a pass) but must not crash or leak.

A chunk is either queued before the reader is called or, when the input
asks for it, written from a second thread while the reader already waits in
`read_frame_timeout`, so the reader's read-some, wait, read-more path sees
bytes arrive; the end of stream is read with either form. The reader itself
(`std/ipc/aether_ipc.c`, untouched) is compiled into the target with the
same coverage and sanitizer flags as the harness, so libFuzzer steers by its
branches and UBSan watches them. Needs `poll()` and `os.socketpair`, so not
Windows; every write stays under 8 KiB, the AF_UNIX socket buffer on macOS,
so none blocks while the reader is not reading.

### Files

- **`fuzz_ipc_frames.ae`**: the target, built `--emit=obj --with=extern,os`,
  exporting `fuzz_target(data: ptr, len: int)`. Byte 0 picks `max`
  (1..4081 in steps of 16); byte 1's low three bits the number of chunks
  (1..8), its bit 3 whether chunks arrive from the writer thread, its bit 4
  whether the end of stream is read with the timeout form; the next bytes
  are the cut offsets, and the rest is the stream. A disagreement with the
  model `abort()`s.
- **`fuzz_ipc_frames.c`**: `LLVMFuzzerTestOneInput` and the writer thread.
- **`gen_corpus.ae`** writes **`corpus/`** (`make fuzz-ipc-frames-corpus`):
  whole, empty and split frames, a frame split at every byte, short headers
  and payloads, frames at and one over `max`, headers announcing 2^31 - 1
  and 2^32 - 1 bytes, a payload that is not CBOR, forty small frames, and a
  4 KB frame. Edit the generator, not the files.
- **`ipc_frames.dict`**: frame headers at the lengths that matter and a few
  CBOR heads.
- **`run_fuzz_ipc_frames.sh`**: builds and runs the harness under libFuzzer,
  ASan and UBSan for `FUZZ_SECONDS` (default 60).
- **`prove_mutants.sh`**: compiles three one-line edits of a copy of
  `std/ipc/aether_ipc.c` (the real file is untouched) the same way and shows
  the model catches each: **M1** the `max` check removed; **M2** a timeout
  inside a frame's header reported as the between-frames one; **M3** an
  empty frame answered as a timeout. A mutant counts only when the model's
  abort leaves a `crash-*` input, and that input is then replayed against
  the clean build, which must accept it.

### Running

```sh
make fuzz-ipc-frames                                   # 60 seconds
FUZZ_SECONDS=1800 sh tests/fuzz/ipc_frames/run_fuzz_ipc_frames.sh
sh tests/fuzz/ipc_frames/prove_mutants.sh
make fuzz-ipc-frames-corpus                            # after editing gen_corpus.ae
```

Not part of `make test`; CI runs it in `make fuzz-ci`. Output stays in
`build/fuzz/ipc_frames/` (`FUZZ_OUT` moves it). A reproducer is run and
minimised as for the CBOR fuzzer, with `build/fuzz/ipc_frames/fuzz_ipc_frames`.

## C-parser fuzzers (`tests/fuzz/http_head/`, `http_chunked/`, `url/`, `json/`)

HTTP is where most of std.net's closed bugs were, and each of these parsers
reads what a peer sends. Each target is a C harness (`fuzz_<target>.c`,
`LLVMFuzzerTestOneInput`) that compiles the std sources it tests into itself
with the flags std objects are built with, so libFuzzer steers by the
parsers' own branches and ASan and UBSan watch them; the rest of the runtime
comes from `build/libaether.a`. Each checks properties of the parser's
documented contract, and a failed check `abort()`s naming what disagreed:

- **`http_head`**: the bytes a client sent on one connection, taken as the
  worker does (`std/net/aether_http_server.c`): `http_find_header_end`,
  `http_request_head_check`, the chunked body, `http_parse_request_n`, for
  each pipelined request. The head check gives the answer a model of its
  contract gives (a NUL or a CR or LF outside a CR LF is malformed; the
  request line and header line limits; obs-fold, whitespace before a colon,
  a name that is not a token; Content-Length a plain decimal that fits a
  `long`, duplicates equal; Transfer-Encoding read as one list across its
  fields, chunked last and once, never beside a Content-Length), and the
  Content-Length it accepts is that exact value. The parser then reads the
  head as the check did: it parses exactly when the request line is method
  (a token) SP target (no control byte) SP `HTTP/1.0` or `HTTP/1.1`, the
  headers are the same lines with the same names and trimmed values, and the
  body is exactly the Content-Length bytes or the decoded chunks, NUL bytes
  included. The client's reader of a response's Transfer-Encoding fields
  (`http_te_in_block`) reads the same lines as the head check. Every request
  is freed.
- **`http_chunked`**: a chunked body as it arrives. `http_chunked_frame` and
  `http_dechunk` give what a reference reading of RFC 9112 7.1 written in
  the harness gives (sizes of 1 to 16 hex digits read exactly, at most
  `INT64_MAX`; the chunk-ext grammar; CR LF after every line and every
  chunk's data; trailers to the empty line; lines of at most 1024 bytes),
  decoding exactly what frames to the same bytes. The answer does not depend
  on where the bytes were cut: every prefix of a whole body needs more, every
  longer buffer frames the same length, and a refusal stays a refusal. The
  same bytes as the body of a response (`http_response_fill_from_bytes`),
  under a handful of Transfer-Encoding fields, are decoded to the same
  payload when the codings end in chunked and the body frames, and come back
  as they came otherwise (RFC 9112 6.3).
- **`url`**: a URL, then optionally a NUL and a Location. `parse_url` gives
  the host, port, target and scheme a reading of its contract gives (the
  authority ends at the first `/`, `?` or `#`; a bracketed IPv6 host comes
  back bare; a port is 1 to 5 digits naming 1..65535; the target keeps the
  query and drops the fragment), and a host never holds a control byte or a
  space, since it is written into the Host header as it is. What it accepts
  parses the same when written back from its pieces, `http_parse_proxy_url`
  agrees with it, and a resolved Location (`http_resolve_location`) is an
  absolute URI with a lower-case scheme, no control byte, nothing unescaped
  outside its authority, and resolves to itself.
- **`json`**: a document, every byte of it. `json_parse_raw_n` accepts
  exactly what an RFC 8259 validator written in the harness accepts (no
  trailing commas, leading zeros, comments, lone surrogates, bad UTF-8 or
  bare control bytes), with nesting as deep as `JSON_MAX_DEPTH`; a refusal
  carries an error kind, a position and a message. A parsed value serializes,
  reads back to the same value (the same double or integer, strings and keys
  byte for byte, members in order; an infinity as null, a double with no
  fraction as the integer of that value) and serializes to the same text.

Each directory holds the harness, **`gen_corpus.ae`**, which writes
**`corpus/`** (`make fuzz-corpus`; edit the generator, not the seeds), and a
dictionary, **`<target>.dict`**. The seeds are committed as binary
(`.gitattributes`), so an HTTP seed keeps its CR LF line ends on every
checkout. **`run_fuzz_c.sh <target>`** builds and runs one target as the
CBOR script does, its output in `build/fuzz/<target>/`.

```sh
make fuzz-http-head                    # or fuzz-http-chunked, fuzz-url, fuzz-json; 60 s
FUZZ_SECONDS=1800 make fuzz-json
make fuzz-corpus                       # after editing a gen_corpus.ae
```

### Without libFuzzer

`FUZZ_REPLAY=1` builds a target with `$CC` and no libFuzzer, linked to
**`replay_main.c`**, a `main` that feeds each file it is given, or each file
in a directory it is given, to `LLVMFuzzerTestOneInput` once. With
`-dict=FILE` it reads a dictionary as libFuzzer reads it (`name="value"` or
`"value"`, printable bytes, the escapes `\\`, `\"` and `\xNN` only), fails
on a line libFuzzer would refuse to start on, naming it, and runs each entry
as an input. `make fuzz-replay` runs every dictionary entry and every seed
of every run_fuzz_c.sh target through their property checks that way (h2
where libnghttp2 is found), and reads the CBOR and frame-reader dictionaries
too (`-check_dict=`); it works where clang has no libFuzzer, Windows (MinGW)
among them, and finds nothing new. It also replays a reproducer:

```sh
make fuzz-replay
FUZZ_REPLAY=1 FUZZ_REPLAY_INPUTS=crash-1234 sh tests/fuzz/run_fuzz_c.sh http_head
```

A reproducer from CI is minimised with the target's libFuzzer binary as for
the CBOR fuzzer; add it to the target's `gen_corpus.ae` and a regression test
beside the module's own (`tests/runtime/test_http_server.c`,
`tests/runtime/test_runtime_http.c`, `tests/regression/test_json_*.ae`).

## Every other decoder that reads untrusted input (#3749)

Eleven more targets run through **`run_fuzz_c.sh`** the same way, each in
its own directory with its harness, **`gen_corpus.ae`**, **`corpus/`** and
dictionary. Four are C harnesses; the Aether ones (**`fuzz_<target>.ae`**,
exporting `fuzz_target(data, len)`) are compiled by `ae build --emit=obj`
with the fuzzing compiler, so the std modules they import carry the
coverage and the sanitizers, and **`ae_target.c`** is their
`LLVMFuzzerTestOneInput`. Each checks its decoder's contract; the header of
each harness lists the properties.

- **`websocket`** (C): `http_ws_recv` over a stream the input gives, part
  already buffered past the upgrade and the rest from a transport that
  hands it over in chunks (the pure-TLS hooks stand in for the socket). Each
  message, each frame it sends back (pong, close echo, a 1002 or 1009
  close) and each refusal agree with a reading of RFC 6455 5 written in the
  harness, for a client handle and a server handle, under the default cap
  and small ones; the reassembly buffer never grows past the cap or much
  past what arrived. Two of the reader's leniencies are modelled as they
  are: a continuation with no message to continue, and a data frame in the
  middle of a fragmented message, which starts a new one; text is not
  checked to be UTF-8.
- **`h2`** (C, needs libnghttp2): the HTTP/2 glue driven with no socket,
  through `aether_h2_session_feed` and `aether_h2_session_drain`. The input
  is raw frames, or a script of client frames the harness encodes with
  nghttp2's own HPACK deflater, so it knows what each stream sent: every
  request the server's middleware sees is one a stream asked for, with the
  method, path, query, fields and body it sent, never one with a missing
  pseudo-header or a connection-specific field, never twice; every whole,
  valid request is dispatched while the connection stands; the server
  writes well-formed frames, a `:status 200` and the echoed body for each.
- **`proxy_response`** (C): what an upstream sends back, framed as the
  proxy event loop frames it, through the copying path (the client's reader
  and the exchange) and the pass-through (`aether_proxy_direct_take`,
  `aether_proxy_direct_head`). Each writes one well-formed head with no
  line break inside a field and no hop-by-hop field, one Content-Length that
  is the body's (none for HEAD), and the two write the same head and body.
- **`xml`** (C): the reader's events nest, names and text hold only what
  they may, an accepted document written again with the builder reads back
  to the same events, and `xml_pretty` formats exactly what the reader
  accepts.
- **`yaml`** (C, with the vendored libfyaml compiled in): a refusal carries
  a message, the tree is consistent, a mapping finds each scalar key's first
  value, and an accepted document emits text that parses to the same tree.
- **`msgpack`** (Aether): what unpacks packs, unpacks and packs to the same
  bytes; one value is the whole input (no proper prefix is accepted, a byte
  after it is refused); nothing nests past 256.
- **`tar`**, **`zip`** (Aether): the input is extracted for real, under
  `$FUZZ_SCRATCH` (`build/fuzz/<target>/scratch`), beside a directory that
  holds one canary file. Nothing is written outside the destination,
  whatever the archive names; no symbolic link is made when they are
  refused (and, for tar, none points out when they are allowed); an
  extraction that succeeds wrote what the reader reads, within its limits.
- **`tls13_record`** (Aether): a record sealed in the harness, with any
  content type and padding, opens to its content or is refused with the
  alert RFC 8446 5 gives; a changed byte, or a record as received, is
  refused with the alert its header gives or bad_record_mac; the sequence
  number moves only for a record that decrypted; `check_header` agrees with
  a reading of 5.1 and 5.2; `seal_records` cuts a long plaintext into
  records that open back to it.
- **`tls13_handshake`** (Aether): the message parsers the server and the
  client run (ClientHello, ServerHello and HelloRetryRequest, the
  extension blocks, Certificate, CertificateRequest, NewSessionTicket):
  a refusal names an alert TLS 1.3 defines, what is accepted is described
  from inside the message, and what the module's builders write its
  parsers read back.
- **`x509`** (Aether): `parse_certificate`, the DER reader under it, PEM and
  OCSP status. An accepted certificate is described from inside its bytes
  and matches each of its own plain DNS names; the DER reader stays inside
  its input, 32 levels into constructed contents; PEM decodes to bytes with
  a label.

The full state machines of the TLS server and client run over a socket and
are not driven here; their parsers are. The seeds come from the existing
tests: the certificates are read out of the TLS integration tests' sources,
the messages and archives are written by the modules' own builders, and the
refusals are the ones the modules' tests pin.

```sh
make fuzz-websocket      # or fuzz-h2, fuzz-proxy-response, fuzz-xml, fuzz-yaml, fuzz-msgpack,
                         # fuzz-tar, fuzz-zip, fuzz-tls13-record, fuzz-tls13-handshake, fuzz-x509
FUZZ_REPLAY=1 sh tests/fuzz/run_fuzz_c.sh x509     # the seeds, without libFuzzer
```

Bugs these targets found, fixed with a regression test each: std.yaml
answered an empty stream with no document and no error, could not emit the
empty document, and looked keys up as YAML source; the HTTP client read a
response with no head as status 0 with no error, which the proxy wrote to
its client as `HTTP/1.1 0`; the proxy's pass-through answered with an
interim (1xx) status, the copying path kept one of a repeated field (one
Set-Cookie of several) and a response held at most 50 fields; ASN.1
indefinite lengths nested without bound and overflowed the stack (a 60 KB
certificate); `asn1.last_tag_class`, `last_tag_pc` and `last_tag_number`
were exported and never defined; std.tar's `entry_read` failed instead of
reporting the end of an entry read whole.

## In CI (`make fuzz-ci`)

**`fuzz_ci.sh`** runs the targets `FUZZ_TARGETS` names (by default every
one, as **`select_targets.sh --all`** lists them), the CBOR and
frame-reader fuzzers included, for `FUZZ_SECONDS` each (default 60), one
after another; a target that fails does not stop the rest, and the script
names each that failed.

`.github/workflows/fuzz.yml` runs it on Linux with clang 18, in three shards
side by side (`select_targets.sh --shard I/3`), about 6 minutes of fuzzing
and 4 of building each:

- on a pull request, the targets its changes reach: `select_targets.sh
  --changed` reads `git diff --name-only` against the base, and holds the
  one table of what each target is built from (a change to the runtime, the
  compiler, `ae`, std.string, std.bytes, the Makefile or the runner reaches
  them all); the workflow's paths filter covers the same paths;
- on every push to main, every target;
- weekly (Sunday 03:17 UTC), every target for 600 seconds.

On a failure it uploads the reproducers as the `fuzz-reproducers-<shard>`
artifact.
