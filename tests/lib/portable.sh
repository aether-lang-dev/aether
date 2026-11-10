# Small POSIX stand-ins for tools that are not the same everywhere.
#
# A test script that reaches for `head -c`, `shasum` or `sha256sum` runs
# fine on Linux and macOS and dies on OpenBSD (`head: unknown option -- c`,
# no `shasum`), and the failure reads as a product bug. Source this file and
# call the function; each one prefers the real tool and only falls back when
# the platform lacks it.

# head_c N [FILE]: the first N bytes of FILE, or of stdin.
if printf 'ab' | head -c 1 >/dev/null 2>&1; then
    head_c() { if [ -n "$2" ]; then head -c "$1" "$2"; else head -c "$1"; fi; }
else
    # dd with bs=1 reads exactly N bytes from a pipe or a device, where a
    # larger block size would take whatever one short read returned.
    head_c() {
        if [ -n "$2" ]; then dd if="$2" bs=1 count="$1" 2>/dev/null
        else dd bs=1 count="$1" 2>/dev/null; fi
    }
fi

# sha256_of FILE: the lowercase hex SHA-256 of FILE, nothing else.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v sha256 >/dev/null 2>&1; then sha256 -q "$1"
    else openssl dgst -sha256 "$1" | sed 's/^.*= *//'; fi
}

# copy_listed DESTDIR: copy the files named on stdin (one path per line,
# relative to the current directory) into DESTDIR under the same relative
# paths. One tar process where tar can take a list (GNU, libarchive), and a
# per-file mkdir+cp where it cannot: OpenBSD's tar has no -T.
if tar cf /dev/null -T /dev/null >/dev/null 2>&1; then
    copy_listed() { tar cf - -T - | (cd "$1" && tar xf -); }
else
    copy_listed() {
        while IFS= read -r _cl_f; do
            [ -n "$_cl_f" ] || continue
            mkdir -p "$1/$(dirname "$_cl_f")" && cp -p "$_cl_f" "$1/$_cl_f" || return 1
        done
    }
fi

# file_desc FILE: what `file -b` says about FILE, plus the words a test greps
# for when this platform's `file` does not know the format. OpenBSD's file
# prints "data" for a wasm module and a COFF object, and spells the machine
# "AArch64" where others print "aarch64". Appended words only add to what
# file said, so a platform whose file is complete sees no difference to a
# `case "$(file_desc f)" in *wasm*)` match.
file_desc() {
    _fd_d="$(file -b "$1" 2>/dev/null)"
    _fd_m="$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')"
    case "$_fd_m" in
        0061736d) case "$_fd_d" in *WebAssembly*) ;; *) _fd_d="$_fd_d WebAssembly (wasm) binary module" ;; esac ;;
        cffaedfe) case "$_fd_d" in *Mach-O*) ;; *) _fd_d="$_fd_d Mach-O 64-bit object" ;; esac ;;
        6486*)    case "$_fd_d" in *COFF*) ;; *) _fd_d="$_fd_d COFF object file, x86-64" ;; esac ;;
    esac
    case "$_fd_d" in *AArch64*) _fd_d="$_fd_d aarch64" ;; esac
    printf '%s\n' "$_fd_d"
}

# dyn_syms FILE: the symbols a shared library defines and exports, one per
# line, with whichever nm this platform has. GNU nm spells it `nm -D
# --defined-only`; macOS `nm -gU`; OpenBSD's nm has neither, and takes `-g`
# (external symbols), undefined ones included, so those are filtered out.
# An unsupported option makes nm exit non-zero with nothing on stdout, which
# must not be read as "the symbols are gone", hence the fall-through.
dyn_syms() {
    _ds_s="$(nm -D --defined-only "$1" 2>/dev/null || true)"
    [ -n "$_ds_s" ] || _ds_s="$(nm -gU "$1" 2>/dev/null || true)"
    [ -n "$_ds_s" ] || _ds_s="$(nm -g "$1" 2>/dev/null | grep -Ev '^[[:space:]]+[Uwv] |[[:space:]][Uwv][[:space:]]' || true)"
    # A reader that stops early (grep -q) closes the pipe: not an error here.
    { printf '%s\n' "$_ds_s"; } 2>/dev/null
}

# od_uint FILE OFFSET SIZE: the SIZE-byte (1, 2 or 4) unsigned integer at
# byte OFFSET of FILE, host byte order, as a plain decimal. OpenBSD's od
# zero-pads `-tu` fields ("0000000120"), which a shell reads as octal in
# `$(( ))` and as a different string in `[ "$x" = 2 ]`.
od_uint() { od -An -tu"$3" -j "$2" -N "$3" "$1" | awk 'NF { print $1 + 0; exit }'; }

# zig_musl_unavailable LOG: succeeds when LOG shows that zig could not build
# its own musl libc.a, which every `--target=*-linux-musl` link needs. That is
# the toolchain failing on this host, before any Aether code is involved:
# zig 0.16 on OpenBSD runs out of descriptors spawning its compiles
# (ProcessFdQuotaExceeded), or finds its cache half-written. A test whose
# point is something else (the module's header, a C source, an ELF check)
# reports a skip with this reason rather than a failure it did not cause.
zig_musl_unavailable() { grep -q "sub-compilation of musl libc.a failed" "$1" 2>/dev/null; }
