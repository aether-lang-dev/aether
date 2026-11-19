/* ae_toolchain.c: `ae toolchain zig` (#3623).
 *
 * `ae build --target=<triple>` cross-compiles with `zig cc`, and the Zig it
 * needs is pinned (tools/ae_zig_pin.h). This command fetches that Zig for the
 * host, checks the archive against the sha256 compiled into `ae`, and installs
 * it under <AETHER_HOME or ~/.aether>/toolchains/zig-<version>/, the same
 * place for every Aether version that pins that Zig. It does what
 * scripts/get-zig.sh does, on every host Aether ships for, without a source
 * checkout.
 *
 * The order of the steps is the point of the command:
 *   1. the archive is downloaded into a staging directory created fresh
 *      (mkdtemp, or an exclusive mkdir on Windows) inside the toolchains
 *      directory, so no other user and no earlier run can have put a file at
 *      that path first;
 *   2. its sha256 is checked against the compiled-in one before anything
 *      reads the archive; on a mismatch the file is deleted and both hashes
 *      are printed;
 *   3. every entry name is checked to lie under the one top directory the
 *      archive is expected to have, with no absolute path and no `..`;
 *   4. it is unpacked inside the staging directory, and only the finished
 *      tree is renamed into place, so a half-unpacked tree is never at the
 *      path that is taken as installed;
 *   5. `zig version` of the result is checked before its path is printed.
 *
 * A stamp file in the install records the archive and hash it came from;
 * `--check` requires it, so a tree that this command did not finish, or one
 * from another host's archive, is not reported as good. */

#include "ae_internal.h"
#include "ae_sha256.h"
#include "ae_zig_pin.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <process.h>
#  ifndef getpid
#    define getpid _getpid
#  endif
#  define AE_TC_EXE ".exe"
#else
#  include <unistd.h>
#  define AE_TC_EXE ""
#endif

#define AE_TC_PATH 2048

/* Where Zig publishes its releases. AE_ZIG_BASE_URL replaces it, for a
 * mirror or for a test that serves a file:// directory; the archive found
 * there is checked against the same compiled-in hash, so the variable moves
 * only where the bytes come from, never what they must be. */
#define AE_ZIG_DEFAULT_BASE "https://ziglang.org/download"

/* The name of the stamp file inside an install. */
#define AE_ZIG_STAMP ".ae-toolchain"

/* A staging directory left by a run that was killed is removed by a later
 * run once it is this old, so that no run in progress loses its own. */
#define AE_ZIG_STALE_STAGE_SECONDS (24 * 60 * 60)

void ae_toolchain_usage(FILE* out) {
    fprintf(out,
        "Usage: ae toolchain zig [--check]\n"
        "\n"
        "  Install the Zig release `ae build --target=<triple>` is pinned to (%s)\n"
        "  for this host, and print the absolute path of its `zig` as the last line\n"
        "  on stdout, so a script can run ZIG=\"$(ae toolchain zig)\".\n"
        "\n"
        "  The archive comes from https://ziglang.org/download/%s/ and its sha256\n"
        "  must match the one compiled into ae, or it is deleted unused. It is\n"
        "  installed in $AETHER_HOME/toolchains/zig-%s/ when AETHER_HOME is set,\n"
        "  else in ~/.aether/toolchains/zig-%s/. An install that is already there\n"
        "  and reports the right `zig version` is used as it is.\n"
        "\n"
        "Options:\n"
        "  --check   Verify the install and download nothing: exit 1 when it is\n"
        "            absent, reports another version, or was not made from this\n"
        "            host's pinned archive.\n"
        "\n"
        "Environment:\n"
        "  AE_ZIG_BASE_URL   Fetch from <url>/<version>/<archive> instead of\n"
        "                    ziglang.org (a mirror). The hash check is the same.\n",
        AE_ZIG_VERSION, AE_ZIG_VERSION, AE_ZIG_VERSION, AE_ZIG_VERSION);
}

