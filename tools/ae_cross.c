/* ae_cross.c — cross-compilation via a zig cc backend (#1105).
 *
 * Split out of ae.c (#1221): this is the hottest edit cluster in the
 * driver (#1208/#1216/#1218/#1220 all live here), and as part of the
 * single 8.5k-line TU every edit recompiled all of it. Code moved
 * verbatim; the three entry points cmd_build uses are declared in
 * ae_internal.h, everything else stays static to this file.
 */

#include "ae_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <process.h>
#  ifndef getpid
#    define getpid _getpid
#  endif
#else
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------ *
 *  Cross-compilation via a zig cc backend (#1105)
 *
 *  `ae build --target=<triple>` builds a foreign-target binary using
 *  zig as a self-contained cross-compiler: zig bundles each target's
 *  libc, system headers and linker, so the Aether runtime and stdlib
 *  compile straight from source for the target. The platform backend
 *  (epoll vs kqueue, spawn_sandboxed_linux vs bsd) is chosen by the
 *  compile-time __linux__ / __APPLE__ macros zig predefines for the
 *  target, so one source set serves every target with no per-host
 *  file selection.
 *
 *  PR 1 scope: dependency-free programs. Stdlib modules whose C code
 *  needs an external library we cannot yet cross-build (openssl,
 *  nghttp2, zlib, pcre2) are left out of the compile set, and a
 *  program importing one is rejected up front with a clear message.
 *  Networking / crypto / compression / regex cross builds are the
 *  documented follow-up. Native builds are entirely unaffected.
 * ------------------------------------------------------------------ */

/* Map an Aether target string to a zig `-target` triple. Returns NULL
 * for anything that isn't a supported cross triple (native / wasm /
 * unknown), which the caller treats as "not a cross build". */
const char* cross_target_to_zig(const char* t) {
    if (!t) return NULL;
    if (!strcmp(t, "aarch64-macos") || !strcmp(t, "arm64-macos"))  return "aarch64-macos-none";
    if (!strcmp(t, "x86_64-macos")  || !strcmp(t, "amd64-macos"))  return "x86_64-macos-none";
    if (!strcmp(t, "aarch64-linux") || !strcmp(t, "arm64-linux"))  return "aarch64-linux-gnu";
    if (!strcmp(t, "x86_64-linux")  || !strcmp(t, "amd64-linux"))  return "x86_64-linux-gnu";
    /* Windows (Tier A — self-contained): zig bundles the full MinGW-w64 target
     * (CRT, Win32 headers, import libs), so no base sysroot, no --sysroot, no
     * CRT/libc dance — identical to the linux/macos arms. The runtime's _WIN32
     * guards (already exercised by native MSYS2 builds) compile against zig's
     * mingw-w64 bundle. cross_target_needs_sysroot stays false for windows. */
    if (!strcmp(t, "x86_64-windows")  || !strcmp(t, "amd64-windows")) return "x86_64-windows-gnu";
    if (!strcmp(t, "aarch64-windows") || !strcmp(t, "arm64-windows")) return "aarch64-windows-gnu";
    /* FreeBSD (Tier B): Zig 0.16 supplies its CRT/libc, but the remaining
     * system headers and libraries come from the FreeBSD 15 base sysroot.
     * State the ABI version so Zig's startup objects match that base. */
    if (!strcmp(t, "aarch64-freebsd") || !strcmp(t, "arm64-freebsd")) return "aarch64-freebsd.15.0";
    if (!strcmp(t, "x86_64-freebsd")  || !strcmp(t, "amd64-freebsd")) return "x86_64-freebsd.15.0";
    return NULL;
}

/* True if `t` is a cross target that needs a base sysroot for system headers
 * and platform libraries (via AETHER_SYSROOT). Tier A (macos/linux) is
 * self-contained; Tier B (freebsd) is not. */
static bool cross_target_needs_sysroot(const char* t) {
    if (!t) return false;
    return strstr(t, "freebsd") != NULL;
}

/* Locate the authoritative source MANIFEST and the base directory its
 * entries are relative to. Dev tree: <root>/build/MANIFEST (entries
 * relative to <root>). Installed: <root>/share/aether/MANIFEST
 * (entries relative to <root>/share/aether). Returns false if neither
 * exists. */
static bool cross_find_manifest(char* manifest, size_t msz,
                                char* base, size_t bsz) {
    snprintf(manifest, msz, "%s/build/MANIFEST", tc.root);
    if (path_exists(manifest)) { snprintf(base, bsz, "%s", tc.root); return true; }
    snprintf(manifest, msz, "%s/share/aether/MANIFEST", tc.root);
    if (path_exists(manifest)) {
        /* tc.root is PATH_MAX-ish and bsz is the same size, so the suffix can
           push the result past the destination. Bound the root explicitly so
           the composed path provably fits rather than silently truncating. */
        int n = snprintf(base, bsz, "%.*s/share/aether",
                         (int)(bsz - sizeof("/share/aether")), tc.root);
        if (n < 0 || (size_t)n >= bsz) { return false; }
        return true;
    }
    return false;
}

/* Read MANIFEST into `out`, one absolute source path per entry, for
 * every link-suitable source. Returns the count (capped at `max`), or
 * -1 on error (unreadable manifest). `zig cc -c` emits only one object
 * when handed several sources at once, so the caller compiles each path
 * individually.
 *
 * Every runtime/stdlib source compiles for a cross target with no
 * external library: each openssl / nghttp2 / zlib / pcre2 dependency is
 * behind an AETHER_HAS_* guard that falls to a graceful "unavailable"
 * stub when the macro is undefined (which it is here). So we build the
 * full set, archive it, and let the final link pull only the objects the
 * program references, exactly as a native `-laether` link against the
 * complete libaether.a does. Library-backed features (TLS, real crypto,
 * zlib, HTTP/2) then report unavailable at runtime on the target,
 * exactly like a native build on a host without those libraries, while
 * pure helpers such as base64 (std.encoding) keep working. std.regex is
 * the exception (#1389): its engine is vendored (std/regex/pcre2/,
 * compiled by the aether_pcre2_vendored.c TU in this same list), so it
 * works on every cross target with no sysroot. */
#define CROSS_SRC_PATH_MAX 1024
static int cross_collect_core_list(char out[][CROSS_SRC_PATH_MAX], int max,
                                   const char* manifest, const char* base) {
    FILE* f = fopen(manifest, "r");
    if (!f) return -1;
    int count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && count < max) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r' ||
                     line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;                 /* comment / blank */
        size_t plen = strlen(p);
        if (plen < 2 || strcmp(p + plen - 2, ".c") != 0) continue;
        int need = snprintf(out[count], CROSS_SRC_PATH_MAX, "%s/%s", base, p);
        if (need < 0 || need >= CROSS_SRC_PATH_MAX) {
            /* A truncated source path would compile the wrong file or
             * fail obscurely at the C compiler; skip it loudly instead. */
            fprintf(stderr, "Warning: source path too long, skipped: %s/%s\n", base, p);
            continue;
        }
        count++;
    }
    fclose(f);
    return count;
}

