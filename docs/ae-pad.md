# `ae pad`, struct layout and padding

aetherc names C types and the C compiler places them: an Aether struct is
emitted with its fields in declaration order, and C puts each field at the
next multiple of its alignment. A struct that mixes bytes and longs pays for
it in holes, and nothing in the language says so. `ae pad` does: for every
struct, message and actor a file declares it prints each field's offset and
size, the holes between them and the tail, the order that would be smallest,
and whether `ae pad --fix` may rewrite the source to it.

```sh
ae pad app/rec.ae                 # the report
ae pad std contrib                # every .ae file under the directories, then the totals
ae pad --min-waste=8 src          # exit 1 when a rewritable struct would save 8 bytes or more
ae pad --verify src               # prove every number against the C compiler
ae pad --fix src                  # rewrite the rewritable structs to the smallest order
```

## The report

```aether,fragment
struct Rec {
    a: byte
    b: long
    c: byte
    d: long
    e: int
}
```

```text
struct Rec  app/rec.ae:1  size 40  align 8  holes 14 (2 x 7)  tail 4  lines 1 (64-aligned) / 2 (16-aligned)
     0  a  byte  1
     1  (hole 7)
     8  b  long  8
    16  c  byte  1
    17  (hole 7)
    24  d  long  8
    32  e  int   4
    36  (tail 4)
  suggested: b, d, e, a, c  -> size 24 (-16, -40%)  reorderable: yes
```

`lines` is the number of 64-byte cache lines the value spans when it starts
on a line, and at worst when only its own alignment, or malloc's 16 bytes, is
promised. The suggested order is a stable sort by alignment, largest first,
ties in declaration order; since the size of every C type is a multiple of its
alignment, that order leaves no hole between fields, only the tail. When no
order is smaller the report says `keep this order`, whatever the alignments.

Each file's report ends with a `summary:` line (structs, messages, actors,
bytes of holes and tail, the saving of the structs `--fix` may rewrite and the
largest of them, the saving of all of them), and a run over several files
ends with the totals. `--json` prints one object, `{"files": [...],
"totals": {...}}`, with the same fields per type (`fields` with `offset`,
`size` and `align`, `suggested`, `saving`, `reorderable` and `reason`), for CI
and other tools.

## The table

The layout comes from compiler/analysis/type_layout.c, one table of what the
generated C is, per target, for each type a field can hold:

| Aether | C | size / align (64-bit) |
|---|---|---|
| `int`, `uint32`, `f32`, an enum, `bool` | `int`, `uint32_t`, `float`, the enum, `int` | 4 / 4 |
| `byte`, `uint8` | `unsigned char`, `uint8_t` | 1 / 1 |
| `uint16` | `uint16_t` | 2 / 2 |
| `long`, `int64`, `uint64`, `Duration`, `float`, `bit_set[E]` | `int64_t`, `uint64_t`, `double`, `unsigned long long` | 8 / 8 |
| `string`, `ptr`, `*T`, an actor reference, a C function pointer | a pointer | 8 / 8 |
| a closure | `_AeClosure`, two pointers | 16 / 8 |
| `T[]` | `AetherSlice` | 24 / 8 |
| `T[N]` | `T name[N]` | N times T |
| `T?` | `struct { int has; T val; }` | |
| a tuple | `struct { T0 _0; T1 _1; ... }` | |
| a sum `A \| B` | `struct { tag; union { A A_; B B_; } data; }` | |
| `f32x4` and the other 128-bit vectors | | 16 / 16 |
| `f32x8`, `i32x8` | | 32 / 16 |
| `longdouble` | `long double` | 16 / 16 |

A `bool` field is a C `int`, 4 bytes: a struct of flags is smaller as `uint8`
fields or a `bitstruct`. `--ptr=4` lays out for 4-byte pointers with i386
SysV placement (int64 and double 4-aligned inside a struct, a 12-byte long
double); `--target=<triple>` picks the row of a target (`i686-windows` and
`arm-linux-gnueabihf` align int64 to 8, `aarch64-macos` has an 8-byte long
double, and so on; an unknown triple is refused with the list). What the C
compiler decides, a bitfield, a `@c_import` struct, an `extern type`, is
reported as unknown, never guessed.