/* The pin for the host this ae was built for, or NULL. */
static const AeZigPin* zig_host_pin(char* host, size_t host_cap) {
    const char* arch = NULL;
    const char* os = NULL;
#if defined(__x86_64__) || defined(_M_X64)
    arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    arch = "aarch64";
#endif
#if defined(_WIN32)
    os = "windows";
#elif defined(__APPLE__)
    os = "macos";
#elif defined(__linux__)
    os = "linux";
#elif defined(__FreeBSD__)
    os = "freebsd";
#endif
    snprintf(host, host_cap, "%s-%s", arch ? arch : "unknown", os ? os : "unknown");
    if (!arch || !os) return NULL;
    for (size_t i = 0; i < sizeof(ae_zig_pins) / sizeof(ae_zig_pins[0]); i++) {
        if (strcmp(ae_zig_pins[i].host, host) == 0) return &ae_zig_pins[i];
    }
    return NULL;
}

/* snprintf that says whether the whole string fit: 0, or -1 when it was cut. */
static int tc_fmt(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return (n >= 0 && (size_t)n < cap) ? 0 : -1;
}

/* The paths below go into command lines and PowerShell scripts inside quotes,
 * so a quote or a shell metacharacter in one would end the quoting. Such a
 * path is refused rather than escaped. */
static int tc_path_quotable(const char* s) {
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '"' || c == '\'' || c == '`' || c == '$') return 0;
    }
    return 1;
}

/* A base URL from the environment is used in the same command lines, so it
 * may hold only the characters a URL or a file:// path needs. */
static int tc_url_ok(const char* s) {
    if (!*s) return 0;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) continue;
        if (strchr(":/._-~%+=,@", c)) continue;
        return 0;
    }
    return 1;
}

/* <AETHER_HOME or ~/.aether>/toolchains, with forward slashes. When `create`
 * is set the directory is made; either way it is made absolute when it
 * exists. Returns 0, 1 when it does not exist (and was not to be made), or
 * -1 on an error, which is printed. */
static int tc_root(char* out, size_t cap, int create) {
    const char* ah = getenv("AETHER_HOME");
    char raw[AE_TC_PATH];
    int cut = (ah && ah[0]) ? tc_fmt(raw, sizeof(raw), "%s/toolchains", ah)
                            : tc_fmt(raw, sizeof(raw), "%s/.aether/toolchains", get_home_dir());
    if (cut) {
        fprintf(stderr, "ae toolchain: the toolchains directory path is too long\n");
        return -1;
    }
    if (create) mkdirs(raw);
    if (!dir_exists(raw)) {
        if (!create) {
            snprintf(out, cap, "%s", raw);
            return 1;
        }
        fprintf(stderr, "ae toolchain: could not create %s\n", raw);
        return -1;
    }
#ifdef _WIN32
    if (!_fullpath(out, raw, cap)) {
        fprintf(stderr, "ae toolchain: could not resolve %s\n", raw);
        return -1;
    }
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
#else
    char resolved[PATH_MAX];
    if (!realpath(raw, resolved) || tc_fmt(out, cap, "%s", resolved)) {
        fprintf(stderr, "ae toolchain: could not resolve %s\n", raw);
        return -1;
    }
#endif
    if (!tc_path_quotable(out)) {
        fprintf(stderr,
                "ae toolchain: %s holds a quote, `$` or control character, which the\n"
                "download and unpack commands cannot carry; set AETHER_HOME to another\n"
                "directory\n", out);
        return -1;
    }
    return 0;
}

/* The first line `"<zig>" version` prints, trimmed, into `out`. The output is
 * parked in `scratch_dir`, which belongs to this user. 0, or -1 when zig
 * could not be run or failed. */
