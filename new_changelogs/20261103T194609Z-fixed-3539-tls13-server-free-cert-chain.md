- **`tls13_server.free_cert_chain(chain, lens)` frees a loaded certificate
  chain.** `parse_cert_chain_pem` hands its caller a list of DER buffers and
  a second list, but the function that frees them was private to the module,
  so a program that loaded a chain for `accept` had no way to give it back
  and leaked it.
