# Audio test vectors

`tone440_22050_mono.ogg` is the OGG Vorbis fixture for
`std/audio/test_audio_ogg.ae` (#2364): a 440 Hz sine at amplitude 0.5,
22050 Hz mono, 11025 frames (0.5 s), encoded at Vorbis quality 0.6. The spec
synthesises the same samples as its WAV twin and checks the OGG decodes to
the twin's rate, length and peak.

`mktone.c` made it, against libvorbis:

```sh
gcc mktone.c -o mktone -lvorbisenc -lvorbis -logg -lm
./mktone tone440_22050_mono.ogg
```

Regenerating changes the file's bytes (libvorbis versions differ), not what
the spec checks. SHA-256 of the committed file:
`4595c49b8a3f73c3e41aeb8fa0b30419add2430ed1c0b78bfd8a3585eb502e06`.