static int tc_zig_version(const char* zig, const char* scratch_dir, char* out, size_t cap) {
    out[0] = '\0';
    char capture[AE_TC_PATH], cmd[AE_TC_PATH + 32];
    if (tc_fmt(capture, sizeof(capture), "%s/.zig-version-%ld.out", scratch_dir, (long)getpid()) ||
        tc_fmt(cmd, sizeof(cmd), "\"%s\" version", zig)) return -1;
    int rc = run_cmd_capture_stdout(cmd, capture);
    FILE* f = fopen(capture, "r");
    if (f) {
        if (!fgets(out, (int)cap, f)) out[0] = '\0';
        fclose(f);
    }
    remove(capture);
    size_t n = strlen(out);
    while (n > 0 && isspace((unsigned char)out[n - 1])) out[--n] = '\0';
    return rc == 0 && out[0] ? 0 : -1;
}

/* What the stamp of an install made from `pin`'s archive holds. */
static void tc_stamp_text(const AeZigPin* pin, char* out, size_t cap) {
    snprintf(out, cap, "zig %s zig-%s-%s%s sha256 %s\n",
             AE_ZIG_VERSION, pin->host, AE_ZIG_VERSION, pin->ext, pin->sha256);
}

/* Check the install at `dir`. 0 when it is good, 1 when there is none, 2
 * when it is there but bad; `why` says what is wrong. */
static int tc_check_install(const char* root, const char* dir, const AeZigPin* pin,
                            char* zig, size_t zig_cap, char* why, size_t why_cap) {
    why[0] = '\0';
    snprintf(zig, zig_cap, "%s/zig" AE_TC_EXE, dir);
    if (!dir_exists(dir)) {
        snprintf(why, why_cap, "zig %s is not installed (no %s)", AE_ZIG_VERSION, dir);
        return 1;
    }
    if (!path_exists(zig)) {
        snprintf(why, why_cap, "%s has no zig" AE_TC_EXE, dir);
        return 2;
    }
    char stamp_path[AE_TC_PATH], want[512], have[512] = "";
    snprintf(stamp_path, sizeof(stamp_path), "%s/" AE_ZIG_STAMP, dir);
    tc_stamp_text(pin, want, sizeof(want));
    FILE* f = fopen(stamp_path, "r");
    int stamped = f != NULL;
    if (f) {
        size_t got = fread(have, 1, sizeof(have) - 1, f);
        have[got] = '\0';
        fclose(f);
    }
    if (strcmp(have, want) != 0) {
        snprintf(why, why_cap,
                 "%s was not installed by `ae toolchain zig` from this host's pinned "
                 "archive (its %s %s)", dir, AE_ZIG_STAMP, stamped ? "differs" : "is missing");
        return 2;
    }
    char ver[128];
    if (tc_zig_version(zig, root, ver, sizeof(ver)) != 0) {
        snprintf(why, why_cap, "`%s version` failed", zig);
        return 2;
    }
    if (strcmp(ver, AE_ZIG_VERSION) != 0) {
        snprintf(why, why_cap, "`%s version` reports %s, not %s", zig, ver, AE_ZIG_VERSION);
        return 2;
    }
    return 0;
}

/* 1 when the archive entry `name` lies under `top`: no absolute path or
 * drive, no `..` component, and `top` or `top/...` once a backslash is read
 * as the separator it is on Windows. A blank line (the listing's end) is
 * allowed. */
static int tc_entry_ok(const char* name, const char* top) {
    char buf[AE_TC_PATH];
    if (tc_fmt(buf, sizeof(buf), "%s", name)) return 0;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) buf[--n] = '\0';
    if (n == 0) return 1;
    for (char* p = buf; *p; p++) if (*p == '\\') *p = '/';
    if (buf[0] == '/') return 0;
    if (isalpha((unsigned char)buf[0]) && buf[1] == ':') return 0;
    for (const char* c = buf; *c; ) {
        const char* e = strchr(c, '/');
        size_t len = e ? (size_t)(e - c) : strlen(c);
        if (len == 2 && c[0] == '.' && c[1] == '.') return 0;
        if (!e) break;
        c = e + 1;
    }
    size_t tl = strlen(top);
    return strncmp(buf, top, tl) == 0 && (buf[tl] == '\0' || buf[tl] == '/');
}

