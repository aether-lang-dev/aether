/* ae_version.c — the version manager and the macOS Gatekeeper helper
 * (#1221 split). List/install/switch releases moved verbatim out of ae.c;
 * macos_prepare_binary travels with them (install re-signs a downloaded
 * binary) but is also called from ae.c's build path, so it is declared in
 * ae_internal.h. The download/extract helpers stay static to this file.
 */

#include "ae_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>    /* GetCurrentProcessId for temp-file names */
#  include <process.h>    /* _getpid */
#  ifndef getpid
#    define getpid _getpid
#  endif
#  define EXE_EXT ".exe"
#else
#  include <unistd.h>
#  define EXE_EXT ""
#endif
#ifndef AE_VERSION
#  ifdef AETHER_VERSION
#    define AE_VERSION AETHER_VERSION
#  else
#    define AE_VERSION "dev"
#  endif
#endif

// --------------------------------------------------------------------------
// Version manager: list available releases, install, and switch versions
// --------------------------------------------------------------------------

// Compile-time platform string used to pick the right release archive.
#if defined(_WIN32)
#  if defined(__aarch64__) || defined(_M_ARM64)
#    define AE_PLATFORM "windows-arm64"
#  else
#    define AE_PLATFORM "windows-x86_64"
#  endif
#  define AE_ARCHIVE_EXT ".zip"
#elif defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
#  define AE_PLATFORM "macos-arm64"
#  define AE_ARCHIVE_EXT ".tar.gz"
#elif defined(__APPLE__)
#  define AE_PLATFORM "macos-x86_64"
#  define AE_ARCHIVE_EXT ".tar.gz"
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
#  define AE_PLATFORM "linux-arm64"
#  define AE_ARCHIVE_EXT ".tar.gz"
#else
#  define AE_PLATFORM "linux-x86_64"
#  define AE_ARCHIVE_EXT ".tar.gz"
#endif

#define AE_GITHUB_REPO "nicolasmd87/aether"

// Download url → dest file. Uses curl/wget on POSIX, PowerShell on Windows.
// Creates parent directories of dest if they don't exist.
static int ae_download(const char* url, const char* dest) {
    // Ensure parent directory exists (e.g. ~/.aether/ for releases.json)
    {
        char parent[1024];
        strncpy(parent, dest, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char* slash = strrchr(parent, '/');
        if (!slash) slash = strrchr(parent, '\\');
        if (slash) { *slash = '\0'; mkdirs(parent); }
    }
#ifdef _WIN32
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "ae_dl_%u.ps1", (unsigned)GetCurrentProcessId());
    char ps_path[1024];
    snprintf(ps_path, sizeof(ps_path), "%s\\%s", get_temp_dir(), tmp);
    FILE* ps = fopen(ps_path, "w");
    if (!ps) return 1;
    fprintf(ps,
        "$ProgressPreference='SilentlyContinue'\n"
        "Invoke-WebRequest -Uri '%s' -OutFile '%s' "
        "-Headers @{'User-Agent'='ae-cli'}\n",
        url, dest);
    fclose(ps);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\" >nul 2>&1", ps_path);
    int r = system(cmd);
    remove(ps_path);
    return r;
#else
    char cmd[2048];
    if (system("curl --version >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof(cmd), "curl -fsSL -o \"%s\" \"%s\" 2>/dev/null", dest, url);
    else
        snprintf(cmd, sizeof(cmd), "wget -q --no-verbose -O \"%s\" \"%s\" 2>/dev/null", dest, url);
    return system(cmd);
#endif
}

// Extract archive → dest_dir.
static int ae_extract(const char* archive, const char* dest_dir) {
#ifdef _WIN32
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "ae_ex_%u.ps1", (unsigned)GetCurrentProcessId());
    char ps_path[1024];
    snprintf(ps_path, sizeof(ps_path), "%s\\%s", get_temp_dir(), tmp);
    FILE* ps = fopen(ps_path, "w");
    if (!ps) return 1;
    fprintf(ps,
        "$ProgressPreference='SilentlyContinue'\n"
        "Expand-Archive -Path '%s' -DestinationPath '%s' -Force\n",
        archive, dest_dir);
    fclose(ps);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\" >nul 2>&1", ps_path);
    int r = system(cmd);
    remove(ps_path);
    return r;
#else
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\"", archive, dest_dir);
    return system(cmd);
#endif
}

