- **`contrib.host.python` is removed.** The Python bridge now lives in
  [aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages)
  (`python/`, copied from this tree at `9673641e6`). Import it with
  `ae add github.com/aether-lang-dev/aether-guest-languages` and then
  `import python`, in place of `import contrib.host.python`. It is deleted
  here with no transition period, along with its tests
  (`host_python_bidirectional`, `host_python_map_context`), its example, its
  `make contrib` module (`host_python`) and the `--with=python` layer of
  `aether-build`; other bridges will follow it to that repository one at a
  time (see `contrib/host/README.md`).
