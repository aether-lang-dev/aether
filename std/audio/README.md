# std.audio

Audio playback: open a device, load samples, play them.

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

## Exports

`open`, `close`, `is_null_backend`, `load_wav`, `load_pcm`, `unload`,
`play`, `pause`, `stop`, `is_playing`, `volume`, `get_volume`, `seek_ms`,
`position_ms`, `duration_ms`, `channels`, `sample_rate`, `last_error`, and
the `FORMAT_*` constants.
