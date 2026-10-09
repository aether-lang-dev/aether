- **`win_batch_compiler_long_cmdline` no longer lists the network root.**
  Its `--extra` paths are past 260 characters, so `cygpath` writes them in
  the long-path form `//?/C:/...`, and the unquoted expansion that splits
  them into words also globbed each one: the `?` made the shell list `//`,
  about three seconds a path, some 80 s before each of its two builds.
  Pathname expansion is off for those words, which never matched anything;
  the test took 166 s on Windows and takes 7 to 12 s (#2596).
- **`message_trace` carries the `manifest_srcs_long_path` check, which now
  builds from a long root.** That test ran `build/ae` with `AETHER_HOME`
  naming a link to the tree, but `ae` takes its root from where its own
  binary sits, so it built from the tree's short path every time, and on
  Windows `ln -s` first copied the whole tree. The traced build now runs
  from copies of `ae` and `aetherc` under the long directory, with the
  source trees linked in (junctions on Windows), checks that the build
  resolved that root, and serves both checks, so the sweep compiles the
  runtime from source once instead of twice: the two tests took 100 to
  134 s on Windows, the one now takes 55 to 62 s (#2596).
- **`fmt_gate` starts fewer processes.** The idempotence tier copies its
  sample with one `tar` and formats it by name, and the IR tier compares a
  pair of generated C files with one `awk` instead of checksumming each
  through four processes: thirteen processes a sampled file became six.
  In back-to-back runs on Windows, 67, 84 and 109 s became 33, 69 and
  41 s (#2596).
- **An install builds the compiled module artifacts in one `aetherc`.**
  `aetherc --emit=aea` takes any number of `<module.ae> <out.aea>` pairs,
  and `scripts/build_module_artifacts.sh` hands it the std tree: the 157
  artifacts took 37 s one process each and take 1.3 s, byte for byte the
  same (`aea_artifacts` checks a batch's artifacts against ones written
  alone). `install.sh` also copies the headers with one `tar` per tree
  instead of three processes a header, and `trim_contrib_sources.sh` reads
  only the `.ae` files that say `@source` and matches the rest in the
  shell. An install took 41 s on Windows and takes 9 s, the installed tree
  byte-identical, and every test that installs gains: `install_manifest`
  went from 131 s to 37 s and `aea_artifacts` from 25 s to 7 s (#2596).
- **`install_contrib_resolves` reads only the `.ae` files that say
  `@source`.** Collecting the `@source`d C files ran a `dirname` and a `sed`
  for each of the 121 contrib `.ae` files, twice. With the faster install
  the test went from 97 s to 15 s on Windows (#2596).
- **`flat_lib_fallback` installs the tree the sweep built.** It ran
  `install.sh` without `AETHER_INSTALL_NO_BUILD=1`, so every run invoked
  `make` four times over the shared tree, built the language server and
  fetched git tags. With the faster install the test went from 132 s to
  22 s on Windows (#2596).
