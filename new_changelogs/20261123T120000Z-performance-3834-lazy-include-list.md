- **`ae` builds the runtime and std include list only for a command that
  compiles (#3834).** Every command walked the runtime/ and std/ trees for
  the `-idirafter` list when it found the toolchain, though only compile
  and link lines and `ae cflags` read it. It is now built where it is first
  asked for: the same directories in the same order. A warm-cache
  `ae run hello.ae` on Windows no longer pays for the walk.
