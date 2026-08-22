# Aether contrib nightly — results

Nightly HEAD build on CachyOS (newer GCC/Clang than the GitHub CI box).
Orphan branch (`nightly-results`, no shared history, no CI). Latest snapshot only.

- **run:** 2026-08-22T03:05:37Z
- **HEAD:** 92ff3d98 Merge pull request #1688 from aether-lang-dev/release/v0.563.0
- **toolchain:** gcc (GCC) 16.1.1 20260725 / clang version 22.1.8
- **host:** CachyOS, kernel 7.1.5-1-cachyos

## ✅ ALL GREEN

### Pipeline steps

| step | status | ms |
|------|--------|----|
| `racket-CS embedding tree` | ✅ PASS | 3 |
| `make install (nightly prefix)` | ✅ PASS | 54008 |
| `make ci` | ✅ PASS | 913496 |
| `make contrib` | ✅ PASS | 2507 |
| `make contrib-host-check` | ✅ PASS | 4207 |
| `contrib ae-check sweep` | ✅ PASS | 861 |
| `contrib build+run (make contrib-check)` | ✅ PASS | 79010 |
| `wycheproof full sweep` | ✅ PASS | 1840691 |

### Tests actually run

| suite | passed | failed | note |
|-------|--------|--------|------|
| C unit tests (`make ci`) | 394 | 0 | |
| `.ae` suite (`make test-ae`) | 981 | 0 | |
| examples built + run | 90 | 0 | |
| host-bridge specs (`[3/3]`) | 35 | 0 |  |

> A row missing here means that suite produced no parseable
> count — treat it as unknown, not as zero.

### Contrib tests (build + run, `make contrib-check`)

| test | status | detail |
|------|--------|--------|
| `avcodec/decode` | ✅ PASS | (run) |
| `sqlite/roundtrip` | ✅ PASS | (run) |
| `templating/native` | ✅ PASS | (run) |
| `parsers/xml_expat` | ✅ PASS | (run) |
| `templating/liquid/syntax` | ✅ PASS | (run) |
| `templating/liquid/values` | ✅ PASS | (run) |
| `templating/liquid/tags` | ✅ PASS | (run) |
| `templating/liquid/filters` | ✅ PASS | (run) |
| `templating/liquid/inheritance` | ✅ PASS | (run) |
| `tinyweb/spec` | ✅ PASS | (run) |
| `tinyweb/inventory` | ✅ PASS | (run) |
| `tinyweb/integration` | ✅ PASS | (run) |
| `tinyweb/schema_api` | ✅ PASS | (run) |
| `tinyweb/websocket` | ✅ PASS | (run) |
| `i18n/collate` | ✅ PASS | (run) |
| `vulkan/offscreen` | ✅ PASS | (run) |
| `vulkan/resources` | ✅ PASS | (run) |
| `vulkan/actors` | ✅ PASS | (run) |
| `vulkan/depth-msaa` | ✅ PASS | (run) |
| `vulkan/frames` | ✅ PASS | (run) |
| `vulkan/materials` | ✅ PASS | (run) |
| `vulkan/example-triangle` | ✅ PASS | (run) |
| `vulkan/example-parallel` | ✅ PASS | (run) |
| `vulkan/example-sprites` | ✅ PASS | (run) |

### Wycheproof full sweep (per family)

| family | counts |
|--------|--------|
| `x25519` | ✅ 487 matched, 31 rejected-acceptable, 0 failed |
| `chacha20poly1305` | ✅ 256 valid ok, 60 forgeries rejected, 9 skipped (iv!=96), 0 failed |
| `aes_gcm` | ✅ 116 valid ok, 81 forgeries rejected, 119 skipped (iv!=96/tag!=128), 0 failed |
| `hmac_sha256` | ✅ 33 valid ok, 54 bogus rejected, 87 skipped (truncated tag), 0 failed |
| `hkdf_sha256` | ✅ 83 valid ok, 3 invalid rejected, 0 failed |
| `ed25519` | ✅ 88 valid ok, 63 forgeries rejected, 0 failed (stride 1) |
| `rsa_pkcs1_2048_sha256` | ✅ 9 valid ok, 250 invalid rejected, 0 failed |
| `rsa_pss_2048_sha256` | ✅ 63 ok, 45 invalid rejected, 0 failed |
| `x448` | ✅ 498 matched, 12 rejected-acceptable, 0 failed |
| `ecdsa_p256_p1363` | ✅ 173 ok, 89 invalid rejected, 0 failed (stride 1) |
| `ecdsa_p256_der` | ✅ 174 ok, 310 invalid rejected, 0 failed (stride 1) |

### Contrib module type-check (`ae check`)

> **This table is a TYPE-CHECK, not a test run.** A green row
> means the module parses and type-checks — it does not mean
> any of its code executed. Modules that also RUN are in the
> two tables above; a module appearing only here has no
> runtime coverage in this pipeline.

| module | type-checks | tests run | ms |
|--------|-------------|-----------|----|
| `avcodec` | ✅ PASS | 1 ran | 8 |
| `host/aether` | ✅ PASS | 4 passed | 7 |
| `host/duktape` | ✅ PASS | 5 passed | 7 |
| `host/factor` | ✅ PASS | 6 passed | 8 |
| `host/go` | ✅ PASS | _no runtime coverage_ | 7 |
| `host/java` | ✅ PASS | _no runtime coverage_ | 8 |
| `host/js` | ✅ PASS | _no runtime coverage_ | 7 |
| `host/lua` | ✅ PASS | 5 passed | 8 |
| `host/perl` | ✅ PASS | 5 passed | 8 |
| `host/python` | ✅ PASS | 5 passed | 8 |
| `host/racket` | ✅ PASS | _no runtime coverage_ | 8 |
| `host/rhombus` | ✅ PASS | _no runtime coverage_ | 8 |
| `host/ruby` | ✅ PASS | 5 passed | 7 |
| `host/tcl` | ✅ PASS | _no runtime coverage_ | 7 |
| `host/tinygo` | ✅ PASS | _no runtime coverage_ | 11 |
| `i18n/collate` | ✅ PASS | _no runtime coverage_ | 9 |
| `parsers/xml_expat` | ✅ PASS | _no runtime coverage_ | 9 |
| `sqlite` | ✅ PASS | 1 ran | 8 |
| `templating/liquid` | ✅ PASS | _no runtime coverage_ | 182 |
| `templating/native` | ✅ PASS | _no runtime coverage_ | 12 |
| `tinyweb` | ✅ PASS | 5 ran | 53 |
| `tinyweb/schema_api` | ✅ PASS | _no runtime coverage_ | 44 |
| `vulkan` | ✅ PASS | 9 ran | 13 |