/* List the archive's entry names and check each with tc_entry_ok. The
 * listing is parked in `stage`. 0 when every entry is under `top`; -1, with
 * the reason printed, otherwise. */
static int tc_check_entries(const char* archive, const char* stage, const char* top) {
    char list[AE_TC_PATH], cmd[AE_TC_PATH * 2];
    if (tc_fmt(list, sizeof(list), "%s/entries.txt", stage)) return -1;
#ifdef _WIN32
    /* .NET reads the zip's central directory; nothing is extracted. */
    char ps1[AE_TC_PATH];
    if (tc_fmt(ps1, sizeof(ps1), "%s/list.ps1", stage)) return -1;
    FILE* s = fopen(ps1, "w");
    if (!s) {
        fprintf(stderr, "ae toolchain: could not write %s\n", ps1);
        return -1;
    }
    fprintf(s,
        "$ErrorActionPreference='Stop'\n"
        "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
        "$z = [IO.Compression.ZipFile]::OpenRead('%s')\n"
        "try { foreach ($e in $z.Entries) { [Console]::Out.WriteLine($e.FullName) } }\n"
        "finally { $z.Dispose() }\n", archive);
    fclose(s);
    if (tc_fmt(cmd, sizeof(cmd),
               "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps1)) return -1;
#else
    if (tc_fmt(cmd, sizeof(cmd), "tar -tJf \"%s\"", archive)) return -1;
#endif
    int rc = run_cmd_capture_stdout(cmd, list);
    if (rc != 0) {
        fprintf(stderr, "ae toolchain: could not list the entries of %s\n", archive);
        return -1;
    }
    FILE* f = fopen(list, "r");
    if (!f) {
        fprintf(stderr, "ae toolchain: could not read the entry list of %s\n", archive);
        return -1;
    }
    char line[AE_TC_PATH];
    long count = 0;
    int ok = 1;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "ae toolchain: an entry name in %s is too long\n", archive);
            ok = 0;
            break;
        }
        if (!tc_entry_ok(line, top)) {
            line[strcspn(line, "\r\n")] = '\0';
            fprintf(stderr, "ae toolchain: refusing %s: the entry '%s' is not under %s/\n",
                    archive, line, top);
            ok = 0;
            break;
        }
        count++;
    }
    fclose(f);
    remove(list);
    if (ok && count == 0) {
        fprintf(stderr, "ae toolchain: %s lists no entries\n", archive);
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* A fresh staging directory inside `root`, made by a call that fails when
 * the path already exists, so nothing can be waiting there. 0, or -1. */
static int tc_make_stage(const char* root, char* out, size_t cap) {
#ifdef _WIN32
    static unsigned seq = 0;
    for (int tries = 0; tries < 100; tries++) {
        if (tc_fmt(out, cap, "%s/.zig-%s-stage-%lu-%lu-%u", root, AE_ZIG_VERSION,
                   (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(),
                   seq++)) return -1;
        if (_mkdir(out) == 0) return 0;
        if (errno != EEXIST) return -1;
    }
    return -1;
#else
    if (tc_fmt(out, cap, "%s/.zig-%s-stage-XXXXXX", root, AE_ZIG_VERSION)) return -1;
    return mkdtemp(out) ? 0 : -1;
#endif
}

/* Remove staging directories a killed run left behind, once they are old
 * enough that no run can still be using them. */
static void tc_sweep_stale_stages(const char* root) {
    DIR* d = opendir(root);
    if (!d) return;
    time_t now = time(NULL);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".zig-", 5) != 0 || !strstr(e->d_name, "-stage-")) continue;
        char path[AE_TC_PATH];
        if (tc_fmt(path, sizeof(path), "%s/%s", root, e->d_name)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
            now - st.st_mtime > AE_ZIG_STALE_STAGE_SECONDS) {
            ae_remove_tree(path);
        }
    }
    closedir(d);
}

/* rename() a directory into place. On Windows a virus scanner can hold a
 * file of a tree it just saw unpacked for a moment, so a refused rename is
 * retried for a few seconds while the destination stays absent. */
