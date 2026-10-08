- **`ae` on macOS finds Homebrew's headers and libraries.** Apple's clang
  searches /usr/local but not /opt/homebrew, where Homebrew lives on Apple
  Silicon, so `contrib.vulkan` (and `tests/integration/vulkan_vkgen`) failed
  with `'vulkan/vulkan.h' file not found` on a Mac with Homebrew's
  vulkan-headers / MoltenVK installed, unless every program added the path
  itself. A native macOS build now adds `HOMEBREW_PREFIX` (else /opt/homebrew)
  with `-idirafter <prefix>/include`, searched after the SDK's and aether's own
  headers so a formula can fill a gap but never shadow them, and
  `-L<prefix>/lib` last on the link line (tests/integration/macos_homebrew_paths).