// Clear macOS Gatekeeper state on a freshly installed/copied binary.
//
// Released Aether binaries are adhoc-signed and get a quarantine xattr
// the moment they're downloaded. Clearing the quarantine alone is not
// enough: Gatekeeper also caches an "assessment" per file, and an
// ad-hoc-signed binary from an untrusted source is rejected by default.
// The rejection manifests as the shell seeing "Killed: 9" (SIGKILL) or
// `ae` hanging for seconds while `syspolicyd` evaluates the binary.
//
// The reliable fix is to re-sign in place with `codesign --force --sign -`.
// This refreshes the CDHash and creates a local ad-hoc signature that
// Gatekeeper allows to execute without notarization checks. Combined with
// clearing the quarantine attribute, the binary runs cleanly right away.
//
// No-op on Linux and Windows, which have no Gatekeeper equivalent.
void macos_prepare_binary(const char* path) {
#ifdef __APPLE__
    if (!path || !*path) return;
    char cmd[2048];
    // Re-sign in place. Suppress both success and failure output so a
    // missing codesign (unusual but not impossible on stripped systems)
    // degrades to quarantine-clear only.
    snprintf(cmd, sizeof(cmd),
             "codesign --force --sign - \"%s\" >/dev/null 2>&1", path);
    (void)system(cmd);
    // Clear quarantine + any other resource forks/xattrs that would
    // otherwise trigger an extra syspolicyd round-trip on first run.
    snprintf(cmd, sizeof(cmd), "xattr -cr \"%s\" >/dev/null 2>&1", path);
    (void)system(cmd);
#else
    (void)path;
#endif
}

// Prepare every executable in a bin directory. Used after install/extract
// and after copying binaries into ~/.aether/bin/ from a versioned install.
static void macos_prepare_bin_dir(const char* bin_dir) {
#ifdef __APPLE__
    if (!bin_dir || !*bin_dir) return;
    char cmd[2048];
    // find + xargs handles spaces via -print0/-0. We only touch regular
    // files; symlinks are skipped so we don't re-sign a link's target.
    snprintf(cmd, sizeof(cmd),
             "find \"%s\" -maxdepth 1 -type f -perm +111 -print0 2>/dev/null "
             "| xargs -0 -I {} codesign --force --sign - \"{}\" >/dev/null 2>&1",
             bin_dir);
    (void)system(cmd);
    snprintf(cmd, sizeof(cmd), "xattr -cr \"%s\" >/dev/null 2>&1", bin_dir);
    (void)system(cmd);
#else
    (void)bin_dir;
#endif
}

// List available releases from GitHub. Marks installed + current versions.
static int cmd_version_list(void) {
    const char* home = get_home_dir();

    // Determine which version is actually active.
    // Priority: 1) ~/.aether/active_version file (authoritative — written by install.sh and ae version use)
    //           2) ~/.aether/current symlink (legacy fallback)
    //           3) compiled-in AE_VERSION (fallback)
    char active_ver[64] = "";

    // Check active_version file first (always authoritative)
    {
        char avpath[512];
#ifdef _WIN32
        snprintf(avpath, sizeof(avpath), "%s\\.aether\\active_version", home);
#else
        snprintf(avpath, sizeof(avpath), "%s/.aether/active_version", home);
#endif
        FILE* avf = fopen(avpath, "r");
        if (avf) {
            if (fgets(active_ver, sizeof(active_ver), avf)) {
                char* nl = strchr(active_ver, '\n'); if (nl) *nl = '\0';
                char* cr = strchr(active_ver, '\r'); if (cr) *cr = '\0';
            }
            fclose(avf);
        }
    }

#ifndef _WIN32
    // Legacy fallback: resolve ~/.aether/current symlink
    if (active_ver[0] == '\0') {
        char current_link[512], target[1024];
        snprintf(current_link, sizeof(current_link), "%s/.aether/current", home);
        ssize_t rlen = readlink(current_link, target, sizeof(target) - 1);
        if (rlen > 0) {
            target[rlen] = '\0';
            const char* last = strrchr(target, '/');
            if (last) last++; else last = target;
            if (last[0] == 'v') last++;
            strncpy(active_ver, last, sizeof(active_ver) - 1);
            active_ver[sizeof(active_ver) - 1] = '\0';
        }
    }
#endif

    // Fallback: use compiled-in version
    if (active_ver[0] == '\0') {
        strncpy(active_ver, AE_VERSION, sizeof(active_ver) - 1);
        active_ver[sizeof(active_ver) - 1] = '\0';
    }

    // Fetch the GitHub releases JSON into a temp file
    char json_path[512];
#ifdef _WIN32
    snprintf(json_path, sizeof(json_path), "%s\\.aether\\releases.json", home);
#else
    snprintf(json_path, sizeof(json_path), "%s/ae_releases_%d.json", get_temp_dir(), (int)getpid());
#endif
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.github.com/repos/" AE_GITHUB_REPO "/releases?per_page=20");

    printf("Fetching release list...\n");
    if (ae_download(url, json_path) != 0) {
        fprintf(stderr, "Failed to fetch releases. Check your internet connection.\n");
        return 1;
    }

    FILE* f = fopen(json_path, "r");
    if (!f) {
        fprintf(stderr, "Failed to read release data.\n");
        return 1;
    }

    printf("\nAvailable Aether releases  (platform: " AE_PLATFORM "):\n\n");
    printf("  %-16s  %s\n", "Version", "Status");
    printf("  %-16s  %s\n", "-------", "------");

    // Read whole file, scan for "tag_name" occurrences
    char buf[131072];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(json_path);
    buf[n] = '\0';

    int found = 0;
    char* p = buf;
    while ((p = strstr(p, "\"tag_name\"")) != NULL) {
        p += 10;  // skip past "tag_name"
        char* q = strchr(p, '"'); if (!q) break; q++;   // opening "
        char* end = strchr(q, '"'); if (!end) break;
        size_t len = (size_t)(end - q);
        if (len == 0 || len > 32) { p = end + 1; continue; }

        char tag[33];
        memcpy(tag, q, len);
        tag[len] = '\0';
        p = end + 1;

        // v-prefix normalisation: strip 'v' to compare with active version
        const char* ver = (tag[0] == 'v') ? tag + 1 : tag;
        bool is_current = strcmp(ver, active_ver) == 0;

        // Check locally installed
        char ver_dir[512];
        snprintf(ver_dir, sizeof(ver_dir), "%s/.aether/versions/%s", home, tag);
        bool installed = dir_exists(ver_dir);

        const char* status = is_current ? "* current"
                           : installed  ? "  installed"
                                        : "";
        printf("  %-16s  %s\n", tag, status);
        found++;
    }

    if (!found) {
        printf("  (no releases found)\n");
    }
    printf("\n");
    // Show latest found tag in examples (or fallback)
    if (found > 0) {
        // First tag found is the latest (GitHub returns newest first)
        // Re-scan to get it
        char latest[33] = "v0.1.0";
        char* lp = strstr(buf, "\"tag_name\"");
        if (lp) {
            lp += 10;
            char* lq = strchr(lp, '"'); if (lq) { lq++;
            char* le = strchr(lq, '"'); if (le) {
                size_t ll = (size_t)(le - lq);
                if (ll > 0 && ll < sizeof(latest)) { memcpy(latest, lq, ll); latest[ll] = '\0'; }
            }}
        }
        printf("Install a version:  ae version install %s\n", latest);
        printf("Switch versions:    ae version use %s\n", latest);
    } else {
        printf("Install a version:  ae version install <version>\n");
        printf("Switch versions:    ae version use <version>\n");
    }
    return 0;
}

