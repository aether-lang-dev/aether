- **`std.audio` gains the mixer tier a game engine needs per source (#2205).**
  Playback could set a volume and nothing else; ae3d, putting sound on game
  objects with the listener on the camera, needed more. Per source: `pan`
  (equal-power, the Web Audio StereoPannerNode law, in a C node between the
  sound and its bus — miniaudio's own panner is a linear crossfade), `pitch`
  (a rate ratio, for Doppler and engine notes), `looping`, `fade_ms`, and
  `played_frames`. Groups/buses (`group_new`, `group_volume`, `set_group`)
  give music, effects, ambience and voice a volume each. `stream_open` +
  `stream_push` make a queue source: a bounded lock-free ring the program
  pushes PCM into as it plays, for procedural sound and decoders of your own;
  an empty ring plays silence and counts an underrun rather than ending the
  sound. And `open_headless` opens an engine with no device and no mixer
  thread, where nothing advances until `render(frames)` pulls it through the
  mixer, so a headless test holds a mix to numbers (`rendered_peak`,
  `rendered_sample`, and per source the volume, pan, pitch and frames played)
  the way ae3d's renderers are held to pixels. `load_wav` already decodes as
  the mixer pulls, so long music needs no separate streaming path, and mp3 /
  flac already decode through it. Sources no longer pass through miniaudio's
  spatializer (the engine owns that model), so a set volume is the volume
  heard. `std/audio/test_audio_mixer.ae` holds every number above under the
  headless engine.