/* Scan a program's (transitive) imports via `aetherc --emit=inspect`.
 * If it uses a stdlib module with library-backed features that are not
 * cross-built (so they report "unavailable" at runtime on the target),
 * write the module name to `which` and return true. Used to warn, not
 * to block: the program still builds and the non-library parts still
 * work. On inspect failure returns false (no warning). */
bool cross_uses_unsupported_module(const char* file, char* which, size_t wsz) {
    static char out[65536];
    if (aetherc_capture_stdout("--emit=inspect", file, NULL, out, sizeof(out)) != 0)
        return false;
    static const char* mods[] = {
        /* std.regex is NOT here: its engine is vendored (#1389), so cross
         * builds get a working regex with no sysroot — nothing to warn about. */
        "std.http", "std.net", "std.cryptography", "std.zlib",
        "std.encoding", NULL   /* base64 in std.encoding is openssl-backed */
    };
    for (int i = 0; mods[i]; i++) {
        if (strstr(out, mods[i])) { snprintf(which, wsz, "%s", mods[i]); return true; }
    }
    return false;
}

/* Execute a full cross build: compile the dependency-free core to
 * per-file objects, archive them, then link the program against the
 * archive. Linking against an archive (rather than force-linking every
 * runtime object into the image) reproduces a native `-laether` link's
 * on-demand object pulling: an object is pulled only when the program
 * references one of its symbols. That is what lets a user program define
 * a top-level function named like an *unreferenced* runtime global
 * (e.g. `describe`, `notify`, `event` in aether_host.c) without a
 * duplicate-symbol clash, exactly as a native build allows. Returns 0
 * on success, non-zero (with a diagnostic) otherwise. POSIX host only.
 *
 * Feature defines mirror the runtime archive's normal Makefile CFLAGS:
 * AETHER_HAS_SANDBOX is the only one not auto-derived by
 * aether_optimization_config.h (filesystem / networking / threading
 * default on when no AETHER_NO_* is passed). The external-library macros
 * (AETHER_HAS_OPENSSL / _ZLIB / _NGHTTP2) are deliberately left
 * undefined so their stub paths compile, matching the excluded sources.
 * AETHER_HAS_PCRE2 is the exception: it is always on, backed either by a
 * CROSSBUILD_SYSROOT's real libpcre2-8 or by the vendored engine
 * (AETHER_VENDOR_PCRE2) — see the fallback after the sysroot probe. */
