# std.audio

Audio playback: open a device, load samples, play them, and the mixer tier a
game engine needs per source — pan, pitch, looping, groups, a queue source,
and a headless engine that holds a mix to numbers.

The backend is chosen at build time. When none is available the module falls
back to a **null backend** that accepts everything and produces no sound —
`is_null_backend()` reports which you have, so a program can warn rather than
appearing broken.

The example **compiles but is not run** in CI: playback needs a device, and a
build machine has none.

```aether
import std.audio
import std.fs

main() {
    if audio.is_null_backend() {
        println("no audio backend in this build")
    }

    // One global device: open takes no handle and returns a bool.
    if !audio.open() {
        println("could not open the device: ${audio.last_error()}")
        return
    }

    // load_wav takes the BYTES, not a path — read the file yourself,
    // which keeps the module out of the filesystem.
    data, dlen, rerr = fs.read_binary("chime.wav")
    if rerr != "" {
        println("read failed: ${rerr}")
        audio.close()
        return
    }

    source = audio.load_wav(data, dlen) or {
        println("decode failed: ${audio.last_error()}")
        audio.close()
        return
    }

    audio.play(source)
    println("duration: ${audio.duration_ms(source)}ms")

    audio.unload(source)
    audio.close()
}
```

`load_wav` decodes a WAV container from bytes; `load_pcm` takes raw samples
with an explicit format — one of the `FORMAT_*` constants — for audio that
arrives already decoded. Both return `ptr!`, so a corrupt file is a value to
handle rather than a crash.

The device is process-global: `open` and `close` take no handle. Sources are
individual, loaded and `unload`ed separately, and `play`/`pause`/`stop` act on
one.

`last_error()` returns the backend's own diagnostic, which is more specific
than the error string when a device refuses to open.

## The mixer tier: what a game engine needs per source

A source on a game object and a listener on the camera need more than a
volume. Each source also has a `pan` (equal-power, the Web Audio
StereoPannerNode law, so the engine can compute its expected numbers from
the spec), a `pitch` ratio (Doppler, an engine note), `looping` (rain, wind)
and a `fade_ms`. Sources are routed through **groups** — music, effects,
ambience, voice — each with a volume of its own.

```aether,fragment
import std.audio

main() {
    _ = audio.open()
    effects, _ = audio.group_new()
    _ = audio.group_volume(effects, 0.8)

    engine, _ = audio.load_wav(engine_wav, engine_len)
    _ = audio.set_group(engine, effects)
    _ = audio.looping(engine, true)
    _ = audio.play(engine)

    // Each tick, from the listener's frame:
    _ = audio.pan(engine, -0.3)      // a little to the left
    _ = audio.pitch(engine, 1.2)     // revving

    audio.unload(engine)
    audio.group_free(effects)
    audio.close()
}
```

`load_wav` decodes as the mixer pulls, so a long track costs its encoded
size and needs no separate streaming path. For audio that does not exist yet
— a procedural wind, a decoder of your own — `stream_open` makes a **queue
source**: a bounded ring the program `stream_push`es PCM into while it plays.
An empty ring plays silence and counts an underrun (`stream_underruns`)
rather than ending the sound.

## Holding a mix to numbers

`open_headless(rate, channels)` opens an engine with no device and no mixer
thread: nothing advances until `render(frames)` pulls frames through the
mixer, so a test's numbers follow from the calls it made, the way a
renderer's pixels do. After a render, `rendered_peak(channel)` and
`rendered_sample(frame, channel)` read the block back, and per source the
last applied `get_volume`, `get_pan`, `get_pitch` and the `played_frames`
are all readable.

```aether,fragment
_ = audio.open_headless(8000, 2)
src, _ = audio.load_pcm(pcm, n, 8000, 1, audio.FORMAT_S16) // constant 0.125
_ = audio.play(src)
_ = audio.render(1000)
// a mono source at pan 0 sits at cos(pi/4) in each ear
assert near(audio.rendered_peak(0), 0.125 * 0.7071)
assert audio.played_frames(src) == 1000
```

The sound reads its source a block (512 frames) ahead of the output, so
`position_ms` and `stream_queued` can lead what has been rendered by up to
a block; `played_frames` is exact.

## Exports

`open`, `open_headless`, `close`, `is_null_backend`, `is_headless`,
`load_wav`, `load_pcm`, `unload`, `play`, `pause`, `stop`, `is_playing`,
`volume`, `get_volume`, `pan`, `get_pan`, `pitch`, `get_pitch`, `looping`,
`is_looping`, `fade_ms`, `played_frames`, `seek_ms`, `position_ms`,
`duration_ms`, `channels`, `sample_rate`, `group_new`, `group_free`,
`group_volume`, `group_get_volume`, `set_group`, `stream_open`,
`stream_push`, `stream_queued`, `stream_capacity`, `stream_underruns`,
`render`, `rendered_peak`, `rendered_sample`, `last_error`, and the
`FORMAT_*` constants.
