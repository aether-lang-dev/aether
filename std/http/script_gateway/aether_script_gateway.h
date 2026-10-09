#ifndef AETHER_SCRIPT_GATEWAY_H
#define AETHER_SCRIPT_GATEWAY_H

/* In-process script gateway (issue #384).
 *
 * Mounts a pre-compiled `aetherc --emit=lib script.ae -o script.so`
 * shared library as an HTTP request handler — gives `.ae` scripts
 * the dispatch ergonomics of CGI without paying the per-request
 * fork/exec cost. The script's exported `aether_script_handle(req,
 * res, ud)` symbol is resolved once at mount time via dlsym(3) and
 * called directly per request from the connection thread.
 *
 * The .so is dlopen()ed with RTLD_NOW so any unresolved symbol
 * fails fast at mount time rather than at the first request, and
 * with RTLD_LOCAL so symbols don't pollute the host process's
 * global namespace.
 *
 * On Windows the library is a DLL loaded with LoadLibrary, and the host
 * and the script must both be built with `ae build --shared-runtime`:
 * there is no -rdynamic there for a script to bind to the host's runtime,
 * so they share aether.dll instead. A mismatch is refused at mount with
 * KIND_IO (sg_host_runtime_check and sg_script_runtime_check in the .c).
 *
 * Lifetime: dlopen handles are intentionally not dlclose()d. The
 * gateway is a long-lived hosting feature; the .so stays mapped
 * until the host process exits. Hot-reload is a separate feature
 * outside the scope of #384.
 *
 * Performance: per-request cost is one prefix-strncmp + one
 * indirect call. On Linux/glibc the indirect call goes through the
 * dlopened SO's PLT once and is then jit-bound; subsequent calls
 * are direct. Empirically this is ~50× faster than spawning a
 * subprocess per request — the entire point of the feature.
 *
 * The structured-error tuple shape uses the same KIND_* constants
 * as std.fs (issue #392). KIND values returned here are a subset
 * of std.fs's: KIND_OK, KIND_NOT_FOUND, KIND_INVALID, KIND_IO,
 * KIND_UNAVAILABLE.
 */

#define AETHER_SCRIPT_GATEWAY_KIND_OK            0
#define AETHER_SCRIPT_GATEWAY_KIND_NOT_FOUND     1  /* .so file missing */
#define AETHER_SCRIPT_GATEWAY_KIND_INVALID       6  /* bad args / missing entrypoint */
#define AETHER_SCRIPT_GATEWAY_KIND_IO            5  /* load / mount failure */
#define AETHER_SCRIPT_GATEWAY_KIND_UNAVAILABLE  99  /* no dynamic loader (unused today) */

/* All public C functions return the tuple ABI shape; declarations
 * live in the .c only since they're invoked exclusively from
 * generated Aether code (matches the fs_copy_raw pattern). */

#endif