A message is laid out as codegen emits it: the 4-byte `_message_id` first,
then its fields by alignment (#3773); the lone `int` of a message with no
other field is an `intptr_t`. An actor is ActorBase (952 bytes on a 64-bit
target with the default 16-slot mailbox), its state in declaration order (a
state field used as a pointer is a `void*`), and, when it has receive arms,
the 32-byte `AetherMsgInline` copy last; the struct is aligned to 64. ActorBase
is modelled for 64-bit targets only, so `--ptr=4` reports an actor as unknown.
Messages and actor state are reported, with the order that would be smaller,
but never rewritten: their order is codegen's.

## `--verify`

`ae pad --verify` compiles each file to C, appends a `_Static_assert` of every
offset, size and alignment the report gives, and compiles that with the C
compiler and the flags `ae` builds with, `-fsyntax-only`: nothing is written
and nothing runs, and the compiler itself proves the table. A failed assert
names the number (`"Rec.b at 8"`). `--verify --ptr=4` adds `-m32` (on MinGW
also `-mno-align-double -mno-ms-bitfields`, whose i686 default is the
Microsoft layout); it needs a compiler that takes `-m32`. `--verify` does not
take `--target=`, since it compiles with this host's compiler.

## `--fix`

`ae pad --fix` rewrites the field list of each struct the report marks
reorderable and smaller in the suggested order. A field moves with the
comment lines right above it and the comment after it on its line; a blank
line stays where it was, and so does each line's separator (`,`, `;` or
none), so the struct reads as before with its fields in another order. A
struct written on one line (`struct P { a: byte, b: long }`) has its fields
moved and its gaps kept. A second `--fix` changes nothing: a rewritten
struct is already in its smallest order.

A run decides over all of its files before it rewrites any. A struct a module
declares may be one a program that imports it hands to C, so the compiler's
plan for each file lists every struct that file refuses, wherever it is
declared, and no file of the run rewrites a struct another file refused (the
line says which file: `not rewritten: passed to extern fn draw (line 3),
which C implements (seen from src/main.ae)`). A module that no program of the
run imports is not rewritten at all: who else uses it is not known. Run
`ae pad --fix` over the whole project, `ae pad --fix src`, not over a module
alone. After a file is rewritten it is compiled again, with every program of
the run that imports it, and put back as it was if one of them no longer
compiles; only the rewrites that stand are reported and counted.

A body `--fix` cannot follow is left as it is, named, and the run exits
non-zero: two fields on one line of a multi-line body, a field on the line
of the opening or the closing brace, a field written over several lines, a
block comment over several lines, a field in the C-style `int x` form. So is
a file `--fix` cannot replace safely: a symbolic link (the file it names is
the one to rewrite) and a file holding a NUL byte. A rewritten file keeps its
mode, and the temporary it is written through is created afresh, never one
that is already there. `--json` is the report alone and is refused with
`--fix` or `--verify`. A directory reached through a symbolic link is not
walked.

Struct literals and field access name their fields, so moving a field
changes no Aether code. What reads the order from outside the source is what
`--fix` refuses, each with its reason in the report:

| refused | reason |
|---|---|
| `extern struct`, `@packed`, `@c_import`, `extern type` | C declares the layout |
| a struct held by an extern struct | C lays it out with the extern struct |
| a struct in the signature of an `extern` function | C code reads it |
| a struct in the signature of a function an `--emit=lib` build exports (and `@c_callback` ones) | a C host, and the generated SDKs, follow the field order |
| a struct a message carries | `--emit-header` writes the message for C hosts |
| `@derive(schema)` | the schema table lists the fields in declaration order |
| `@observable` | observers are told the index of the field that changed |
| a struct reached through a pointer in a file that reads raw offsets with `std.mem` | a hard-coded offset would go stale |
| a struct whose pointer becomes an untyped `ptr` (`h as ptr`, a `ptr` parameter) | C may read its bytes at offsets of its own (`fwrite(h as ptr, ...)`) |

Each refusal follows a struct into the structs it holds or points at. Three
of them are conservative, and say so where they are approximate:

- `ae pad` cannot know how a file is built, so the `--emit=lib` rule applies
  to every file: a struct in the signature of a function that such a build
  would export is refused even in a program only ever built as an executable.
  The rule is the one the `--emit=lib` build applies (codegen's
  `lib_fn_is_local`), so a struct passed only between `_`-suffixed or
  internal functions stays rewritable.
- A raw `mem.get_*` or `mem.set_*` offset access cannot be followed to the
  struct whose bytes it reads. In a file that has one, every struct the file
  reaches through a pointer (`*S`, `p as *S`, `heap.new(S)`), and every struct
  a function doing such an access handles at all, is refused, the reason
  starting `conservative:`.
- Where an untyped `ptr` goes is not followed either. A `*S` or `heap.new(S)`
  cast to `ptr`, stored where a `ptr` is declared, or passed to any parameter
  that is not `*S` itself refuses S. Freeing it (`free`, `heap.free`) does
  not: that reads no field.

A struct a binary-imported module was compiled with is that module's, so it
is not in this file's report at all.

## `aetherc --emit=layout`

`ae pad` drives `aetherc --emit=layout`, as `ae inspect` drives
`--emit=inspect`. The report runs after codegen, because which actor state
fields are pointers and which functions a library exports are codegen's
decisions; the C still goes to the output path, the null device when none is
given:

```sh
aetherc --emit=layout app.ae                    # the text report
aetherc --emit=layout --json app.ae             # JSON
aetherc --emit=layout --layout-plan app.ae      # `fix` lines, for ae pad --fix
aetherc --emit=layout --layout-asserts app.ae app.c   # app.c, and the asserts on stdout
aetherc --emit=layout --ptr=4 app.ae            # the 32-bit table
aetherc --emit=layout --target=arm-linux-gnueabihf app.ae
```
