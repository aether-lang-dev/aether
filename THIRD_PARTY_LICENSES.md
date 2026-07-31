# Third-party licenses

Portions of Aether's standard library and contrib modules are ported from
third-party open-source projects. Each ported source file carries an
attribution header naming its upstream; this file reproduces the upstream
licence texts in full. Where a module is original to Aether it is *not* listed
here.

## Bouncy Castle (bc-csharp) — MIT License

Used by the cryptography port (issue
[#739](https://github.com/aether-lang-dev/aether/issues/739)). Files that port
logic, structure, or test vectors from Bouncy Castle for .NET carry the
attribution header:

```
// MIT License (https://opensource.org/licenses/MIT)
//
// Portions copyright (c) 2000-2026 The Legion of the Bouncy Castle Inc. (https://www.bouncycastle.org)
//
// Portions copyright (c) 2026 Aether Developers.
```

Ported so far:

- `std/bits/` (`std.bits`) — bit helpers ported from `crypto/src/util/Integers.cs`
  and `crypto/src/util/Longs.cs` (rotate / leading-zeros / popcount).
- `std/bytes/` big-endian accessors (`set_be16/32/64`, `get_be16/32/64`) —
  modelled on `crypto/src/crypto/util/Pack.cs`.
- `tests/regression/test_bits.ae`, `test_bytes_be.ae` — test vectors ported
  from `crypto/test/src/util/utiltest/{IntegersTest,LongsTest}.cs`.

### License text

```
MIT License (https://opensource.org/licenses/MIT)

Copyright (c) 2000-2026 The Legion of the Bouncy Castle Inc. (https://www.bouncycastle.org).
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sub license, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions: The above copyright notice and this
permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
