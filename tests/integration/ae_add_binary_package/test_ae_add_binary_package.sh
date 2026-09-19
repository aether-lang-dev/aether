#!/bin/sh
# `ae add` installs a BINARY PACKAGE: a bare per-triple shared lib named by a
# released aether.toml's `[package] binary = "<stem>"`, rather than an archive
# or a git clone. The aether.toml's `modules = "."` puts the fetched lib on the
# search path so the binary-import prepass can find it.
#
# Hermetic: a python http.server serves a fake forge whose layout mirrors a real
# one (<pkg>/releases/download/<tag>/{aether.toml, <stem>-<tag>-<triple>.<ext>,
# .sha256}); AE_RELEASE_BASE_URL points `ae add` at it. No public internet.
#
# Pinned properties:
#   1. a released aether.toml with a `binary` key + a matching lib installs BOTH
#      the lib and the aether.toml into ~/.aether/packages/<pkg>/ (no archive,
#      no git), checksum-verified, and records the dep.
#   2. a `binary` key WITHOUT a lib for this host is FATAL (exit 1) — it does NOT
#      silently fall through to a source archive / git clone (no fallback ladder).
#   3. an aether.toml with NO `binary` key is NOT a binary package — it falls
#      through to the archive path (here: to git, which fails on the fake forge;
#      we assert it did NOT install as a binary package).
#   4. a MISMATCHED checksum on the lib is fatal — nothing installed.
#   4b. a MISSING .sha256 on a binary package is fatal too (stricter than the
#       archive path): a raw downloaded shared library must be verifiable.
#   4c. `--target <triple>` fetches a FOREIGN platform's binary (right OS ext),
#       and `--target` with `--source` is rejected.
#   5. end-to-end: a real installed lib actually imports + runs after ae add.
#
# HOME is redirected per-case so the real package cache is never touched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] ae_add_binary_package: ae not built"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] ae_add_binary_package: python3 needed"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] ae_add_binary_package: curl needed"; exit 0; }
if command -v sha256sum >/dev/null 2>&1; then SHA="sha256sum"
elif command -v shasum >/dev/null 2>&1; then SHA="shasum -a 256"
else echo "  [SKIP] ae_add_binary_package: no sha256 tool"; exit 0; fi

# The binary-package path is per-host-triple; on a host we can't name a triple
# for, ae_host_triple() returns NULL and everything correctly falls to git.
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64|Linux-aarch64|Linux-arm64|Darwin-arm64|Darwin-x86_64|FreeBSD-*) ;;
    *) echo "  [SKIP] ae_add_binary_package: no release triple for this host"; exit 0 ;;
esac

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; rm -rf "$TMP"; return 0; }
trap cleanup EXIT

fail() {
    echo "  [FAIL] ae_add_binary_package: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

# ---- host triple + shlib ext, spelled as the release convention does ------
case "$(uname -s)" in
    Linux)   OS_PART="linux";   EXT=".so" ;;
    Darwin)  OS_PART="macos";   EXT=".dylib" ;;
    FreeBSD) OS_PART="freebsd"; EXT=".so" ;;
esac
case "$(uname -m)" in
    x86_64)        ARCH_PART="x86_64" ;;
    aarch64|arm64) ARCH_PART="arm64" ;;
esac
TRIPLE="$OS_PART-$ARCH_PART"

# ---- fake forge -----------------------------------------------------------
FORGE="$TMP/forge"
PKG="fake.host/user/pkg"
STEM="libpkg"

# mk_binpkg <tag> <mode>  where mode = ok | badsum | nolib | nobinkey
mk_binpkg() {
    tag="$1"; mode="$2"
    d="$FORGE/$PKG/releases/download/$tag"
    mkdir -p "$d"
    if [ "$mode" = "nobinkey" ]; then
        printf '[package]\nname = "pkg"\nmodules = "."\n' > "$d/aether.toml"
        return
    fi
    printf '[package]\nname = "pkg"\nmodules = "."\nbinary = "%s"\n' "$STEM" > "$d/aether.toml"
    [ "$mode" = "nolib" ] && return   # binary declared, but no lib asset

    asset="$STEM-$tag-$TRIPLE$EXT"
    # A small but real binary payload (not a real dlopen target — this test
    # exercises ae add's FETCH + INSTALL, not loading).
    head -c 4096 /dev/urandom > "$d/$asset"
    if [ "$mode" = "badsum" ]; then
        echo "0000000000000000000000000000000000000000000000000000000000000000  $asset" > "$d/$asset.sha256"
    elif [ "$mode" = "nosum" ]; then
        :   # lib present, NO .sha256 sidecar — a binary package must refuse this
    else
        ( cd "$d" && $SHA "$asset" | awk -v n="$asset" '{print $1"  "n}' > "$asset.sha256" )
    fi
    # A FOREIGN-platform lib for the same release, so `ae add --target <triple>`
    # has something to fetch. Pick a triple that is NOT this host's.
    case "$TRIPLE" in
        linux-x86_64) FT="macos-arm64";   FEXT=".dylib" ;;
        *)            FT="linux-x86_64";  FEXT=".so" ;;
    esac
    fasset="$STEM-$tag-$FT$FEXT"
    head -c 4096 /dev/urandom > "$d/$fasset"
    ( cd "$d" && $SHA "$fasset" | awk -v n="$fasset" '{print $1"  "n}' > "$fasset.sha256" )
}
mk_binpkg v1.0.0 ok
mk_binpkg v2.0.0 nolib
mk_binpkg v3.0.0 nobinkey
mk_binpkg v4.0.0 badsum
mk_binpkg v5.0.0 nosum

