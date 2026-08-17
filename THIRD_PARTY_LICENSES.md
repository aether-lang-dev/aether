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
- `std/bits/test_bits.ae`, `tests/regression/test_bytes_be.ae` — test vectors
  ported from `crypto/test/src/util/utiltest/{IntegersTest,LongsTest}.cs`.

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

## PCRE2 — BSD-3-Clause (with binary-redistribution exemption)

Used by `std.regex` (issue
[#1389](https://github.com/aether-lang-dev/aether/issues/1389)). The directory
`std/regex/pcre2/` is a byte-identical vendored subset of upstream PCRE2
(<https://github.com/PCRE2Project/pcre2>), pinned by version and SHA-256 in
`std/regex/pcre2/VENDOR.md` and regenerated only by
`scripts/vendor-pcre2.sh`. It carries no local patches; three files keep the
alternate names upstream itself designates for non-autotools embedding
(`pcre2.h`, `config.h`, `pcre2_chartables.c`). The upstream licence text,
also present as `std/regex/pcre2/LICENCE`:

```
PCRE2 LICENCE
-------------

PCRE2 is a library of functions to support regular expressions whose syntax
and semantics are as close as possible to those of the Perl 5 language.

Releases 10.00 and above of PCRE2 are distributed under the terms of the "BSD"
licence, as specified below, with one exemption for certain binary
redistributions. The documentation for PCRE2, supplied in the "doc" directory,
is distributed under the same terms as the software itself. The data in the
testdata directory is not copyrighted and is in the public domain.

The basic library functions are written in C and are freestanding. Also
included in the distribution is a just-in-time compiler that can be used to
optimize pattern matching. This is an optional feature that can be omitted when
the library is built.


THE BASIC LIBRARY FUNCTIONS
---------------------------

Written by:       Philip Hazel
Email local part: Philip.Hazel
Email domain:     gmail.com

Retired from University of Cambridge Computing Service,
Cambridge, England.

Copyright (c) 1997-2024 University of Cambridge
All rights reserved.


PCRE2 JUST-IN-TIME COMPILATION SUPPORT
--------------------------------------

Written by:       Zoltan Herczeg
Email local part: hzmester
Email domain:     freemail.hu

Copyright(c) 2010-2024 Zoltan Herczeg
All rights reserved.


STACK-LESS JUST-IN-TIME COMPILER
--------------------------------

Written by:       Zoltan Herczeg
Email local part: hzmester
Email domain:     freemail.hu

Copyright(c) 2009-2024 Zoltan Herczeg
All rights reserved.


THE "BSD" LICENCE
-----------------

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notices,
      this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notices, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of the University of Cambridge nor the names of any
      contributors may be used to endorse or promote products derived from this
      software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.


EXEMPTION FOR BINARY LIBRARY-LIKE PACKAGES
------------------------------------------

The second condition in the BSD licence (covering binary redistributions) does
not apply all the way down a chain of software. If binary package A includes
PCRE2, it must respect the condition, but if package B is software that
includes package A, the condition is not imposed on package B unless it uses
PCRE2 independently.

End
```

## Project Wycheproof — Apache License 2.0

Used by the crypto test suites (issues
[#739](https://github.com/aether-lang-dev/aether/issues/739),
[#1298](https://github.com/aether-lang-dev/aether/issues/1298)). The
directory `tests/vectors/wycheproof/` holds byte-identical **test-vector
data files** from Project Wycheproof
(<https://github.com/C2SP/wycheproof>), pinned by commit in the README
there. Vectors are data consumed by tests, not compiled code. Apache-2.0:
the full licence text is upstream's `LICENSE`; per §4 we retain this
attribution and the files carry no modifications.

```
Copyright 2016 Google Inc., and later contributors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```