static int tc_rename_dir(const char* from, const char* to) {
#ifdef _WIN32
    for (int tries = 0; tries < 40; tries++) {
        if (rename(from, to) == 0) return 0;
        if (dir_exists(to)) return -1;
        Sleep(250);
    }
    return -1;
#else
    return rename(from, to);
#endif
}

/* Download, verify, unpack and move the pinned Zig into `dir`. 0, or 1 with
 * the reason printed. */
static int tc_install(const char* root, const char* dir, const AeZigPin* pin) {
    char archive_name[256], top[256];
    snprintf(archive_name, sizeof(archive_name), "zig-%s-%s%s", pin->host, AE_ZIG_VERSION, pin->ext);
    snprintf(top, sizeof(top), "zig-%s-%s", pin->host, AE_ZIG_VERSION);

    const char* base = getenv("AE_ZIG_BASE_URL");
    if (base && base[0]) {
        if (!tc_url_ok(base)) {
            fprintf(stderr, "ae toolchain: AE_ZIG_BASE_URL may hold only letters, digits and "
                            "the characters :/._-~%%+=,@\n");
            return 1;
        }
    } else {
        base = AE_ZIG_DEFAULT_BASE;
    }
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/') bl--;
    char url[AE_TC_PATH];
    if (tc_fmt(url, sizeof(url), "%.*s/%s/%s", (int)bl, base, AE_ZIG_VERSION, archive_name)) {
        fprintf(stderr, "ae toolchain: the download URL is too long\n");
        return 1;
    }

    tc_sweep_stale_stages(root);
    char stage[AE_TC_PATH];
    if (tc_make_stage(root, stage, sizeof(stage)) != 0) {
        fprintf(stderr, "ae toolchain: could not create a staging directory in %s\n", root);
        return 1;
    }
    char archive[AE_TC_PATH], unpack[AE_TC_PATH], tree[AE_TC_PATH], old[AE_TC_PATH];
    if (tc_fmt(archive, sizeof(archive), "%s/%s", stage, archive_name) ||
        tc_fmt(unpack, sizeof(unpack), "%s/unpack", stage) ||
        tc_fmt(tree, sizeof(tree), "%s/%s", unpack, top) ||
        tc_fmt(old, sizeof(old), "%s/old", stage)) {
        fprintf(stderr, "ae toolchain: the staging path is too long\n");
        ae_remove_tree(stage);
        return 1;
    }

    fprintf(stderr, "ae toolchain: downloading %s\n", url);
    if (ae_download_ex(url, archive, 0) != 0 || !path_exists(archive)) {
        fprintf(stderr, "ae toolchain: could not download %s\n", url);
        ae_remove_tree(stage);
        return 1;
    }

    /* The hash is checked before anything reads the archive. */
    char got[65];
    if (ae_sha256_file_hex(archive, got) != 0) {
        fprintf(stderr, "ae toolchain: could not read %s to hash it\n", archive);
        ae_remove_tree(stage);
        return 1;
    }
    if (strcmp(got, pin->sha256) != 0) {
        remove(archive);
        ae_remove_tree(stage);
        fprintf(stderr,
                "ae toolchain: sha256 mismatch for %s, so it was deleted unused\n"
                "  expected %s\n"
                "  got      %s\n", url, pin->sha256, got);
        return 1;
    }

    if (tc_check_entries(archive, stage, top) != 0) {
        ae_remove_tree(stage);
        return 1;
    }
    mkdirs(unpack);
    fprintf(stderr, "ae toolchain: unpacking %s\n", archive_name);
    if (ae_extract(archive, unpack) != 0 || !dir_exists(tree)) {
        fprintf(stderr, "ae toolchain: could not unpack %s\n", archive_name);
        ae_remove_tree(stage);
        return 1;
    }
    remove(archive);

    char stamp_path[AE_TC_PATH], stamp[512];
    if (tc_fmt(stamp_path, sizeof(stamp_path), "%s/" AE_ZIG_STAMP, tree)) {
        ae_remove_tree(stage);
        return 1;
    }
    tc_stamp_text(pin, stamp, sizeof(stamp));
    FILE* sf = fopen(stamp_path, "w");
    if (!sf || fputs(stamp, sf) < 0 || fclose(sf) != 0) {
        fprintf(stderr, "ae toolchain: could not write %s\n", stamp_path);
        ae_remove_tree(stage);
        return 1;
    }

    /* A bad install in the way is moved into the stage, and goes with it. */
    if (dir_exists(dir) && tc_rename_dir(dir, old) != 0) {
        fprintf(stderr, "ae toolchain: could not move the old %s aside\n", dir);
        ae_remove_tree(stage);
        return 1;
    }
    if (tc_rename_dir(tree, dir) != 0) {
        /* Another run may have finished first; its install is checked by the
         * caller like any other. */
        if (!dir_exists(dir)) {
            fprintf(stderr, "ae toolchain: could not move the unpacked tree to %s\n", dir);
            ae_remove_tree(stage);
            return 1;
        }
    }
    ae_remove_tree(stage);
    return 0;
}

