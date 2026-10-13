- **`ae build --namespace` takes the manifest whole, and refuses what
  `aether_describe()` cannot hold.** The SDK generator copied the
  manifest's namespace, module, class, input and event names into 128-byte
  fields and its inputs and events into tables of 64, and built class,
  setter and handler names in 128 and 160 bytes; the library's name and
  paths were cut at 256, 512 and 1024 bytes. They are built whole now, and
  a namespace declaring more than the 64 inputs or events the
  `aether_describe()` layout holds is refused by aetherc with the count,
  where the rest were dropped without a word (#2888).
