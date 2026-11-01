- **`abi_policy` checks that a catalog import leaves no stub directory** (#3485).
  Each of its hand-made catalogs (another major, an unknown duty, a newer minor,
  another runtime ABI, a legacy shared runtime) is imported with a temp dir of
  its own, and a leftover `ae-binimport-*` fails the test, as
  `binary_import_ownership` already does for the ownership refusals.