static int cmd_toolchain_zig(int argc, char** argv) {
    int check = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            /* taken by the driver */
        } else {
            fprintf(stderr, "ae toolchain zig: unknown option '%s'\n\n", argv[i]);
            ae_toolchain_usage(stderr);
            return 1;
        }
    }

    char host[64];
    const AeZigPin* pin = zig_host_pin(host, sizeof(host));
    if (!pin) {
        fprintf(stderr, "ae toolchain zig: no Zig %s archive is pinned for this host (%s).\n"
                        "Pinned hosts:", AE_ZIG_VERSION, host);
        for (size_t i = 0; i < sizeof(ae_zig_pins) / sizeof(ae_zig_pins[0]); i++)
            fprintf(stderr, " %s", ae_zig_pins[i].host);
        fprintf(stderr, "\n");
        return 1;
    }

    char root[AE_TC_PATH];
    int r = tc_root(root, sizeof(root), !check);
    if (r < 0) return 1;
    if (r == 1) {
        fprintf(stderr, "ae toolchain zig: zig %s is not installed (no %s). "
                        "Run `ae toolchain zig` to install it.\n", AE_ZIG_VERSION, root);
        return 1;
    }
    char dir[AE_TC_PATH], zig[AE_TC_PATH], why[AE_TC_PATH * 2];
    if (tc_fmt(dir, sizeof(dir), "%s/zig-%s", root, AE_ZIG_VERSION)) {
        fprintf(stderr, "ae toolchain zig: the install path is too long\n");
        return 1;
    }

    int state = tc_check_install(root, dir, pin, zig, sizeof(zig), why, sizeof(why));
    if (state == 0) {
        printf("%s\n", zig);
        return 0;
    }
    if (check) {
        fprintf(stderr, "ae toolchain zig: %s.%s\n", why,
                state == 1 ? " Run `ae toolchain zig` to install it." : "");
        return 1;
    }
    if (state == 2) fprintf(stderr, "ae toolchain zig: %s; installing it again\n", why);

    if (tc_install(root, dir, pin) != 0) return 1;
    state = tc_check_install(root, dir, pin, zig, sizeof(zig), why, sizeof(why));
    if (state != 0) {
        fprintf(stderr, "ae toolchain zig: the new install is not usable: %s\n", why);
        return 1;
    }
    printf("%s\n", zig);
    return 0;
}

int cmd_toolchain(int argc, char** argv) {
    if (argc < 1) {
        ae_toolchain_usage(stderr);
        return 1;
    }
    if (strcmp(argv[0], "zig") == 0) return cmd_toolchain_zig(argc - 1, argv + 1);
    fprintf(stderr, "ae toolchain: unknown toolchain '%s'\n\n", argv[0]);
    ae_toolchain_usage(stderr);
    return 1;
}
