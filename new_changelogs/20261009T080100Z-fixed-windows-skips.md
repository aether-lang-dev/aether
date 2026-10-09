- **`std.http.script_gateway` mounts a script on Windows.** It was a stub
  there that answered every mount with `KIND_UNAVAILABLE`. It now loads the
  script DLL with `LoadLibrary` and calls its `aether_script_handle`, as the
  POSIX build does with `dlopen`. Windows has no `-rdynamic` for a script to
  bind its runtime calls to the host's runtime, so the host and the script
  are both built with `ae build --shared-runtime` and share `aether.dll`. A
  host or a script on its own static runtime is refused at mount with
  `KIND_IO` and a message naming the flag. Otherwise the two would each keep
  their own caps and config while handing each other heap objects (#2547).
- **A Windows DLL that `ae` builds exports its `@c_callback` functions.** A
  `@c_callback` definition is emitted weak, so two translation units carrying
  one module can share a link, and PE cannot export a weak definition. So a
  C host's `GetProcAddress` never found a `@c_callback` in an `--emit=lib`
  DLL, which is the one thing the annotation is for. The script gateway's
  `aether_script_handle` was the first to need it. A DLL `ae` links is one
  translation unit, so its `@c_callback` definitions are now strong, as
  `aether_lib_meta` already was (#2547).
- **The HTTP server's refusals reach a Windows client.** A 413, 414, 431 or
  framing 400 goes out while the client may still be sending the rest of the
  request, and closing with those bytes unread makes the kernel reset the
  connection. A Windows client discards what it had received but not read on
  a reset, so an upload refused with 413 arrived there as "connection reset"
  about half the time. The server now closes in stages, as RFC 9112 9.6 asks:
  it half-closes after the refusal and reads off what the client still sends,
  for at most two seconds, before it closes (#2547).
- **Twenty-six integration tests that skipped on Windows run there (#2547).**
  These are the namespace round trips (C, Python, Ruby and Java hosts), the
  raw-socket HTTP client and server probes (`http_client_bad_status` and
  `http_client_dechunk` among them), `http_server_background_quiet`,
  `std_testing_arms`, `http_script_gateway`, `emit_csrc`, `emit_lib_swig`,
  `wasm_installed_prefix_paths`, `notify`, `liquid_sandbox_gate` and
  `manifest_srcs_long_path`. The raw-socket fixtures share
  `tests/lib/raw_socket.h`, and `tests/lib/raw_exchange.c` replaces `nc`,
  which neither a Windows nor every Linux runner has. A test that needs Ruby,
  a JDK 22 or SWIG still skips where that tool is missing.
  `manifest_srcs_long_path` had built from the short root on every platform:
  `ae` takes its root from its own path, which `AETHER_HOME` does not
  override, and a cached binary was served anyway. Its check now runs `ae`
  from the long path with a fresh cache, checks the root it built from, and
  lives in `message_trace`, whose traced build it shares (#2596).
