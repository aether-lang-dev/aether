- **AGENTS.md says a runtime change needs `make stdlib` before `.ae`
  tests (#3674).** Its rebuild table said a `runtime/**` change needs no rebuild for
  `.ae` builds because the runtime is compiled into each one. In a source
  tree with `build/libaether.a`, which every built tree has, `ae build`
  links that archive and compiles the runtime only when it is missing, and
  the build cache keys on the archive, so an edit to a runtime `.c` file
  was not in the program at all until `make stdlib`, cache hit included.
