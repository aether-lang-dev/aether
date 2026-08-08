# contrib/i18n — internationalization support

Phase 5 of the Aether i18n/l10n RFC ([#863](https://github.com/aether-lang-dev/aether/issues/863)).
The earlier phases live in `std` (`std.language` BCP 47, `std.plural`,
`std.message` ICU MessageFormat, `std.number`); this phase adds the data-heavy
piece the RFC deliberately deferred — **locale-aware collation** — in `contrib/`
so its vendored Unicode data tables sit alongside it rather than in core `std`.

## `collate` — Unicode collation (string sorting)

`contrib.i18n.collate` orders strings by the Unicode Collation Algorithm
(UTS #10) rather than raw byte value, so accents and case tie-break correctly:

```aether
import contrib.i18n.collate

collate.compare("en", "apple", "banana")  // -1  (primary: a < b)
collate.compare("en", "apple", "Apple")   // -1  (tertiary: case)
collate.compare("en", "cafe", "café")     // -1  (secondary: accent)
collate.compare("en", "café", "café")     //  0  (canonical equivalence)

collate.sort("en", my_string_list)        // in-place, UCA order
key = collate.sort_key("en", s)           // stable, order-preserving key
```

- `compare(locale, a, b) -> int` — -1 / 0 / 1.
- `sort_key(locale, s) -> string` — a precomputed, order-preserving key;
  comparing two keys as ordinary strings matches `compare`. Compute once per
  element when sorting large lists repeatedly.
- `sort(locale, items)` — sort a `list` of strings in place.

**Scope / honesty:** this is **DUCET** (language-neutral) collation with
canonical (NFD) normalization and full three-level weighting
(primary/secondary/tertiary). It is **not yet per-locale *tailored*** collation
(e.g. Swedish sorting å/ä/ö after z, or Spanish traditional ñ): the `locale`
argument is accepted for API stability and future tailoring but currently
selects DUCET order for every locale. That already matches the large majority of
real-world ordering expectations across scripts.

## Layout

```
contrib/i18n/
  aether_i18n.c          collation engine (UCA over DUCET + utf8proc NFD)
  collate/module.ae      the `collate` Aether module
  collate/test_collate.ae  regression test (leak-checked)
  utf8proc/              vendored utf8proc 2.9.0 (MIT) — NFD normalization
    utf8proc.c utf8proc.h utf8proc_data.c LICENSE.md VERSION
  ducet/                 Default Unicode Collation Element Table, UCA 15.1.0
    allkeys.txt          pinned Unicode source (Unicode license — see NOTICE)
    gen_ducet.py         generator: allkeys.txt -> ducet_data.c
    ducet_data.c         GENERATED table (do not edit)
    ducet.h NOTICE
```

## Building a consumer

The module is backed by three C files; pass them with `--extra`:

```sh
ae build yourprog.ae \
  --extra contrib/i18n/aether_i18n.c \
  --extra contrib/i18n/utf8proc/utf8proc.c \
  --extra contrib/i18n/ducet/ducet_data.c
```

Or, for the bundled test: `make contrib-i18n-check`.

## Regenerating the DUCET table

`ducet_data.c` is generated from the pinned `allkeys.txt`:

```sh
python3 contrib/i18n/ducet/gen_ducet.py contrib/i18n/ducet/allkeys.txt \
  > contrib/i18n/ducet/ducet_data.c
```

To move to a newer Unicode version, replace `allkeys.txt` with the matching
`https://www.unicode.org/Public/UCA/<version>/allkeys.txt`, update the utf8proc
vendored copy to a release targeting the same Unicode version, and regenerate.

## Licensing

- **utf8proc** — MIT (`utf8proc/LICENSE.md`); its bundled data is under the
  Unicode license (also in that file).
- **DUCET / `allkeys.txt`** — © Unicode, Inc., under the Unicode Terms of Use;
  see `ducet/NOTICE`. `ducet_data.c` is a mechanical transformation of it and
  carries the same terms.