// Download and install a specific version into ~/.aether/versions/<tag>/
static int cmd_version_install(const char* version) {
    char vtag[64];
    if (version[0] != 'v') snprintf(vtag, sizeof(vtag), "v%s", version);
    else { strncpy(vtag, version, sizeof(vtag) - 1); vtag[sizeof(vtag)-1] = '\0'; }

    const char* ver = vtag + 1;  // strip leading 'v'
    const char* home = get_home_dir();

    char ver_dir[512];
    snprintf(ver_dir, sizeof(ver_dir), "%s/.aether/versions/%s", home, vtag);
    if (dir_exists(ver_dir)) {
        // Verify the install is complete by checking for binaries
        char probe[1024];
        int has_binary = 0;
#ifdef _WIN32
        snprintf(probe, sizeof(probe), "%s\\bin\\aetherc.exe", ver_dir);
        if (path_exists(probe)) has_binary = 1;
        snprintf(probe, sizeof(probe), "%s\\aetherc.exe", ver_dir);
        if (path_exists(probe)) has_binary = 1;
#else
        snprintf(probe, sizeof(probe), "%s/bin/aetherc", ver_dir);
        if (path_exists(probe)) has_binary = 1;
        snprintf(probe, sizeof(probe), "%s/aetherc", ver_dir);
        if (path_exists(probe)) has_binary = 1;
#endif
        if (has_binary) {
            // Also verify the install has sources or a prebuilt lib —
            // old ae versions had an extraction bug that only copied bin/
            char lib_probe[1024], share_probe[1024];
            int has_sources = 0;
            snprintf(lib_probe, sizeof(lib_probe), "%s/lib/aether/libaether.a", ver_dir);
            if (path_exists(lib_probe)) has_sources = 1;
            snprintf(share_probe, sizeof(share_probe), "%s/share/aether/runtime", ver_dir);
            if (dir_exists(share_probe)) has_sources = 1;
            if (has_sources) {
                printf("Version %s is already installed.\n", vtag);
                printf("Switch to it with: ae version use %s\n", vtag);
                return 0;
            }
            printf("Version %s has binaries but missing lib/share — reinstalling...\n", vtag);
            // Fall through to remove and re-download
        }
        // Incomplete install — remove and re-download
        printf("Incomplete installation of %s detected, reinstalling...\n", vtag);
#ifdef _WIN32
        char rm_cmd[1024];
        snprintf(rm_cmd, sizeof(rm_cmd), "rmdir /S /Q \"%s\"", ver_dir);
        if (system(rm_cmd) != 0) {
            fprintf(stderr, "Warning: failed to remove incomplete install at %s\n", ver_dir);
        }
#else
        char rm_cmd[1024];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", ver_dir);
        if (system(rm_cmd) != 0) {
            fprintf(stderr, "Warning: failed to remove incomplete install at %s\n", ver_dir);
        }
#endif
    }

    // Build URL and local archive path
    char filename[256], url[1024], archive[512];
    snprintf(filename, sizeof(filename),
        "aether-%s-" AE_PLATFORM AE_ARCHIVE_EXT, ver);
    snprintf(url, sizeof(url),
        "https://github.com/" AE_GITHUB_REPO "/releases/download/%s/%s",
        vtag, filename);
    snprintf(archive, sizeof(archive), "%s/.aether/%s", home, filename);

    printf("Downloading Aether %s for " AE_PLATFORM "...\n", vtag);
    fflush(stdout);
    if (ae_download(url, archive) != 0) {
        fprintf(stderr, "Error: Version %s not found for " AE_PLATFORM ".\n", vtag);
        fprintf(stderr, "Run 'ae version list' to see available versions.\n");
        return 1;
    }

    // Verify the downloaded file is a real archive, not a 404 HTML page.
    // Valid archives are at least 10KB; GitHub 404 pages are ~10-20KB HTML
    // but tar.gz/zip archives for Aether are always >100KB.
    {
        FILE* af = fopen(archive, "rb");
        if (!af) {
            fprintf(stderr, "Error: Downloaded file not found.\n");
            return 1;
        }
        fseek(af, 0, SEEK_END);
        long asize = ftell(af);
        // Also check the first bytes for archive magic
        fseek(af, 0, SEEK_SET);
        unsigned char magic[4] = {0};
        if (fread(magic, 1, 4, af) < 4) { /* short read — magic stays zeroed */ }
        fclose(af);

        int is_gzip = (magic[0] == 0x1f && magic[1] == 0x8b);  // .tar.gz
        int is_zip  = (magic[0] == 'P' && magic[1] == 'K');     // .zip
        int is_xz   = (magic[0] == 0xFD && magic[1] == '7');    // .tar.xz

        if (!is_gzip && !is_zip && !is_xz) {
            remove(archive);
            fprintf(stderr, "Error: Version %s not found for platform " AE_PLATFORM ".\n", vtag);
            fprintf(stderr, "The download returned an error page, not a release archive.\n");
            fprintf(stderr, "Available versions: ae version list\n");
            return 1;
        }
        if (asize < 1024) {
            remove(archive);
            fprintf(stderr, "Error: Downloaded archive is too small (%ld bytes) — likely corrupt.\n", asize);
            return 1;
        }
    }

    mkdirs(ver_dir);
    printf("Extracting...\n");

    // The release archive contains a top-level directory (e.g. "release/").
    // Extract to a temp dir first, then move the contents into ver_dir.
    char tmp_dir[512];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/.aether/_tmp_install", home);
    mkdirs(tmp_dir);

    if (ae_extract(archive, tmp_dir) != 0) {
        fprintf(stderr, "Extraction failed.\n");
        remove(archive);
        return 1;
    }
    remove(archive);

    // Move extracted contents into ver_dir.
    // Release archives may have a single wrapper directory (e.g. "aether-v0.21.0-macos-arm64/")
    // OR may have bin/, lib/, share/, include/ directly at root. Handle both cases.
#ifdef _WIN32
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "xcopy /E /Y /Q \"%s\\*\" \"%s\\\"", tmp_dir, ver_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error: Failed to copy installation files.\n");
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "rmdir /S /Q \"%s\"", tmp_dir);
    if (system(cmd) != 0) { /* non-fatal: temp dir cleanup */ }
