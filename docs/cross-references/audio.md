# An audio API for Aether, costed cross-reference and v1 scope

> **Purpose.** Aether has no audio API. Before building one, this doc does what
> worked for C3 (`c3.md` → five stdlib modules) and error-unification: inventory
> the best-designed audio APIs, map each concept onto what Aether *already
> owns*, pin the C-substrate decision against the no-C-consumers rule, and scope
> a small v1. Origin: `asks/audio-api-cross-reference-and-v1.md`.
>
> **Driving use case (confirmed with the requester, not assumed): PLAYBACK**
> play files/streams in an app. The concrete consumer is `apps/LisMusic` in the
> aether-ui repo, a music-player port whose `lis_audio.ae` seam is a stub today
> purely for lack of any audio API. Synthesis/DSP, capture, and game-style
> mixing are named later tiers, deliberately out of v1.

## 1. Framing: two separate questions

1. **Whose API *design* do we map?**, the Aether-level vocabulary and shape.
2. **Whose *plumbing* do we stand on?**, the C device/decode substrate.

These have different answers, because the best-designed audio APIs are not in
languages whose runtime model matches Aether's. Conflating them produces either
a C-shaped Aether API (miniaudio's handle soup surfaced verbatim) or an
unimplementable Aether-shaped C fantasy (traits/graph schedulers with no
substrate). The survey keeps them apart throughout.

## 2. Concept map, candidates vs. what Aether already owns

The columns that matter: what the candidate's core concept is, and which
*existing* Aether primitive expresses it. Nothing in v1 may require a new
language feature.

### 2.1 Go, `beep` (over `oto`): the design to map

| beep concept | What it is | Aether already owns |
| --- | --- | --- |
| `Streamer` | pull-based sample source: `Stream(buf) -> (n, ok)` | a closure `fn(buf: ptr, frames: int) -> int` (closures shipped; `std.mem` pokes fill the buffer), **tier 2**, see §6 |
| `Mixer` | a streamer that sums other streamers | v1: the substrate's mixer (§5); tier 2: an Aether streamer |
| `Ctrl` / `Volume` | streamers wrapping streamers (pause, gain) | v1: substrate controls; tier 2: wrapper closures |
| `Format` / `SampleRate` | sample-format descriptor | fixed in v1, one PCM format, convert at the edges (§9) |
| `speaker.Init` / `speaker.Play` | device open + hand a streamer to the device | `audio.open()` / `audio.play(...)` over the substrate |
| seek/position (`beep.Seeker`) | position in samples | **`Duration`** (the language's ns scalar) at the API; frames internally |

Why beep wins the design slot: one organising idea, no inheritance, no graph
scheduler, no generics, and Aether is explicitly Go-ergonomic. Every beep
concept lands on an existing Aether primitive.

### 2.2 Web Audio API (W3C): the vocabulary to steal

| Web Audio concept | Verdict |
| --- | --- |
| node graph + `AudioContext` scheduler | **declined** (§8), framework-weight |
| names: *gain*, *biquad filter*, *analyser*, *buffer source*, *pan* | **adopt** when the effect tier lands (§4) |
| `decodeAudioData` returning a buffer | shape of `audio.load`, decode-to-source, fallible |

### 2.3 Rust, `cpal`/`rodio`/`dasp`: the layering lesson

| rodio concept | Verdict |
| --- | --- |
| `Source` trait (pull iterator of samples) | same idea as beep's Streamer; beep's non-generic spelling fits Aether |
| **device layer ≠ playback layer ≠ DSP layer** | **adopt as structure**: substrate (C) / `std.audio` (playback) / streamer tier (later) |
| trait/generic combinators | **declined**, Aether deliberately has no traits/generics; `(A, B)!` rejection (P1.5) set that precedent |

### 2.4 Elixir, Membrane: the actor tier, later

Media pipelines as supervised actor elements. Uniquely relevant because Aether
has Erlang-shaped actors and the runtime already owns the plumbing an element
pipeline needs (`runtime/actors/aether_spsc_queue.h`, lock-free mailboxes). But
Membrane is a large framework; the borrowable idea is exactly one sentence
**pipeline elements are actors**, and it belongs to the streaming tier
(internet radio, gapless preload), not v1. Declined-for-now, named as tier 3.

### 2.5 What Aether owns that no candidate has to provide

- **`bytes` / `mem`**, caps-accounted buffers + raw pokes: sample staging.
- **`Duration`**, seek/position type for free; no ad-hoc milliseconds ints.
- **`T!` + faults**, `audio.load(path)` is fallible the same way `fs.read` is;
  `err == audio.UnsupportedFormat` identity via the fault machinery (P3).
- **caps allocator + leak-tracker**, buffer ownership at the FFI edge is the
  same discipline every existing C-backed module already follows.
- **`@c_callback`**, exists (tests/integration/c_callback) but is *not* the
  realtime answer; see §6 for why.

## 3. The thing to steal that isn't a feature

**beep's organising principle: everything is a streamer.** A source is a
streamer; an effect is a streamer that wraps one; a mixer is a streamer that
sums several; the device just drains one streamer. Stated now, even though v1
ships no user-defined streamers, because it fixes the *seam*: v1's `Source`
handle is the thing that will later accept an Aether closure behind it. The v1
API must not paint over that seam (e.g. no play-by-filename-only API that hides
the source object).

## 4. Vocabulary decision

- v1 uses **player words** for player things: `play`, `pause`, `stop`, `seek`,
  `volume`, `position`, `duration`, because v1 is a playback API and LisMusic
  is a player. (`volume` not `gain` at this tier: it is the user-facing
  sound-level, exactly `ma_sound_set_volume`.)
- The effect tier (later) adopts **Web Audio's names**: `gain`, `biquad`
  (lowpass/highpass/…), `analyser`, `pan`, the industry lingua franca, so
  every search result and textbook maps onto our API.
- Sample math stays in **frames** internally, **`Duration`** at the API.

## 5. Plumbing decision: vendored miniaudio, pinned against the no-C rule

**Decision: vendor miniaudio** (single-file `miniaudio.h`, public-domain /
MIT-0 dual, David Reid) as `std/audio/`'s substrate, with a thin
`aether_audio.c` glue file, the same shape as every existing C-backed module.

Why it clears the committed constraint ("committed code must be Aether; zero
downstream C consumers; `dtoa.c` the one exception", plus the lzf precedent):

- **It is vendored upstream, not our C.** Exactly the `std/lzf/lzf_c.c` (Marc
  Lehmann, BSD-2) and `dtoa.c` category: a tracked third-party substrate below
  the FFI line. The strbuilder probe (2026-07-17) sharpened the rule: the tier
  that earns C is the tier whose *contract* is C, callbacks, realtime, OS
  APIs. An audio device layer is the definitional case.
- **It is not a downstream consumer of Aether.** Nothing in it calls
  Aether-emitted code; Aether calls *it* through externs, like OpenSSL/PCRE2
  except vendored rather than system-linked, which is strictly better for the
  CI matrix (no `libminiaudio-dev` on any platform; Windows/MinGW needs no new
  system dep).
- **One file covers the whole matrix**: ALSA/PulseAudio (Linux), CoreAudio
  (macOS), WASAPI/DirectSound (Windows), **sndio/OSS (FreeBSD, the
  GhostBSD/capsicum story)**, plus a **null backend**, which is the CI test
  story (§7). Decoders (dr_wav / dr_mp3 / dr_flac, same author & licence) come
  bundled: `wav + mp3` for v1 is zero extra dependencies.
- **The alternatives lose on the same axes**: PortAudio is a system dependency
  with no decoders; SDL_mixer drags SDL; OpenAL is 3D-game-shaped;
  per-platform hand C is five backends of Tier-3 work we'd own forever.

The glue (`aether_audio.c`, est. 150–300 lines) rides **miniaudio's high-level
engine** (`ma_engine` / `ma_sound`): device + decode + mixer + per-sound
volume/seek in one battle-tested layer, so v1's C is bindings, not logic.

## 6. The realtime-callback boundary (the one genuinely hard part)

The audio device pulls samples on a **realtime thread** that must never
allocate, lock, or block. Aether-emitted code cannot run there: heap-string
machinery allocates, uniform-heap wraps allocate, caps accounting touches
shared state. `@c_callback` existing does not change this, the constraint is
*what the callback body may do*, not whether the ABI is expressible.

Three rules, stated as the module contract:

1. **The device callback is C and stays inside the substrate.** In v1 it is
   miniaudio's own engine thread; our glue never registers an Aether closure
   as the data callback. (Same category as the epoll loop and the scheduler
   Nic's runtime-stays-C hard line applies verbatim.)
2. **Control flows in via atomics/commands, never calls.** `play` / `pause` /
   `volume` / `seek` are main-thread calls into `ma_engine`'s own atomic
   controls. Nothing waits on the realtime thread.
3. **Data crosses on a ring, not a call** (tier 2). When Aether-side streamers
   arrive (synthesis tier), samples cross via an SPSC ring buffer, a
   primitive the runtime *already ships*
   (`runtime/actors/aether_spsc_queue.h`): Aether fills on a worker cadence,
   the C callback drains, underrun plays silence. No Aether frame ever
   executes on the realtime thread.

**Cross-reference, `asks/ui-async-worker-for-blocking-io.md`.** Completion
*events* (track ended, device lost) are the same worker→main marshalling
problem that ask exists to solve for HTTP: a result produced off the UI thread
must invoke a closure *on* it. Audio must not invent a private post-back; v1
exposes track-end by **polling** (`audio.is_playing` / `audio.position` from
the app's existing `ui.timer` tick, LisMusic needs that tick for its seek
slider anyway), and upgrades to the async-worker primitive when it lands.

## 7. Testing story (CI has no sound card)

- miniaudio's **null backend** makes every device test headless-safe and
  deterministic: open/close, play-state transitions, seek arithmetic.
- **Decode-to-buffer** tests carry the correctness weight: a small committed
  wav fixture (and one mp3) decodes to a known frame count / duration /
  first-N samples, bit-exact asserts, valgrind-clean under the leak gates,
  same discipline as every module this year.
- The regression `.ae` test is self-contained; any helper files join the
  Makefile prune list per the established rule.

## 8. Considered and NOT recommended

| Candidate | Why declined |
| --- | --- |
| Web Audio's graph + `AudioContext` scheduler | framework-weight; a graph scheduler on a realtime thread is the hardest version of §6 for benefits playback doesn't need |
| Membrane wholesale | large framework; the one borrowable idea (elements are actors) is tier 3 |
| JUCE | C++ framework, plugin-ecosystem-shaped; nothing to vendor, everything to fight |
| rodio/dasp trait-generic combinators | Aether has no traits/generics by design; P1.5 set the single-payload precedent |
| PortAudio | system dep on every platform, no decoders, strictly worse than vendored miniaudio |
| SDL_mixer / OpenAL | pull in SDL / 3D-game shape; wrong tier for a stdlib playback module |
| writing the device layer ourselves | five OS backends of permanent Tier-3 C; the exact thing vendoring exists to avoid |

## 9. Verdict and costed v1 (playback)

**Stack: beep's streamer model as the Aether-level design · Web Audio's
vocabulary reserved for the effect tier · vendored miniaudio (`ma_engine`) as
the substrate.** Every concept in v1 maps onto a primitive that already exists;
no language work required.

### v1 surface (`std.audio`)

```aether,fragment
import std.audio

ok, err   = audio.open()                      // once per app; null-backend aware
src, err  = audio.load("song.mp3")            // decode-capable source (wav+mp3)
audio.play(src)                               // start / resume
audio.pause(src)
audio.seek(src, 90s)                          // Duration in, frames inside
audio.volume(src, 0.8)                        // 0.0..1.0, ma_sound volume
pos  = audio.position(src)                    // Duration — feeds the seek slider
dur  = audio.duration(src)                    // Duration
playing = audio.is_playing(src)               // poll from ui.timer (§6)
audio.unload(src)
audio.close()
```

Fallible ops return `T!`; error identities as declared faults
(`audio.UnsupportedFormat`, `audio.NoDevice`) per P3.

### Cost estimate

| Piece | Est. |
| --- | --- |
| vendored `miniaudio.h` (+ bundled dr_wav/dr_mp3) | 0 written lines; ~1 file |
| `std/audio/aether_audio.c` glue (engine init, sound handles, null-backend switch) | 150–300 |
| `std/audio/module.ae` | 120–200 |
| tests (decode fixtures + null-device regression) | 120–180 |
| **Total written** | **~400–700 lines**, one PR |

### Explicitly OUT of v1 (named later tiers)

- **Tier 2, synthesis/DSP**: Aether streamers over the SPSC ring (§6.3),
  Web-Audio-named effects. - **Tier 3, streaming/pipeline**: actor elements,
  Membrane-style (internet radio, gapless). - **Capture**, **game-style
  sample-bank mixing**, MIDI, spatial audio, format breadth beyond wav+mp3,
  any generic sample-format abstraction (one PCM format, convert at edges).

### The worked proof

LisMusic's `lis_audio.ae`: replace the stub with `std.audio`, wire the
transport buttons to `play`/`pause`, the seek slider to
`position`/`seek`, the volume control to `volume`, the same way
`contrib.sqlite` turned `lis_store` real.
