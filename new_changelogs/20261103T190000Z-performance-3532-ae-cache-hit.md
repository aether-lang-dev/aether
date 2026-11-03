- **A warm `ae run` or `ae build` starts about twice as fast (#3532).** On a
  cache hit `ae` re-read every file of the cache key, aetherc, ae,
  libaether.a, the C compiler and, in a source tree, every runtime and std
  header, about 18 MB one byte at a time, and listed the whole cache
  directory for stale temporaries on every command. It now remembers each
  file's content hash beside its size, modification time and identity
  (`filehash.v1` in the cache directory) and reads a file again only when
  one of those changed, hashes eight bytes at a time, sweeps temporaries with
  the size-cap scan at most every ten minutes, and starts `gcc --version`
  directly instead of through cmd.exe. On a Windows machine whose cache had
  grown to 140,000 entries a warm `ae run` of hello world went from 183 ms to
  93 ms; with an empty cache, from 138 ms to 96 ms. The key still holds
  content hashes, so no build that used to be rebuilt is served from the
  cache now.
