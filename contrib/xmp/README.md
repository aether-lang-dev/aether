# contrib.xmp: a tracker-module player for std.audio

Plays tracker modules (MOD, S3M, XM, IT, MED and the rest of the formats
[libxmp](https://github.com/libxmp/libxmp) reads) through `std.audio`. It is
full libxmp: the player, every loader, ProWizard and the depackers, not
libxmp-lite. A player renders interleaved signed 16-bit stereo PCM, and
std.audio plays it, either the whole module at once or block by block into
a stream source. contrib.xmp has no audio output of its own: std.audio
(miniaudio) is the only backend.

It lives in `contrib/` because it is not something every program expects of
a standard library, and libxmp is a sizeable C library fetched against a
pinned checksum. See [docs/stdlib-vs-contrib.md](../../docs/stdlib-vs-contrib.md).

## Playing a module

```aether
import std.audio
import std.fs
import contrib.xmp

main() {
    data, n, rerr = fs.read_binary("song.xm")
    if rerr != "" { println(rerr); return }
    p, err = xmp.load(data, n)
    if err != "" { println(err); return }
    defer xmp.free(p)
    println("${xmp.title(p)} (${xmp.format(p)}), ${xmp.duration_ms(p)} ms")

    if !audio.open() { return }
    // A ring of half a second; pump keeps it full.
    src, serr = xmp.stream_open(p, 48000, 24000)
    if serr != "" { println(serr); return }
    _ = audio.play(src)
    while !xmp.ended(p) {
        _, _ = xmp.pump(p, src)
        sleep(100)
    }
    // ... let the ring drain, then
    audio.unload(src)
    audio.close()
}
```

For a short module, or one played many times, render it once instead:
`src, err = xmp.sound(p, 48000)` gives a std.audio source of the whole song
(via `audio.load_pcm`) that plays, seeks and loops like any other source.

## API

All errors follow contrib.sqlite and std.audio: a fallible call returns
`(value, "")` or `(null / "" / -1, reason)`, and the reason is a static
string beginning `xmp:` (or `audio:` from std.audio).

### Loading and freeing

| Call | Returns | |
|---|---|---|
| `xmp.load(data, length)` | `ptr!` | A module from its bytes (as `fs.read_binary` gives them). The bytes are copied, and never read past the string's own length. |
| `xmp.load_file(path)` | `ptr!` | A module from a file; libxmp unpacks a packed one (zip, gzip, xz, LHA, PowerPacker, ...) itself. |
| `xmp.free(p)` | | Release the player and its module. `null` is a no-op. |

Bytes that are no module, an empty input, a truncated module or a missing
file are refused with a reason and no player.

### Module info

`xmp.title(p)`, `xmp.format(p)` (strings, owned copies), `xmp.channels(p)`,
`xmp.patterns(p)`, `xmp.orders(p)` (the length of the order list),
`xmp.instruments(p)`, `xmp.samples(p)` and `xmp.duration_ms(p)` (libxmp's
scan of the main sequence, once through). All are valid from `load` on.

### Playing

| Call | Returns | |
|---|---|---|
| `xmp.start(p, sample_rate)` | `string` | Start, or restart, from the top at 4000 to 768000 Hz. `""` or the reason. |
| `xmp.render(p, frames)` | `string!` | The next `frames` frames, `frames * xmp.FRAME_BYTES` bytes (FRAME_BYTES is 4). Fewer at the end of a module that is not looping, `""` once it has ended. Up to 4,194,304 frames a call. |
| `xmp.render_all(p)` | `string!` | From where the player stands to the end, once, looping held off for the call. Refused past 30 minutes. |
| `xmp.seek_ms(p, ms)` | `string` | Move to `ms` into the module, to the libxmp frame (a tick). |
| `xmp.set_order(p, order)` | `string` | Move to the start of entry `order` of the order list. |
| `xmp.position_ms(p)` | `int` | Where the next rendered frame falls, in ms. |
| `xmp.order(p)`, `xmp.row(p)` | `int` | The order-list entry and pattern row now playing. |
| `xmp.looping(p, on)`, `xmp.is_looping(p)` | | Wrap at the end (on) or stop there (off, the default). |
| `xmp.ended(p)` | `bool` | A module that is not looping has played to its end. |

The PCM is std.audio's `FORMAT_S16`: signed 16-bit, two channels
interleaved, in the machine's byte order (little-endian on every target).

### Through std.audio

| Call | Returns | |
|---|---|---|
| `xmp.sound(p, sample_rate)` | `ptr!` | `start`, `render_all`, then `audio.load_pcm`: a std.audio source of the whole module. Release it with `audio.unload`. |
| `xmp.stream_open(p, sample_rate, capacity_frames)` | `ptr!` | `start`, then `audio.stream_open` with a ring of `capacity_frames`, filled once by `pump`. Release it with `audio.unload`. |
| `xmp.pump(p, src)` | `int!` | Render as much as the ring has room for and push it. The frames pushed: 0 when the ring is full or the module has ended. |

## Threading

A player belongs to one thread at a time, and every call does its work on
the calling thread, there and then. Nothing in contrib.xmp starts a thread
or needs a callback. With a stream source, std.audio's mixer thread drains
the ring on its own while the thread that owns the player calls `pump`
often enough to keep it from running dry: a ring of `capacity_frames` lasts
`capacity_frames / sample_rate` seconds, so pump at least twice in that
time. `audio.stream_underruns(src)` counts any silence the mixer had to
substitute. Each player has its own libxmp context; there is no state
shared between players, so two players on two threads are independent.

## Build

`import contrib.xmp` is all a program needs. The module's `@link`
(`-laether_xmp -lxmp -lm`) puts the libraries on the link line.

### Where libxmp comes from

libxmp is compiled from its release source, pinned by version, URL and
SHA-256 in [`libxmp.lock`](libxmp.lock) (libxmp **4.7.4**, the release
tarball from libxmp's GitHub releases; its SHA-256 is the one libxmp
publishes in the release notes). It is not committed:
[`scripts/fetch-libxmp.sh`](../../scripts/fetch-libxmp.sh) downloads it,
refuses it unless the checksum matches, and extracts `include/`, `src/`
(without `src/lite`), `COPYING` and `README` into `contrib/xmp/libxmp/`
(gitignored), with `sources.txt`: the C files libxmp's own `Makefile.vc`
compiles for the full library (its `OBJS`, `PROWIZ_OBJS` and
`DEPACKER_OBJS`). libxmp's `configure` and CMake are not used. Release
archives ship the extracted directory, so an installed toolchain never
fetches anything.

- **Native (`make contrib`).** The veneer builds into
  `build/contrib/libaether_xmp.a`, and libxmp's sources into
  `build/contrib/libxmp.a` beside it, which `-lxmp` resolves to ahead of any
  system library: the program carries libxmp and needs none where it runs.
  `make install-contrib` installs both. The archive is cached in
  `.ci-cache/libxmp/` (`LIBXMP_OBJ_CACHE_DIR`), keyed by the release, the
  compiler and the flags.
- **Cross (`ae build --target=<triple>`).** The veneer and libxmp are
  compiled for the target with zig cc, each of libxmp's objects cached per
  target in the ae cache (`~/.aether/cache`, or `AETHER_CACHE_DIR`). The
  first build for a target prints `Compiling libxmp for <triple>`. A
  `CROSSBUILD_SYSROOT` staging `libaether_xmp.a` and `libxmp.a` still takes
  precedence.
- **No archives (a binary release, or a tree without `make contrib`).**
  `ae build` compiles the veneer and the libxmp source shipped in
  `share/aether/contrib/xmp/` with the host compiler, caches the objects and
  the archive, and links them in place of `-laether_xmp -lxmp`.
- **System library (fallback).** When the source cannot be fetched
  (offline, no `curl`/`wget`) or `LIBXMP_SYSTEM=1` is set, `make contrib`
  builds the veneer against the system `xmp.h` and `-lxmp` links the system
  libxmp (`libxmp-dev` on Debian and Ubuntu, `libxmp` in Homebrew, MSYS2,
  FreeBSD ports). Cross builds have no such fallback.

The one compile flag, `LIBXMP_STATIC`, lives in the lock's `LIBXMP_CFLAGS`
and is part of every cache key. With no `HAVE_*` flags libxmp never starts
a helper program to unpack a module, which is the configuration
`Makefile.vc` builds.

### Moving to a newer libxmp

Change the four values in `libxmp.lock` (`LIBXMP_VERSION`, `LIBXMP_URL`,
`LIBXMP_SHA256`, `LIBXMP_CFLAGS`), checking the SHA-256 against the one in
libxmp's release notes, run `scripts/fetch-libxmp.sh`, run the tests below,
and update the version in this README.

## Test

```sh
ONLY="xmp/" bash .github/scripts/contrib_check.sh            # the module spec, test_xmp.ae
sh tests/integration/libxmp_fetch/test_libxmp_fetch.sh      # the fetch script, offline
sh tests/integration/xmp_vendored_cross/test_xmp_vendored_cross.sh  # needs zig
```

The spec writes its own modules, byte by byte: a four-channel ProTracker MOD
and a two-channel Fast Tracker II XM, each one looped square-wave sample that
sounds for the first half of the song and is silenced for the second. It
asserts on the rendered PCM (sound where the note plays, silence where it
does not, the song's exact length, where seeking and looping land) and runs
its std.audio half on std.audio's headless engine, so it needs no sound
card. No third-party music is in the tree.

## Licence

libxmp is MIT-licensed, Copyright (C) 1996-2026 Claudio Matsuoka and
Hipolito Carraro Jr; some of its source files come from other projects under
their own notices. See [NOTICE](NOTICE), which release archives ship with
`libxmp/COPYING`.