# ---- serve on a free loopback port ---------------------------------------
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
( cd "$FORGE" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
SRV_PID=$!
ready=0; i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; then ready=1; break; fi
    i=$((i + 1)); sleep 0.1
done
[ "$ready" = "1" ] || { echo "  [SKIP] ae_add_binary_package: fixture server did not start"; exit 0; }
BASE="http://127.0.0.1:$PORT"

new_proj() {
    p="$TMP/proj_$1"; mkdir -p "$p/home"
    printf '[package]\nname = "t"\nversion = "0.1.0"\n\n[dependencies]\n' > "$p/aether.toml"
    echo "$p"
}

# ---- Property 1: binary package installs lib + aether.toml, verified ------
P="$(new_proj ok)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v1.0.0" ) \
    >"$TMP/ok.log" 2>&1 || fail "binary-package install exited non-zero" "$TMP/ok.log"
grep -q "as a binary package" "$TMP/ok.log" || fail "did not report a binary-package install" "$TMP/ok.log"
grep -q "Checksum verified" "$TMP/ok.log" || fail "lib was not checksum-verified" "$TMP/ok.log"
INST="$P/home/.aether/packages/$PKG"
# The lib is staged under the RESOLVER-VISIBLE name <stem><ext> (NOT the full
# versioned asset name <stem>-<tag>-<triple><ext>): ae_find_binimport_so only
# probes lib<mod><ext> / <mod><ext>, so the versioned name would never resolve
# on `import`. This is the regression the end-to-end Property 5 guards.
[ -f "$INST/$STEM$EXT" ] || fail "the lib was not installed under its resolver-visible name $STEM$EXT" "$TMP/ok.log"
[ -f "$INST/$STEM-v1.0.0-$TRIPLE$EXT" ] && fail "the lib was installed under the full versioned asset name; import cannot resolve it"
[ -f "$INST/aether.toml" ] || fail "the aether.toml was not installed beside the lib" "$TMP/ok.log"

# ---- Property 2: binary declared but no lib for this host is FATAL --------
P="$(new_proj nolib)"
if ( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v2.0.0" ) \
        >"$TMP/nolib.log" 2>&1; then
    fail "a binary package with no lib for this host should have failed" "$TMP/nolib.log"
fi
grep -q "declares a binary package but publishes no" "$TMP/nolib.log" \
    || fail "missing-lib error did not name the missing asset" "$TMP/nolib.log"
[ -d "$P/home/.aether/packages/$PKG" ] && fail "a failed binary-package install left files behind"

# ---- Property 3: no `binary` key is NOT a binary package -----------------
P="$(new_proj nobinkey)"
# No lib is declared, so ae add must NOT treat it as a binary package; it falls
# through to the archive path and then to git (which fails on the fake forge).
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v3.0.0" ) \
    >"$TMP/nobin.log" 2>&1
grep -q "as a binary package" "$TMP/nobin.log" \
    && fail "an aether.toml with no binary key was wrongly installed as a binary package" "$TMP/nobin.log"

# ---- Property 4: a mismatched lib checksum is fatal ----------------------
P="$(new_proj badsum)"
if ( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v4.0.0" ) \
        >"$TMP/badsum.log" 2>&1; then
    fail "a mismatched lib checksum should have failed" "$TMP/badsum.log"
fi
grep -qi "checksum MISMATCH" "$TMP/badsum.log" || fail "mismatched checksum was not reported" "$TMP/badsum.log"
[ -d "$P/home/.aether/packages/$PKG" ] && fail "a mismatched-checksum install left files behind"

# ---- Property 4b: a MISSING .sha256 on a binary package is FATAL ----------
# A binary package REQUIRES a published checksum (stricter than the archive
# path, which warns and installs unverified): a raw downloaded shared library
# must be verifiable (#2105).
P="$(new_proj nosum)"
if ( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v5.0.0" ) \
        >"$TMP/nosum.log" 2>&1; then
    fail "a binary package with no .sha256 should have been refused" "$TMP/nosum.log"
fi
grep -qi "refusing to install an" "$TMP/nosum.log" \
    || fail "missing-checksum was not refused with the expected message" "$TMP/nosum.log"
[ -d "$P/home/.aether/packages/$PKG" ] && fail "a no-checksum install left files behind"

# ---- Property 4c: --target fetches a FOREIGN platform's binary ------------
# `--target <triple>` names another platform's lib (for cross-platform bundling),
# using the triple's OS extension, not the host's. It installs it; and --target
# on a package that isn't a binary package is an error, not a host fall-through.
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) FT="macos-arm64" ; FE=".dylib" ;;
    *)            FT="linux-x86_64"; FE=".so" ;;
esac
P="$(new_proj target)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v1.0.0" --target "$FT" ) \
    >"$TMP/target.log" 2>&1 || fail "--target fetch of a foreign binary failed" "$TMP/target.log"
