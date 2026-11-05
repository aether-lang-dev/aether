- **The language bridges are removed from `contrib/host`.** `contrib.host.duktape`
  (and its alias `contrib.host.js`), `quickjs`, `perl`, `ruby`, `tcl`, `go`,
  `tinygo`, `java`, `factor`, `racket` and `rhombus` now live in
  [aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages),
  where Lua and Python already went, each copied from this tree at
  `2609ec841` (see its `PROVENANCE.md`) and held to stunt's catalogue by
  name. Add it with `ae add github.com/aether-lang-dev/aether-guest-languages`
  and then write `import tcl` in place of `import contrib.host.tcl` (and so
  on for each language). They are deleted here with no transition period,
  along with their tests (`host_duktape_bidirectional`,
  `host_tcl_bidirectional`, `host_racket`, `host_rhombus`, `host_tinygo`,
  `tests/sandbox/test_shared_map_all.sh`), the `04-interop/05-hosts`
  examples, their `make contrib` modules, the language demos of
  `make contrib-host-check`, `ae`'s Racket and TinyGo link special cases and
  the `js`/`rhombus` import aliases, the `--with=<lang>` layers of
  `tools/docker/aether-build` (it now refuses `--with=`) and the `WITH_*`
  build args of `tools/docker/Dockerfile`. `contrib.host.stunt` (the test
  double) and `contrib.host.aether` (Aether hosting Aether) stay, as does
  `contrib.quickjs`, the engine the QuickJS bridge imports.
