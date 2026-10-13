- **`ae` works again in a working directory longer than 1023 bytes.** A
  checkout found by its `build/aetherc` in the working directory takes the
  directory as its toolchain root; #2862 refused one longer than the root
  holds, where `ae` had used "." for it. The directory is now read whole,
  and one too long for the root is named "." again, which is exact while
  `ae` runs there (#2862).
