# std.audio: a PCM source, so a decoder can supply samples

**From:** the aether-ui video line (2026-08-09) · **Where it bit:**
`apps/video_frame`, which plays an MP4 in a UI frame with A/V sync working —
but takes its audio from a hand-extracted sidecar WAV.

## The ask, in one line

A way to play PCM samples that the caller already has, rather than only
container bytes that miniaudio can parse itself:

```
audio.load_pcm(data, length, sample_rate, channels, format) -> ptr!
```

Everything downstream — `play`, `pause`, `position_ms`, `duration_ms`,
`seek_ms`, `volume` — should work on the returned source exactly as it does
for `load_wav` today. `position_ms` in particular is the A/V-sync master
clock, and it is the reason this matters.

## Why the current surface cannot do it

`load_wav` is better than its name: it is `ma_decoder_init_memory`, which
sniffs the format, so it already accepts more than WAV. Measured on
0.510.0:

| Input | Result |
| --- | --- |
| WAV | accepted |
| **MP3** | **accepted** — `duration_ms=6013` |
| MP4 (whole file) | rejected: "unsupported or malformed audio data" |
| raw AAC (`-c:a copy` out of the MP4) | rejected: same |

So the gap is not "only WAV". It is that **every** entry point takes an
encoded container miniaudio can demux, and there is no way in for samples a
*different* decoder produced. (The `load_wav` name understates what it does
and is worth revisiting separately — a caller reading the API would not
guess MP3 works.)

## What we are doing instead

`contrib/avcodec` (in-process video decode, landed 0.510.0) removed the
intermediate file for video: frames come straight from FFmpeg into a vg
raster region. Audio still cannot make the same trip, so `video_frame`
requires a manual pre-step:

```
ffmpeg -i clip.mp4 -vn -ar 44100 -ac 2 clip.wav     # 20 MB sidecar for a 21 MB source
```

That is the exact intermediate-file problem `contrib/avcodec` was written to
eliminate, reappearing on the audio side — and it is worse than it looks:

- it roughly **doubles on-disk cost** (20 MB sidecar for a 21 MB clip; a
  feature film would be gigabytes of PCM);
- it is a **manual step before playback**, so an app cannot just open a file
  the user picked;
- for a **live source** — a camera, a network stream, a generator — there is
  no file to extract from and no workaround at all. Same shape as the
  `fd_read_into` ask (#1471), which fixed the equivalent hole on the read
  side.

## Why this is the natural seam

FFmpeg is already demuxing the container. `contrib/avcodec` opens the file,
finds the video stream, and reads *past* the audio packets. Teaching it to
decode those packets to PCM is a small, contained addition on our side — it
already has the format context, and the frame-handoff pattern
(`try_/get_/release_` plus a zero-allocation `_into` variant) is written and
tested.

What is missing is somewhere to put the samples. `std.audio` already owns
the device, the mixer and the clock; it needs a source constructed from
memory rather than from a decoded blob. In miniaudio terms that is
`ma_audio_buffer` (or a custom `ma_data_source`) instead of `ma_decoder`,
fed into the same `ma_sound_init_from_data_source` the shim already calls.

## Two shapes, either would work

**1. Whole-buffer PCM** (simpler; matches today's ownership model)

```
audio.load_pcm(data, length, sample_rate, channels, format) -> ptr!
```

The caller decodes fully, hands over the samples, and `std.audio` copies
them the way `load_wav` copies its encoded input. Good enough for a clip
that fits in memory, which covers the current demo and most app audio.

**2. Streaming push** (the one that unblocks live sources)

```
audio.open_stream(sample_rate, channels, format) -> ptr!
audio.push_pcm(src, data, length) -> int!     # bytes accepted; 0 = buffer full
audio.stream_end(src)
```

A ring buffer the caller tops up from a decode loop, so nothing needs to fit
in memory. This is what a camera, a network stream, or a two-hour film
actually wants. It also raises a question worth answering deliberately:
whether `position_ms` should then report the DEVICE's play position rather
than a decoder offset — for A/V sync it must, since that is the clock video
chases.

Shape 1 alone would remove the sidecar for `video_frame`. Shape 2 is the
one that makes `std.audio` usable for anything live.

## Not urgent, and not blocking

A/V sync is proven and correct today — video chases `audio.position_ms` to
within 3 ms on a real 720p/5.1 clip. Nothing about the clock relationship
changes with where the samples come from; this is packaging, not
architecture. Filing it because the sidecar is the last hand-cranked step in
an otherwise in-process pipeline.

## Environment

aether 0.510.0 (`92619ba1`), Linux/CachyOS. `std.audio` is miniaudio-backed
with `MA_NO_ENCODING`; decoders are compiled in, which is why MP3 already
works.