[ -f "$P/home/.aether/packages/$PKG/$STEM$FE" ] \
    || fail "--target did not install the foreign lib under $STEM$FE" "$TMP/target.log"
# --target with --source is rejected
if ( cd "$(new_proj tconf)" && HOME="$TMP/proj_tconf/home" AE_RELEASE_BASE_URL="$BASE" \
        "$AE" add "$PKG@v1.0.0" --target "$FT" --source ) >"$TMP/tconf.log" 2>&1; then
    fail "--target --source together should be rejected" "$TMP/tconf.log"
fi
grep -qi "mutually exclusive" "$TMP/tconf.log" || fail "--target/--source conflict not reported" "$TMP/tconf.log"

# ---- Property 5: END-TO-END — a real installed lib actually IMPORTS + RUNS -
# Properties 1-4 use a random payload (they exercise fetch/verify/install). This
# one builds a REAL importable shared lib, publishes it as a binary package, and
# confirms `ae run` can `import` it after `ae add` — the whole point of the
# feature. It is the regression guard for the install-name bug: staging the lib
# under its full versioned asset name installs fine but never resolves on import
# (ae_find_binimport_so probes lib<mod><ext> / <mod><ext> only).
MOD="binpkgmod"
BSTEM="lib$MOD"     # stem carries the lib prefix, like a conventional shared lib
BREL="$FORGE/$PKG/releases/download/v5.0.0"
mkdir -p "$BREL" "$TMP/libsrc"
printf 'exports(add)\nadd(a: int, b: int) -> int { return a + b }\n' > "$TMP/libsrc/$MOD.ae"
if ! "$AE" build --emit=lib "$TMP/libsrc/$MOD.ae" -o "$TMP/libsrc/$BSTEM$EXT" >"$TMP/libbuild.log" 2>&1; then
    echo "  [SKIP] ae_add_binary_package: could not build a lib for the e2e case"; exit 0
fi
printf '[package]\nname = "pkg"\nmodules = "."\nbinary = "%s"\n' "$BSTEM" > "$BREL/aether.toml"
BASSET="$BSTEM-v5.0.0-$TRIPLE$EXT"
cp "$TMP/libsrc/$BSTEM$EXT" "$BREL/$BASSET"
( cd "$BREL" && $SHA "$BASSET" | awk -v n="$BASSET" '{print $1"  "n}' > "$BASSET.sha256" )

P="$(new_proj e2e)"
printf 'import %s\nmain() {\n    println("sum=${%s.add(2, 40)}")\n    return 0\n}\n' "$MOD" "$MOD" > "$P/main.ae"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v5.0.0" ) \
    >"$TMP/e2e_add.log" 2>&1 || fail "e2e binary-package add failed" "$TMP/e2e_add.log"
E2E="$P/home/.aether/packages/$PKG"
[ -f "$E2E/$BSTEM$EXT" ] || fail "e2e lib not staged under $BSTEM$EXT" "$TMP/e2e_add.log"
RUN=$( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" run main.ae 2>&1 )
case "$RUN" in
    *sum=42*) ;;
    *) echo "  [FAIL] ae_add_binary_package: installed lib did not IMPORT+RUN (the install-name bug)"
       printf '%s\n' "$RUN" | sed 's/^/        /'
       exit 1 ;;
esac

echo "  [PASS] ae_add_binary_package: install+import+run, require-checksum, --target foreign fetch, no fallback ladder"
exit 0
