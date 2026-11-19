/* ae_zig_pin.h: the Zig release `ae toolchain zig` installs, and the sha256
 * of its archive for every host Aether ships for (#3623).
 *
 * This file is the one place the pin lives. tools/ae_toolchain.c compiles it
 * into `ae`, and scripts/get-zig.sh reads the version and the x86_64 Linux
 * hash out of it with sed when no `ae` is at hand, so keep each row on one
 * line in the shape below.
 *
 * Zig 0.16 is the first release whose WASI libc has the setjmp headers
 * runtime/actors/aether_panic.c needs.
 *
 * The hashes are copied from https://ziglang.org/download/index.json (the
 * "0.16.0" entry, fetched 2026-11-19). The x86_64 Linux one matched the hash
 * scripts/get-zig.sh pinned before this file existed. They are never fetched
 * at run time: a hash that came over the same network as the archive would
 * verify nothing.
 *
 * Each row names the host as Zig spells it in its archive names
 * (zig-<host>-<version><ext>). FreeBSD x86_64 and arm64 are published for
 * 0.16.0 and are listed. */
#ifndef AE_ZIG_PIN_H
#define AE_ZIG_PIN_H

#define AE_ZIG_VERSION "0.16.0"

typedef struct {
    const char* host;    /* "x86_64-linux": Zig's <arch>-<os> */
    const char* ext;     /* ".tar.xz" or ".zip" */
    const char* sha256;  /* lowercase hex of the archive */
} AeZigPin;

static const AeZigPin ae_zig_pins[] = {
    { "x86_64-linux", ".tar.xz", "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00" },
    { "aarch64-linux", ".tar.xz", "ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17" },
    { "x86_64-macos", ".tar.xz", "0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7" },
    { "aarch64-macos", ".tar.xz", "b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489" },
    { "x86_64-windows", ".zip", "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e" },
    { "aarch64-windows", ".zip", "aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef" },
    { "x86_64-freebsd", ".tar.xz", "390379bfaf6b89b001c6bb4035b8fcbc5c12193fd9fd5048c3a0fe39e5d1cf72" },
    { "aarch64-freebsd", ".tar.xz", "0a441f50696a34cfb9f9a0b3c510fd49a56b18b364e03c26525cb7999959c969" },
};

#endif /* AE_ZIG_PIN_H */