#else
    {
        char cmd[4096];
        // If there is exactly one top-level entry and it is a directory,
        // treat it as a wrapper and copy its contents. Otherwise copy
        // everything directly (the archive has bin/, lib/, etc. at root).
        snprintf(cmd, sizeof(cmd),
            "entries=$(ls '%s' | wc -l | tr -d ' '); "
            "single=$(ls -d '%s'/*/ 2>/dev/null | wc -l | tr -d ' '); "
            "if [ \"$entries\" = \"1\" ] && [ \"$single\" = \"1\" ]; then "
            "  src=$(ls -d '%s'/*/); cp -r \"$src\"* '%s/'; "
            "else "
            "  cp -r '%s'/* '%s/'; "
            "fi",
            tmp_dir, tmp_dir, tmp_dir, ver_dir, tmp_dir, ver_dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "Error: Failed to copy installation files.\n");
            return 1;
        }
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
        if (system(cmd) != 0) { /* non-fatal: temp dir cleanup */ }
    }
#endif

    // Verify the installation has the expected structure.
    // Releases should have bin/, lib/ or share/aether/ — if we only see
    // flat binaries, the extraction went wrong.
    {
        char probe[1024];
        int has_structure = 0;
        snprintf(probe, sizeof(probe), "%s/bin", ver_dir);
        if (dir_exists(probe)) has_structure = 1;
        snprintf(probe, sizeof(probe), "%s/lib", ver_dir);
        if (dir_exists(probe)) has_structure = 1;
        snprintf(probe, sizeof(probe), "%s/share/aether", ver_dir);
        if (dir_exists(probe)) has_structure = 1;
        if (!has_structure) {
            // Clean up the empty/broken install directory
#ifdef _WIN32
            snprintf(probe, sizeof(probe), "rmdir /S /Q \"%s\"", ver_dir);
#else
            snprintf(probe, sizeof(probe), "rm -rf '%s'", ver_dir);
#endif
            if (system(probe) != 0) { /* cleanup failed — non-fatal */ }
            fprintf(stderr, "Error: Installation of %s failed — no bin/, lib/, or share/ found.\n", vtag);
            fprintf(stderr, "This version may not have a release for " AE_PLATFORM ".\n");
            fprintf(stderr, "Available versions: ae version list\n");
            return 1;
        }
    }

    // #959: integrity-check the prebuilt runtime archive after extraction.
    // An interrupted/partial extract has been observed to leave a truncated
    // libaether.a whose symbols are undefined (U) instead of defined (T), so
    // every later `ae build` fails to link with nothing pointing at the cause.
    // Validate that the archive (canonical nested path, or the flat fallback
    // some packages ship) is a well-formed `ar` archive of plausible size; on
    // failure remove the install and tell the user to retry, rather than
    // leaving a landmine. Skipped when the package ships sources only (no
    // prebuilt archive present). This does not replace a full checksum verify
    // (which needs the release pipeline to publish per-asset sums) but catches
    // the truncation that actually corrupted a macOS download.
    {
        char arch_path[1024];
        snprintf(arch_path, sizeof(arch_path), "%s/lib/aether/libaether.a", ver_dir);
        if (!path_exists(arch_path))
            snprintf(arch_path, sizeof(arch_path), "%s/lib/libaether.a", ver_dir);
        if (path_exists(arch_path)) {
            FILE* lf = fopen(arch_path, "rb");
            int ok = 0;
            if (lf) {
                char hdr[8] = {0};
                size_t n = fread(hdr, 1, sizeof(hdr), lf);
                fseek(lf, 0, SEEK_END);
                long lsize = ftell(lf);
                fclose(lf);
                /* `ar` archives begin with the 8-byte magic "!<arch>\n" on
                 * every platform we ship (GNU/BSD/macOS ar). A complete
                 * runtime+stdlib archive is multi-megabyte; a truncated one
                 * falls well short of this conservative 64 KB floor. */
                ok = (n == sizeof(hdr) &&
                      memcmp(hdr, "!<arch>\n", sizeof(hdr)) == 0 &&
                      lsize > 65536);
            }
            if (!ok) {
                char rmc[1024];
#ifdef _WIN32
                snprintf(rmc, sizeof(rmc), "rmdir /S /Q \"%s\"", ver_dir);
#else
                snprintf(rmc, sizeof(rmc), "rm -rf '%s'", ver_dir);
#endif
                if (system(rmc) != 0) { /* cleanup failed — non-fatal */ }
                fprintf(stderr,
                    "Error: %s installed a corrupt runtime archive "
                    "(lib/.../libaether.a is truncated or not an `ar` archive).\n",
                    vtag);
                fprintf(stderr,
                    "The download or extraction was likely interrupted. "
                    "Re-run: ae version install %s\n", vtag);
                return 1;
            }
        }
    }

    // Prepare binaries for macOS Gatekeeper. Download + extract leaves the
    // binaries quarantined; without this step the first run of any binary
    // from the fresh install (including the self-invocation from
    // `ae version use`) hangs or gets SIGKILL'd by syspolicyd.
    {
        char bin_sub[1024];
        snprintf(bin_sub, sizeof(bin_sub), "%s/bin", ver_dir);
        if (dir_exists(bin_sub)) {
            macos_prepare_bin_dir(bin_sub);
        }
    }

    printf("Installed Aether %s → %s\n", vtag, ver_dir);
    printf("Switch to it with: ae version use %s\n", vtag);
    return 0;
}

