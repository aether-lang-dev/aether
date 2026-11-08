- **Building aether natively on FreeBSD links again.** The toolchain's link
  line dropped libfyaml's flags word by word, and pkg-config's libfyaml line
  there begins `-L/usr/local/lib`, so every `-L/usr/local/lib` went with it
  and `aetherc` failed with `unable to find library -lnghttp2`. Only the run
  of words libfyaml added is removed now.
