- **`AETHER_VULKAN_LOADER` names the Vulkan loader `contrib.vulkan` opens.**
  On a machine with more than one loader, the one the platform search finds
  first need not be the one a driver was installed with: on Windows the DLL
  search takes `System32` before `PATH`, so a loader beside MSYS2's or the
  Vulkan SDK's driver loses to whatever `System32` holds. Set to a path, the
  module opens exactly that loader, and reports it if it does not open rather
  than falling back to another.