// Determine where binaries live inside a version directory.
// Release archives may have a bin/ subdirectory or binaries at root.
static void resolve_version_bin_dir(const char* ver_dir, char* out, size_t outsz) {
    char probe[1024];
#ifdef _WIN32
    snprintf(probe, sizeof(probe), "%s\\bin\\aetherc" EXE_EXT, ver_dir);
    if (path_exists(probe)) { snprintf(out, outsz, "%s\\bin", ver_dir); return; }
    snprintf(probe, sizeof(probe), "%s\\bin\\ae" EXE_EXT, ver_dir);
    if (path_exists(probe)) { snprintf(out, outsz, "%s\\bin", ver_dir); return; }
    snprintf(out, outsz, "%s", ver_dir);
#else
    snprintf(probe, sizeof(probe), "%s/bin/aetherc" EXE_EXT, ver_dir);
    if (path_exists(probe)) { snprintf(out, outsz, "%s/bin", ver_dir); return; }
    snprintf(probe, sizeof(probe), "%s/bin/ae" EXE_EXT, ver_dir);
    if (path_exists(probe)) { snprintf(out, outsz, "%s/bin", ver_dir); return; }
    snprintf(out, outsz, "%s", ver_dir);
#endif
}

int cmd_version_use(const char* version) {
    char vtag[64];
    if (version[0] != 'v') snprintf(vtag, sizeof(vtag), "v%s", version);
    else { strncpy(vtag, version, sizeof(vtag) - 1); vtag[sizeof(vtag)-1] = '\0'; }

    const char* home = get_home_dir();
    char ver_dir[512];
    snprintf(ver_dir, sizeof(ver_dir), "%s/.aether/versions/%s", home, vtag);

    if (!dir_exists(ver_dir)) {
        fprintf(stderr, "Version %s is not installed.\n", vtag);
        fprintf(stderr, "Install it first: ae version install %s\n", vtag);
        return 1;
    }

    char src_bin[1024];
    resolve_version_bin_dir(ver_dir, src_bin, sizeof(src_bin));

#ifdef _WIN32
    // Backup the currently active version to versions/ before overwriting.
    // This preserves the initial install (e.g., v0.30.0 installed via install.sh
    // lives in ~/.aether/ directly, not in versions/).
    {
        char avpath_bak[512];
        snprintf(avpath_bak, sizeof(avpath_bak), "%s\\.aether\\active_version", home);
        FILE* avf_bak = fopen(avpath_bak, "r");
        if (avf_bak) {
            char cur_ver[64] = "";
            if (fgets(cur_ver, sizeof(cur_ver), avf_bak)) {
                char* nl = strchr(cur_ver, '\n');
                if (nl) *nl = '\0';
            }
            fclose(avf_bak);
            if (cur_ver[0]) {
                char cur_vtag[66];
                if (cur_ver[0] != 'v') snprintf(cur_vtag, sizeof(cur_vtag), "v%s", cur_ver);
                else { strncpy(cur_vtag, cur_ver, sizeof(cur_vtag) - 1); cur_vtag[sizeof(cur_vtag)-1] = '\0'; }
                char cur_ver_dir[512];
                snprintf(cur_ver_dir, sizeof(cur_ver_dir), "%s\\.aether\\versions\\%s", home, cur_vtag);
                if (!dir_exists(cur_ver_dir)) {
                    // Current version not in versions/ — back it up
                    char bak_cmd[2048];
                    char dest_root_bak[512];
                    snprintf(dest_root_bak, sizeof(dest_root_bak), "%s\\.aether", home);
                    mkdirs(cur_ver_dir);
                    snprintf(bak_cmd, sizeof(bak_cmd),
                        "robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /IS /IT /XD versions cache >nul 2>&1",
                        dest_root_bak, cur_ver_dir);
                    if (system(bak_cmd) != 0) { /* backup failed — non-fatal */ }
                }
            }
        }
    }

    // Copy the entire version directory to ~/.aether/ so lib/, include/,
    // share/ are available alongside bin/.
    char dest_root[512];
    snprintf(dest_root, sizeof(dest_root), "%s\\.aether", home);
    char cmd[2048];
    // robocopy /E copies all subdirectories; /NFL /NDL /NJH /NJS suppress output
    snprintf(cmd, sizeof(cmd),
        "robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /IS /IT >nul 2>&1",
        ver_dir, dest_root);
    int rc = system(cmd);
    // robocopy returns 0-7 for success, >=8 for failure
    if (rc >= 8) {
        // Fall back to xcopy
        snprintf(cmd, sizeof(cmd),
            "xcopy /E /Y /Q \"%s\\*\" \"%s\\\"", ver_dir, dest_root);
        if (system(cmd) != 0) {
            fprintf(stderr, "Failed to copy version files from %s to %s\n", ver_dir, dest_root);
            return 1;
        }
    }
#else
    // Backup current version to versions/ before switching (preserves initial install)
    {
        char avpath_bak[512];
        snprintf(avpath_bak, sizeof(avpath_bak), "%s/.aether/active_version", home);
        FILE* avf_bak = fopen(avpath_bak, "r");
        if (avf_bak) {
            char cur_ver[64] = "";
            if (fgets(cur_ver, sizeof(cur_ver), avf_bak)) {
                char* nl = strchr(cur_ver, '\n'); if (nl) *nl = '\0';
            }
            fclose(avf_bak);
            if (cur_ver[0]) {
                char cur_vtag[64];
                if (cur_ver[0] != 'v') snprintf(cur_vtag, sizeof(cur_vtag), "v%s", cur_ver);
                else { strncpy(cur_vtag, cur_ver, sizeof(cur_vtag) - 1); cur_vtag[sizeof(cur_vtag)-1] = '\0'; }
                char cur_ver_dir[512];
                snprintf(cur_ver_dir, sizeof(cur_ver_dir), "%s/.aether/versions/%s", home, cur_vtag);
                if (!dir_exists(cur_ver_dir)) {
                    char bak_cmd[4096];
                    mkdirs(cur_ver_dir);
                    // Copy bin/, lib/, include/, share/ but NOT versions/ or cache/
                    snprintf(bak_cmd, sizeof(bak_cmd),
                        "cp -r \"%s/.aether/bin\" \"%s/\" 2>/dev/null; "
                        "cp -r \"%s/.aether/lib\" \"%s/\" 2>/dev/null; "
                        "cp -r \"%s/.aether/include\" \"%s/\" 2>/dev/null; "
                        "cp -r \"%s/.aether/share\" \"%s/\" 2>/dev/null; true",
                        home, cur_ver_dir, home, cur_ver_dir,
                        home, cur_ver_dir, home, cur_ver_dir);
                    if (system(bak_cmd) != 0) { /* backup failed — non-fatal */ }
                }
            }
        }
    }

    // Verify the source bin dir actually contains an `ae` binary before
    // we start mutating anything. This catches extraction-layout bugs
    // early rather than leaving the user with a half-switched install.
    {
        // src_bin is 1024 bytes; adding "/ae" + EXE_EXT (".exe" worst
        // case) + NUL needs 8 bytes of headroom over the source.
        char src_ae[1040];
        snprintf(src_ae, sizeof(src_ae), "%s/ae" EXE_EXT, src_bin);
        if (!path_exists(src_ae)) {
            fprintf(stderr, "Error: no ae binary at %s. The install for %s looks incomplete.\n", src_ae, vtag);
            fprintf(stderr, "Try reinstalling: ae version install %s\n", vtag);
            return 1;
        }
    }

    // POSIX: update ~/.aether/current symlink
    char current[512];
    snprintf(current, sizeof(current), "%s/.aether/current", home);
    remove(current);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "ln -sf \"%s\" \"%s\"", ver_dir, current);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to create symlink. Try manually:\n");
        fprintf(stderr, "  ln -sf %s %s\n", ver_dir, current);
        return 1;
    }

    // Copy binaries into ~/.aether/bin/. Previously this was a single
    // `cp -f "%s"/* "%s/" 2>/dev/null; true` which suppressed every
    // possible failure mode including a missing source or permissions
    // error. We now fail loudly and verify afterwards.
    char dest_bin[512];
    snprintf(dest_bin, sizeof(dest_bin), "%s/.aether/bin", home);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dest_bin);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error: failed to create %s\n", dest_bin);
        return 1;
    }
    // Use a subshell with nullglob so an empty source (which shouldn't
    // happen after the verification above, but just in case) fails the
    // `cp` explicitly rather than trying to copy a literal "*".
    snprintf(cmd, sizeof(cmd),
        "/bin/sh -c 'set -e; for f in \"%s\"/*; do cp -f \"$f\" \"%s/\"; done'",
        src_bin, dest_bin);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error: failed to copy binaries from %s to %s\n", src_bin, dest_bin);
        return 1;
    }

    // Verify the copy landed.
    {
        char dest_ae[1024];
        snprintf(dest_ae, sizeof(dest_ae), "%s/ae" EXE_EXT, dest_bin);
        if (!path_exists(dest_ae)) {
            fprintf(stderr, "Error: expected %s after copy, but it's not there.\n", dest_ae);
            return 1;
        }
    }

    // Prepare the binaries for macOS Gatekeeper (codesign + xattr clear).
    // Without this the next invocation of `ae` hangs or gets SIGKILL'd by
    // syspolicyd on first run. No-op on Linux.
    macos_prepare_bin_dir(dest_bin);

    // Sync lib/, include/, and share/ from the version directory to ~/.aether/
    // so that stale files left by a previous install.sh don't shadow the
    // version-managed files. The 'current' symlink alone is not enough because
    // toolchain discovery may resolve the parent directory first.
    //
    // We used to self-invoke the freshly copied ae with --sync-from as a
    // bootstrap for old-binary upgrades. On macOS that invocation hangs or
    // gets SIGKILL'd by Gatekeeper before syspolicyd finishes evaluating the
    // just-copied binary, leaving the user waiting minutes. The in-process
    // sync below does the same work, so we drop the self-invocation entirely.
    {
        char dest[512];
        const char* subdirs[] = {"lib", "include", "share"};
        for (int i = 0; i < 3; i++) {
            char src_sub[1024];
            snprintf(src_sub, sizeof(src_sub), "%s/%s", ver_dir, subdirs[i]);
            if (dir_exists(src_sub)) {
                snprintf(dest, sizeof(dest), "%s/.aether/%s", home, subdirs[i]);
                snprintf(cmd, sizeof(cmd),
                    "rm -rf \"%s\" && cp -r \"%s\" \"%s\"",
                    dest, src_sub, dest);
                if (system(cmd) != 0) {
                    fprintf(stderr, "Warning: failed to sync %s to %s\n", src_sub, dest);
                }
            }
        }
    }
