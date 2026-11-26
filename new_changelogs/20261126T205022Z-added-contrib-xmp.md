- **`contrib.xmp`: a tracker-module player for std.audio.** `import
  contrib.xmp` loads MOD, S3M, XM, IT, MED and the other formats libxmp
  reads (full libxmp: every loader, ProWizard and the depackers), from bytes
  or a path, reports the title, format, channels, patterns, orders and
  duration, and renders interleaved 16-bit stereo PCM with seeking, order
  jumps and looping. `xmp.sound` renders a whole module into a std.audio
  source (`load_pcm`); `xmp.stream_open` and `xmp.pump` feed a std.audio
  stream source block by block. std.audio stays the only audio output. Each
  player carries its own libxmp context, and nothing in the module runs on a
  thread of its own. libxmp 4.7.4 is pinned by URL and SHA-256 in
  `contrib/xmp/libxmp.lock` and fetched by `scripts/fetch-libxmp.sh`, never
  committed; `make contrib` compiles it from the source list libxmp's
  Makefile.vc builds into `build/contrib/libxmp.a`, `ae build --target`
  compiles it for the target, a binary release ships the source (with
  libxmp's MIT licence, and `contrib/xmp/NOTICE`) and the archives, and the
  system libxmp is the fallback when the source cannot be fetched.
