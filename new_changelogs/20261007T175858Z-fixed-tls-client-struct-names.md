- **Importing the pure TLS client no longer takes `Pt`, `Fe`, `Params` or
  `ReaderView` from the program.** Struct names are one namespace across
  modules, and the TLS client's import closure declared those common names
  internally (`std.cryptography.p384` and `.p521`: `Pt`; `.x25519`: `Fe`;
  `.mlkem`: `Params`; `.tls13_cert`: `ReaderView`), so a program with its own
  struct of one of those names and another layout failed to compile once it
  added `import std.cryptography.tls13_client` ("struct 'Pt' is defined
  differently in two modules") — sae, whose vector graphics have a 2-D `Pt`,
  could not get HTTPS on Android. They are now `P384Pt`, `P521Pt`, `X25519Fe`,
  `MlkemParams` and `CertReaderView`; none was exported
  (tests/regression/test_tls_client_struct_names_stay_private.ae).