#endif

    // Update active_version file so 'ae version list' shows the correct current
    // even if the symlink approach fails or on Windows.
    {
        char avpath[512];
#ifdef _WIN32
        snprintf(avpath, sizeof(avpath), "%s\\.aether\\active_version", home);
#else
        snprintf(avpath, sizeof(avpath), "%s/.aether/active_version", home);
#endif
        FILE* avf = fopen(avpath, "w");
        if (avf) {
            // Write version without 'v' prefix
            const char* v = (vtag[0] == 'v') ? vtag + 1 : vtag;
            fprintf(avf, "%s\n", v);
            fclose(avf);
        }
    }

    printf("Switched to Aether %s.\n", vtag);
    return 0;
}

// "ae version [list|install|use]"
// Read the active version from ~/.aether/active_version, fall back to compiled-in
static const char* get_active_version(void) {
    static char active[64];
    const char* home = get_home_dir();
    char avpath[512];
#ifdef _WIN32
    snprintf(avpath, sizeof(avpath), "%s\\.aether\\active_version", home);
#else
    snprintf(avpath, sizeof(avpath), "%s/.aether/active_version", home);
#endif
    FILE* f = fopen(avpath, "r");
    if (f) {
        if (fgets(active, sizeof(active), f)) {
            // Trim newline
            char* nl = strchr(active, '\n');
            if (nl) *nl = '\0';
            fclose(f);
            if (active[0]) return active;
        }
        fclose(f);
    }
    return AE_VERSION;
}