/* Grow *buf to hold at least `need` bytes, doubling. Same shape as
 * include_flags_grow in ae.c, and for the same reason: the cross-compile
 * command line scales with the install prefix and the module count, so any
 * fixed size is a limit someone eventually runs into with nothing they can do
 * about it. */
static bool cross_buf_reserve(char** buf, size_t* cap, size_t need) {
    if (need <= *cap) return true;
    size_t next = *cap ? *cap : 8192;
    while (next < need) next *= 2;
    char* bigger = (char*)realloc(*buf, next);
    if (!bigger) return false;
    *buf = bigger;
    *cap = next;
    return true;
}

/* printf into a heap buffer that grows to fit. Returns false only on OOM. */
static bool cross_cmd_fmt(char** buf, size_t* cap, const char* fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(*buf, *cap, fmt, ap);
        va_end(ap);
        if (w < 0) return false;
        if ((size_t)w < *cap) return true;
        if (!cross_buf_reserve(buf, cap, (size_t)w + 1)) return false;
    }
}

int run_cross_build(const char* c_file, const char* out_file,
                           bool optimize, const char* extra,
                           const char* ztriple) {
    char manifest[2048], base[1024];
    if (!cross_find_manifest(manifest, sizeof(manifest), base, sizeof(base))) {
        fprintf(stderr,
            "Error: cross-compilation needs the runtime source MANIFEST, which was "
            "not found (looked under %s/build and %s/share/aether). Run `make stdlib` "
            "in a source tree, or reinstall the toolchain.\n", tc.root, tc.root);
        return 1;
    }
    static char srcs[256][CROSS_SRC_PATH_MAX];
    int n = cross_collect_core_list(srcs, 256, manifest, base);
    if (n <= 0) {
        fprintf(stderr, "Error: could not assemble the cross-compile source set from %s.\n",
                manifest);
        return 1;
    }

    /* Fresh per-build object directory under the system temp. */
    char objdir[1024];
    snprintf(objdir, sizeof(objdir), "%s/ae-cross-%d", get_temp_dir(), (int)getpid());
    mkdirs(objdir);

    const char* user_cflags = get_cflags();
    const char* opt = optimize ? "-O2" : "-O0 -g";
    const char* ex = extra ? extra : "";
    /* std.audio's vendored miniaudio auto-selects a backend by platform macro:
     * on a macos target it #includes <CoreAudio/CoreAudio.h>, an APPLE FRAMEWORK
     * header that zig's bundled macOS SDK stubs deliberately do NOT ship (the
     * Apple-licensed part). A cross-built binary can't use CoreAudio without a
     * real Mac SDK anyway, so disable it: -DMA_NO_COREAUDIO makes miniaudio fall
     * back to its null backend (std.audio reports unavailable at runtime — same
     * warn-and-degrade as openssl/nghttp2 on a Tier-B target). Without this, a
     * macos cross-build of ANY program fails compiling aether_audio.c, even one
     * that never touches std.audio (the runtime always compiles it). */
    /* feature_defs grows below when the CROSSBUILD_SYSROOT probe finds a
     * Tier-2 lib staged: each such lib both LINKS (crossbuild_libs) and
     * COMPILES REAL (its -DAETHER_HAS_* here). Sized for the base defs plus
     * every Tier-2 macro. */
    char feature_defs[2048] = "-DAETHER_HAS_SANDBOX";
    if (strstr(ztriple, "macos")) {
        strncat(feature_defs, " -DMA_NO_COREAUDIO",
                sizeof(feature_defs) - strlen(feature_defs) - 1);
    }
    /* Tier-B (FreeBSD) targets need a base sysroot for headers and libraries;
     * AETHER_SYSROOT points at it (bases/<cpu>-freebsd[ver]/ from
     * aether-crossbuild). Applied to BOTH compile and link. Empty for the
     * self-contained Tier-A targets. NB: for a FreeBSD target, `--sysroot`
     * ALONE does not make zig cc search the sysroot's usr/include and
     * usr/lib (unlike its bundled targets) — the -I/-L must be explicit
     * (verified with the repository-pinned Zig). */
    char sysroot_flag[3200];   /* COMPILE flags: --sysroot + -I/-L */
    char fbsd_link[4096];      /* LINK tail: FreeBSD threading library */
    char fbsd_platform_libs[2048]; /* LINK tail: FreeBSD platform -l names */
    char win_platform_libs[512];   /* LINK tail: Windows system -l names */
    sysroot_flag[0] = '\0';
    fbsd_link[0] = '\0';
    fbsd_platform_libs[0] = '\0';
    win_platform_libs[0] = '\0';
    /* Windows system libs. std.cryptography's OS RNG uses BCryptGenRandom
     * (bcrypt.dll); winsock, crypt32, advapi32 etc. are pulled by the runtime
     * and static openssl. Same shape as fbsd_platform_libs (casper) — a
     * target-specific always-on set the cross link must append (zig bundles the
     * mingw CRT but NOT these -l names). Matches the native Windows
     * win_link_libs. Detected off the zig triple (…-windows-gnu). */
    if (strstr(ztriple, "windows")) {
        snprintf(win_platform_libs, sizeof(win_platform_libs),
                 "-lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -ldbghelp");
    }
    if (cross_target_needs_sysroot(ztriple)) {
        const char* sr = getenv("AETHER_SYSROOT");
        if (!sr || !*sr) {
            fprintf(stderr,
                "Error: target %s needs a FreeBSD base sysroot, but AETHER_SYSROOT is unset.\n"
                "  Provision the FreeBSD system headers and libraries with aether-crossbuild:\n"
                "    ./scripts/fetch-freebsd-base.sh <cpu> [major]   # e.g. x86_64 15\n"
                "  then: AETHER_SYSROOT=<crossbuild>/bases/<cpu>-freebsd[ver] ae build ... --target=%s\n",
                ztriple, ztriple);
            return 1;
        }
        /* Zig 0.16 resolves target-root -L paths beneath --sysroot. Supplying
         * already-prefixed host paths makes it prefix the sysroot twice. Zig
         * now supplies the CRT/libc itself; adding the base startup objects
         * duplicates `_start`. libthr remains an explicit platform dependency
         * of the scheduler and actor runtime. */
        snprintf(sysroot_flag, sizeof(sysroot_flag),
                 "--sysroot=%s -I%s/usr/include -L/usr/lib -L/lib", sr, sr);
        snprintf(fbsd_link, sizeof(fbsd_link), "-lthr");

        /* Platform libs the FreeBSD link needs, mirroring the NATIVE FreeBSD
         * build (ae.c ~2368). The base sysroot's -L (from sysroot_flag) already
         * resolves these; only the -l names were missing on the cross path.
         *
         * casper — ALWAYS: std/casper/aether_casper.c is unconditionally in
         * libaether.a and calls cap_getpwnam / cap_sysctlbyname / cap_getaddrinfo
         * / ..., so a FreeBSD cross-link fails with `undefined symbol: cap_*`
         * regardless of what the app imports. The base ships all of them. The
         * complete set, per the cap_* symbols aether_casper.c references:
         *   cap_init/cap_close/cap_service_open -> libcasper (core)
         *   cap_getpwnam                        -> libcap_pwd
         *   cap_sysctl(byname)                  -> libcap_sysctl
         *   (grp)                               -> libcap_grp
         *   cap_getaddrinfo (aether_casper_resolve, DNS) -> libcap_dns
         * cap_dns was the one #1216 missed (its DNS path only surfaces after
         * pwd/sysctl resolve). We emit the names literally rather than via
         * AETHER_CASPER_LIBS — that macro is populated by globbing the HOST's
         * /lib when `ae` is built on FreeBSD, and is empty in an `ae`
         * cross-compiled/built on Linux.
         *
         * casper is FreeBSD-only. The openssl/nghttp2/zlib/pcre2 (Tier-2) libs
         * are target-AGNOSTIC and handled by crossbuild_libs below, so they
         * work for windows/linux/macos too — not just freebsd. */
        snprintf(fbsd_platform_libs, sizeof(fbsd_platform_libs),
                 "-lcasper -lcap_pwd -lcap_sysctl -lcap_grp -lcap_dns");
    }

    /* Tier-2 libs from a CROSSBUILD_SYSROOT — CONDITIONAL and TARGET-AGNOSTIC.
     * openssl/nghttp2/zlib/pcre2 are not bundled by zig for any target;
     * aether-crossbuild's provision.sh <triple> builds them into
     * sysroots/<triple>/. When that sysroot is provided (same CROSSBUILD_SYSROOT
     * contract #1213 gave contrib_build.sh), append its -L + the -l names so
     * std.cryptography / std.http / std.zlib / std.regex link for real — for
     * FreeBSD AND Windows AND linux/macos. Without it, warn-and-omit stands
     * (the features report unavailable at runtime). The -l names are the same
     * across targets; only the -L (the sysroot) differs. */
    char crossbuild_libs[2048];
    crossbuild_libs[0] = '\0';
    {
        const char* xsr = getenv("CROSSBUILD_SYSROOT");
        if (xsr && *xsr) {
            /* Append each -l ONLY when that lib is actually staged in the
             * sysroot. provision.sh may build a subset (a target might have
             * pcre2 + zlib but not openssl yet), and zig hard-errors on a
             * requested-but-absent lib. Probing per-lib links exactly what's
             * provisioned — the same "link what's there" discipline #1213's
             * contrib cross mode uses. openssl is -lssl + -lcrypto (both from
             * libssl.a/libcrypto.a); the rest are 1:1. */
            size_t p = 0;
            p += (size_t)snprintf(crossbuild_libs + p, sizeof(crossbuild_libs) - p,
                                  "-L%s/lib", xsr);
            /* The sysroot's headers (openssl/, zlib.h, pcre2.h, ...) must be on
             * the COMPILE include path too, else enabling a real path below
             * (-DAETHER_HAS_OPENSSL etc.) hits `openssl/ssl.h file not found`.
             * Added once, unconditionally when a CROSSBUILD_SYSROOT is set —
             * harmless when a probed lib is absent (nothing includes its
             * header) and required when present. */
            strncat(feature_defs, " -I",
                    sizeof(feature_defs) - strlen(feature_defs) - 1);
            strncat(feature_defs, xsr,
                    sizeof(feature_defs) - strlen(feature_defs) - 1);
            strncat(feature_defs, "/include",
                    sizeof(feature_defs) - strlen(feature_defs) - 1);
            char probe[2600];
            /* `defines` is the -D that makes the corresponding stdlib source
             * compile its REAL path instead of the "unavailable" stub. It MUST
             * accompany the -l: linking libcrypto.a without -DAETHER_HAS_OPENSSL
             * leaves the stub compiled in (the -l references nothing) — which
             * is exactly the bug where a cross-built agent's hmac_sha256_hex
             * returned "" despite a staged sysroot. Empty for libs with no
             * compile-time guard (contrib veneers link but have no AETHER_HAS_*
             * macro of their own). */
            struct { const char* lib; const char* names; const char* defines; } t2[] = {
                /* libssl.a present => both -l names AND the openssl compile
                 * macro. HMAC/SHA (std.cryptography) need only libcrypto, but
                 * the source guard is a single AETHER_HAS_OPENSSL, so staging
                 * libssl.a (which the sysroot pairs with libcrypto.a) enables
                 * the real crypto path; TLS (std.http) additionally needs the
                 * libssl symbols, which the same -lssl provides. */
                { "ssl",     "-lssl -lcrypto", "-DAETHER_HAS_OPENSSL" },
                { "nghttp2", "-lnghttp2",      "-DAETHER_HAS_NGHTTP2" },
                { "z",       "-lz",            "-DAETHER_HAS_ZLIB" },
                { "pcre2-8", "-lpcre2-8",      "-DAETHER_HAS_PCRE2" },
                /* contrib.sqlite: the Aether veneer archive
                 * (libaether_sqlite.a, from contrib_build.sh CONTRIB_TARGET
                 * mode) BEFORE the underlying C lib (libsqlite3.a) — ld.lld's
                 * single pass needs the veneer's sqlite3_* references resolved
                 * by the lib that follows. Probed on the VENEER so a program
                 * that doesn't use sqlite links nothing extra. */
                { "aether_sqlite", "-laether_sqlite -lsqlite3", "" },
                /* contrib.host.python: the embedded-Python bridge veneer. NO
                 * -lpython — the bridge dlopen()s the deploy host's libpython
                 * at runtime (AETHER_PYTHON_SONAME), so the .a has no
                 * unresolved CPython symbols and needs no python at link. Just
                 * the veneer archive. Probed on it so a program not embedding
                 * python links nothing extra. */
                { "aether_host_python", "-laether_host_python", "" },
                /* contrib.host.ruby: same dlopen model as python (no -lruby;
                 * the deploy host's libruby is dlopen'd at runtime). Veneer
                 * only. */
                { "aether_host_ruby", "-laether_host_ruby", "" },
            };
            for (size_t i = 0; i < sizeof(t2) / sizeof(t2[0]); i++) {
                snprintf(probe, sizeof(probe), "%s/lib/lib%s.a", xsr, t2[i].lib);
                if (path_exists(probe) && p < sizeof(crossbuild_libs)) {
                    p += (size_t)snprintf(crossbuild_libs + p, sizeof(crossbuild_libs) - p,
                                          " %s", t2[i].names);
                    /* Also compile the matching stdlib source's REAL path, not
                     * its stub — the fix for the "linked but stubbed" bug. */
                    if (t2[i].defines[0]) {
                        strncat(feature_defs, " ",
                                sizeof(feature_defs) - strlen(feature_defs) - 1);
                        strncat(feature_defs, t2[i].defines,
                                sizeof(feature_defs) - strlen(feature_defs) - 1);
                    }
                }
            }
        }
    }
    /* std.regex needs no sysroot (#1389): when nothing above staged a real
     * libpcre2-8 (no CROSSBUILD_SYSROOT, or one without pcre2), compile the
     * vendored engine instead. AETHER_VENDOR_PCRE2 turns
     * std/regex/aether_pcre2_vendored.c — already in the MANIFEST compile
     * loop below — from an empty TU into the full unity build, and switches
     * aether_regex.c to the in-tree pcre2.h; both are self-contained
     * (relative includes), so no -I and no -l is needed for any target.
     * A sysroot-staged pcre2 keeps precedence: its define is already in
     * feature_defs and its -lpcre2-8 in crossbuild_libs, and adding the
     * vendored engine on top would compile two copies of the same symbols. */
    if (!strstr(feature_defs, "-DAETHER_HAS_PCRE2")) {
        strncat(feature_defs, " -DAETHER_HAS_PCRE2 -DAETHER_VENDOR_PCRE2",
                sizeof(feature_defs) - strlen(feature_defs) - 1);
    }
    /* Heap-grown rather than fixed: the command line is dominated by
     * tc.include_flags, which is itself heap-grown and scales with the install
     * prefix length and the module count. A fixed buffer turned a longer-than-
     * usual prefix into "cross-compile command exceeded the N-byte buffer",
     * with no way for the user to shorten anything that mattered. */
    char* cmd = NULL;
    size_t cmd_cap = 0;
    /* Accumulated quoted "<objpath>" list, in compile order, for the ar
     * step. posix_run tokenizes the command itself (no shell), so the
     * archive must name each object explicitly rather than glob. Also grows:
     * it holds one quoted path per runtime object. */
    char* objlist = NULL;
    size_t objlist_cap = 0;
    size_t obj_pos = 0;
    if (!cross_buf_reserve(&objlist, &objlist_cap, 1)) {
        fprintf(stderr, "Error: out of memory building the cross-compile command.\n");
        return 1;
    }
    objlist[0] = '\0';
    int rc = 1;

    int w;
    do {
        /* 1. Compile each core source to its own object in objdir. zig cc
         *    -c emits only one object when handed several sources at once,
         *    so compile one at a time (all core basenames are unique). */
        bool compile_failed = false;
        for (int i = 0; i < n; i++) {
            const char* bn = strrchr(srcs[i], '/');
            bn = bn ? bn + 1 : srcs[i];
            char objpath[2048];
            /* basename with its trailing ".c" replaced by ".o" */
            snprintf(objpath, sizeof(objpath), "%s/%.*so", objdir,
                     (int)(strlen(bn) - 1), bn);
            if (!cross_cmd_fmt(&cmd, &cmd_cap,
                "zig cc -target %s %s %s %s %s %s -c \"%s\" -o \"%s\"",
                ztriple, sysroot_flag, opt, feature_defs, user_cflags, tc.include_flags,
                srcs[i], objpath)) {
                fprintf(stderr, "Error: out of memory building the cross-compile command.\n");
                compile_failed = true;
                break;
            }
            if (run_cmd_show_warnings(cmd) != 0) {
                fprintf(stderr, "Error: cross-compiling %s for %s failed.\n", srcs[i], ztriple);
                compile_failed = true;
                break;
            }
            size_t need = obj_pos + strlen(objpath) + 4;
            if (!cross_buf_reserve(&objlist, &objlist_cap, need)) {
                fprintf(stderr, "Error: out of memory building the cross-compile object list.\n");
                compile_failed = true;
                break;
            }
            int ow = snprintf(objlist + obj_pos, objlist_cap - obj_pos,
                              "%s\"%s\"", obj_pos ? " " : "", objpath);
            if (ow < 0) { compile_failed = true; break; }
            obj_pos += (size_t)ow;
        }
        if (compile_failed) break;

        /* 2. Archive the objects (named explicitly, no glob) so the final
         *    link pulls only what the program references (native
         *    `-laether` semantics). */
        if (!cross_cmd_fmt(&cmd, &cmd_cap,
            "zig ar rcs \"%s/libaether.a\" %s", objdir, objlist)) {
            fprintf(stderr, "Error: out of memory building the cross-compile archive command.\n");
            break;
        }
        if (run_cmd_show_warnings(cmd) != 0) {
            fprintf(stderr, "Error: archiving the cross runtime failed.\n");
            break;
        }

        /* Clear any stale output so the FreeBSD "output exists == linked"
         * success signal below can't be fooled by a prior build's binary. */
        remove(out_file);

        /* 3. Link the program against the archive. FreeBSD adds libthr and its
         *    platform libraries from the base sysroot. */
        if (fbsd_link[0]) {
            /* Platform -l names go AFTER libaether.a — it references their
             * symbols (casper's cap_*, openssl's SSL_*, …), so they must
             * follow it on the link line for ld.lld's single-pass resolution. */
            w = cross_cmd_fmt(&cmd, &cmd_cap,
                "zig cc -target %s %s %s %s %s %s \"%s\" %s \"%s/libaether.a\" %s %s -lm -o \"%s\"",
                ztriple, sysroot_flag, fbsd_link, opt, feature_defs, tc.include_flags,
                c_file, ex, objdir, fbsd_platform_libs, crossbuild_libs, out_file) ? 1 : -1;
        } else {
            /* Tier A (linux/macos/windows): compact form + any CROSSBUILD_SYSROOT
             * Tier-2 libs AND the Windows system libs after libaether.a (it
             * references their symbols — BCryptGenRandom etc.). */
            w = cross_cmd_fmt(&cmd, &cmd_cap,
                "zig cc -target %s %s %s %s %s \"%s\" %s \"%s/libaether.a\" %s %s -o \"%s\" -lm",
                ztriple, sysroot_flag, opt, feature_defs, tc.include_flags, c_file, ex, objdir, crossbuild_libs, win_platform_libs, out_file) ? 1 : -1;
        }
        if (w < 0) {
            fprintf(stderr, "Error: out of memory building the cross-compile link command.\n");
            break;
        }
        if (run_cmd_show_warnings(cmd) != 0) {
            fprintf(stderr, "Error: cross-linking for %s failed.\n", ztriple);
            break;
        }
        rc = 0;
    } while (0);

    free(cmd);
    free(objlist);

    /* Best-effort removal of the temp object tree. A failure here does not
       affect the build result, but the status must be consumed: glibc marks
       system() warn_unused_result, and gcc does not accept a (void) cast as
       suppression the way clang does. */
    char rmcmd[1100];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf \"%s\"", objdir);
    if (system(rmcmd) != 0) {
        fprintf(stderr, "Warning: could not remove temporary object dir %s\n", objdir);
    }
    return rc;
}
