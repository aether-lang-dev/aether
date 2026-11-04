- **`contrib.host.lua` is removed.** The Lua bridge now lives in
  [aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages)
  (`lua/`, copied from this tree at `d3732a0ed`). Import it with
  `ae add github.com/aether-lang-dev/aether-guest-languages` and then
  `import lua`, in place of `import contrib.host.lua`. It is deleted here with
  no transition period, along with its tests, its example, its `make contrib`
  module (`host_lua`) and the `--with=lua` layer of `aether-build`; other
  bridges will follow it to that repository one at a time (see
  `contrib/host/README.md`).
