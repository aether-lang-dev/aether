# Aether contrib nightly — results

Nightly HEAD build on CachyOS (newer GCC/Clang than the GitHub CI box).
Orphan branch (`nightly-results`, no shared history, no CI). Latest snapshot only.

- **run:** 2026-08-08T05:23:20Z
- **HEAD:** b456ac53 Merge pull request #1445 from aether-lang-dev/feat/schema-defaults-transforms-jsonschema-nested
- **toolchain:** gcc (GCC) 16.1.1 20260725 / clang version 22.1.8
- **host:** CachyOS, kernel 7.1.5-1-cachyos

## ❌ 3 step(s) FAILED

### Pipeline steps

| step | status | ms |
|------|--------|----|
| `racket-CS embedding tree` | ❌ FAIL | 3 |
| `make ci` | ❌ FAIL | 751756 |
| `make contrib` | ✅ PASS | 2193 |
| `make contrib-host-check` | ✅ PASS | 1320 |
| `contrib ae-check sweep` | ❌ FAIL | 318 |

### Contrib module type-check (`ae check`)

| module | status | ms |
|--------|--------|----|
| _(sweep did not run)_ | ❌ FAIL | 0 |
