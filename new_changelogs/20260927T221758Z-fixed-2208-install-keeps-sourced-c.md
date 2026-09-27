- **An installed toolchain builds `contrib.vulkan`, `contrib.vulkan.vk`,
  `contrib.d3d12` and `contrib.metal` again.** `install.sh` and the Makefile's
  install targets deleted every `.c` under `share/aether/contrib` except the
  host bridges, and those four modules compile theirs into the program with
  `@source` rather than linking an archive, so the installed module named a
  file that was not there (E0100). The trim now keeps every file a `module.ae`
  in the tree names with `@source`.