// Fetch the newest release tag (e.g. "v0.231.0") from GitHub into `out`.
// Returns 0 on success, non-zero on network/parse failure. GitHub returns
// releases newest-first, so the first "tag_name" is the latest — same
// scan cmd_version_list uses.
static int fetch_latest_release_tag(char* out, size_t outlen) {
    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s/ae_latest_%d.json",
             get_temp_dir(), (int)getpid());
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.github.com/repos/" AE_GITHUB_REPO "/releases?per_page=5");
    if (ae_download(url, json_path) != 0) return -1;
    FILE* f = fopen(json_path, "r");
    if (!f) { remove(json_path); return -1; }
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(json_path);
    buf[n] = '\0';
    char* p = strstr(buf, "\"tag_name\"");
    if (!p) return -1;
    p += 10;
    char* q = strchr(p, '"'); if (!q) return -1; q++;   // opening quote
    char* e = strchr(q, '"'); if (!e) return -1;         // closing quote
    size_t len = (size_t)(e - q);
    if (len == 0 || len >= outlen) return -1;
    memcpy(out, q, len);
    out[len] = '\0';
    return 0;
}

// "ae install [<version>]" — install a release into ~/.aether/versions/.
// With no version, resolves and installs the latest. A discoverable
// top-level alias for "ae version install" (matches rustup/nvm muscle
// memory). Does NOT switch the active version — that's "ae use <v>" or
// "ae upgrade".
int cmd_install(int argc, char** argv) {
    if (argc >= 1 && argv[0] && argv[0][0]) {
        return cmd_version_install(argv[0]);
    }
    char latest[64];
    printf("Resolving latest release...\n");
    if (fetch_latest_release_tag(latest, sizeof(latest)) != 0) {
        fprintf(stderr, "Could not determine the latest release.\n");
        fprintf(stderr, "Check your connection, or name a version: ae install <v>\n");
        fprintf(stderr, "('ae version list' shows what's available.)\n");
        return 1;
    }
    return cmd_version_install(latest);
}

// "ae upgrade" / "ae update" — install the latest release and switch to it.
// The one-shot "get me the newest Aether" command; a no-op (with a notice)
// when already current.
int cmd_upgrade(void) {
    char latest[64];
    printf("Checking for the latest release...\n");
    if (fetch_latest_release_tag(latest, sizeof(latest)) != 0) {
        fprintf(stderr, "Could not determine the latest release. Check your connection.\n");
        return 1;
    }
    const char* latest_ver = (latest[0] == 'v') ? latest + 1 : latest;
    const char* active = get_active_version();   // no leading 'v'
    if (active && strcmp(active, latest_ver) == 0) {
        printf("Already on the latest version (%s).\n", latest);
        return 0;
    }
    printf("Upgrading %s -> %s\n", (active && active[0]) ? active : "(unknown)", latest);
    int rc = cmd_version_install(latest);
    if (rc != 0) return rc;
    return cmd_version_use(latest);
}

int cmd_version(int argc, char** argv) {
    if (argc == 0) {
        printf("ae %s (Aether Language)\n", get_active_version());
        printf("Platform: " AE_PLATFORM "\n");
        printf("\nSubcommands:\n");
        printf("  ae version list              List all available releases\n");
        printf("  ae version install <v>       Download and install a release\n");
        printf("  ae version use <v>           Switch to an installed release\n");
        return 0;
    }
    const char* sub = argv[0];
    if (strcmp(sub, "list") == 0)    return cmd_version_list();
    if (strcmp(sub, "install") == 0) {
        if (argc < 2) { fprintf(stderr, "Usage: ae version install <v>\n"); return 1; }
        return cmd_version_install(argv[1]);
    }
    if (strcmp(sub, "use") == 0) {
        if (argc < 2) { fprintf(stderr, "Usage: ae version use <v>\n"); return 1; }
        return cmd_version_use(argv[1]);
    }
    // Internal: called by old ae binaries after copying new ae to ~/.aether/bin/
    // Syncs lib/, include/, share/ from a version directory to ~/.aether/
    if (strcmp(sub, "--sync-from") == 0) {
        if (argc < 2) return 1;
        const char* ver_dir = argv[1];
        const char* h = get_home_dir();
        const char* subdirs[] = {"lib", "include", "share"};
        for (int i = 0; i < 3; i++) {
            char src_sub[1024], dest[512], cmd[4096];
            snprintf(src_sub, sizeof(src_sub), "%s/%s", ver_dir, subdirs[i]);
            if (dir_exists(src_sub)) {
                snprintf(dest, sizeof(dest), "%s/.aether/%s", h, subdirs[i]);
                snprintf(cmd, sizeof(cmd),
                    "rm -rf \"%s\" && cp -r \"%s\" \"%s\"", dest, src_sub, dest);
                if (system(cmd) != 0) {
                    fprintf(stderr, "Warning: failed to sync %s to %s\n", src_sub, dest);
                }
            }
        }
        return 0;
    }
    // Fall-through: treat unknown sub as "ae version" (backward compat)
    printf("ae %s (Aether Language)\n", AE_VERSION);
    return 0;
}
