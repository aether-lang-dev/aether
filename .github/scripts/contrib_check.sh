#!/usr/bin/env bash
# contrib_check.sh — build + RUN every contrib test_*.ae (not just type-check).
#
# The nightly used to only `ae check` (type-check) contrib modules — which is
# exactly what failed to catch the tinyweb WebSocket server rot (it compiled
# fine; it was runtime-broken). This runs the actual tests, so runtime and (with
# VALGRIND=1) leak regressions surface.
#
# Usage:
#   .github/scripts/contrib_check.sh            # build + run each test
#   VALGRIND=1 .github/scripts/contrib_check.sh # ... under valgrind (leak gate)
#   LSAN=1 .github/scripts/contrib_check.sh     # LeakSanitizer gate (vulkan)
#
# Assumes ./build/ae is already built (the Makefile target depends on it).
# Exits nonzero if any contrib test fails to build, fails at runtime, or (under
# valgrind) leaks. Each test binary is expected to self-terminate — the tinyweb
# server tests run an in-process client+server and exit(0)/exit(1) on their own.
set -u

AE="./build/ae"
EXE_EXT="${EXE_EXT:-}"
VALGRIND="${VALGRIND:-0}"
LSAN="${LSAN:-0}"
VG="valgrind --leak-check=full --error-exitcode=99 --errors-for-leak-kinds=definite"

# LeakSanitizer gate (leakgate = "lsan"). Standalone LSan, not ASan: ASan's
# memcpy interceptor walks its shadow map, and lavapipe's LLVM JIT maps code
# outside it, so an ASan build dies with SEGV inside RuntimeDyld before the
# first draw. LSan alone intercepts the allocators and checks at exit, which
# the JIT does not disturb.
LSAN_CFLAGS="-fsanitize=leak -fno-omit-frame-pointer -g"
LSAN_SUPP="$(pwd)/.github/scripts/lsan-contrib.supp"
LSAN_KEEP_SRC="$(pwd)/.github/scripts/lsan_keep_modules.c"
LSAN_KEEP_SO="$(pwd)/build/contrib-check/lsan_keep_modules.so"

rc=0
run_dir="build/contrib-check"
mkdir -p "$run_dir"

# Each entry: "<label>|<test.ae>|<extra C files>|<leakgate>".
#   leakgate = "leak" : under VALGRIND=1 this test is also a LEAK gate.
#   leakgate = "lsan" : under LSAN=1 this test is built with LeakSanitizer and
#                       gated on it (see the vulkan block for why not valgrind).
#   leakgate = "run"  : run for correctness only; excluded from every leak gate.
# Extra C files are passed via --extra. Keep this table in sync as contrib
# modules gain tests.
#
# Why some tests are run-only under valgrind: the tinyweb tests were written to
# `exit()` on completion (and the server tests keep an actor blocked in an
# accept loop), so the process is torn down without running scope-end heap
# cleanup — valgrind reports the still-live buffers as "leaks" even though the
# code is correct. Those tests are gated for RUNTIME correctness (the thing that
# actually caught the WS rot); making them leak-clean is separate follow-up
# work. i18n/collate was written leak-clean by design, so it IS leak-gated.
AVC="contrib/avcodec"
TW="contrib/tinyweb"
I18N="contrib/i18n"
VK="contrib/vulkan"
SQL="contrib/sqlite"
NT="contrib/templating/native"
XE="contrib/parsers/xml_expat"
LQ="contrib/templating/liquid"
# <label>|<test .ae>|<extra C sources>|<leakgate>|<pkg-config link>|<pkg-config headers>
#
# Column 5 is optional and names the pkg-config modules the test needs to
# BUILD against, both --cflags and --libs. It exists because `ae build --extra
# foo.c` compiles the shim but has no way to pass -I or -l flags, so a module
# needing a system library compiles and then dies at link. When either column
# is set the runner stages an aether.toml workspace (the shape
# contrib/sqlite/test_sqlite.ae needs) so the flags reach gcc, and SKIPS
# the entry when pkg-config cannot find the modules: an absent system library
# is a provisioning gap, not a test failure.
TESTS=(
  # avcodec: needs FFmpeg's dev libraries to LINK and the ffmpeg BINARY to
  # generate its clip; the test itself SKIPs cleanly without the latter.
  "avcodec/decode|$AVC/test_avcodec.ae|$AVC/aether_avcodec.c|run|libavcodec libavformat libavutil libswscale libswresample"
  # sqlite: co-located spec (replaces tests/integration/sqlite_{roundtrip,
  # prepared}/, whose shell wrappers existed mainly to probe for libsqlite3
  # and skip). Column 5 makes the runner stage a workspace so -I/-l reach
  # gcc, and SKIP the entry when pkg-config cannot find sqlite3.
  "sqlite/roundtrip|$SQL/test_sqlite.ae|$SQL/aether_sqlite.c|run|sqlite3"
  # templating/native: co-located spec (replaces the four
  # tests/integration/native_templating_* dirs). Pure Aether — no system
  # library, so no pkg-config column and nothing to skip on.
  "templating/native|$NT/test_native.ae||run|"
  # parsers/xml_expat: co-located spec (replaces
  # tests/integration/contrib_xml_expat/). Needs libexpat to link, so
  # column 5 stages the workspace and SKIPs when pkg-config cannot find it.
  "parsers/xml_expat|$XE/test_xml_expat.ae|$XE/aether_xml_expat.c|run|expat"
  # templating/liquid: five co-located specs grouped by feature, replacing
  # 22 tests/integration/liquid_*/ dirs. Pure Aether, no system library.
  # (liquid_sandbox_gate stays a .sh — it drives aetherc and asserts on
  # compiler diagnostics, which a spec cannot do.)
  "templating/liquid/syntax|$LQ/test_syntax.ae||run|"
  "templating/liquid/values|$LQ/test_values.ae||run|"
  "templating/liquid/tags|$LQ/test_tags.ae||run|"
  "templating/liquid/filters|$LQ/test_filters.ae||run|"
  "templating/liquid/inheritance|$LQ/test_inheritance.ae||run|"
  "tinyweb/spec|$TW/test_spec.ae||run|"
  "tinyweb/inventory|$TW/test_inventory.ae|$TW/ws_handshake.c|run|"
  "tinyweb/integration|$TW/test_integration.ae|$TW/ws_handshake.c|run|"
  "tinyweb/schema_api|$TW/test_schema_api.ae|$TW/ws_handshake.c|run|"
  "tinyweb/websocket|$TW/test_websocket.ae|$TW/ws_handshake.c|run|"
  "i18n/collate|$I18N/collate/test_collate.ae|$I18N/aether_i18n.c $I18N/utf8proc/utf8proc.c $I18N/ducet/ducet_data.c|leak|"
  # vulkan: needs only the HEADERS to build (the loader is opened at runtime),
  # and SKIPs itself at runtime when no driver is installed.
  #
  # Leak-gated under LSAN=1, not under valgrind. The CI driver is lavapipe,
  # whose LLVM JIT valgrind cannot follow: a single render reports ~13k errors
  # from ~1000 contexts, all inside libvulkan and the driver's own worker
  # threads, and the "definitely lost" total moves run to run. Gating on that
  # would measure Mesa. LSan suppresses by MODULE instead, so the driver's
  # allocations are excluded by object while every allocation this repo makes
  # is still gated: 464 bytes of driver noise, and a deliberate malloc in
  # aether_vulkan.c fails the leg with the function and line named.
  "vulkan/offscreen|$VK/test_vulkan.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/resources|$VK/test_vulkan_resources.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/actors|$VK/test_vulkan_actors.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/depth-msaa|$VK/test_vulkan_depth_msaa.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/frames|$VK/test_vulkan_frames.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/materials|$VK/test_vulkan_materials.ae|$VK/aether_vulkan.c|lsan||vulkan"
  # The examples are RUN, not just compiled. An example that only builds
  # rots into decoration: both of these render and write a PPM, so a
  # regression that leaves them producing nothing fails here.
  "vulkan/example-triangle|$VK/example_triangle.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/example-parallel|$VK/example_parallel_render.ae|$VK/aether_vulkan.c|lsan||vulkan"
  "vulkan/example-sprites|$VK/example_sprites.ae|$VK/aether_vulkan.c|lsan||vulkan"
)

# Kill any stray cache/test binaries squatting ports before we start (aborted
# server tests orphan busy-looping binaries — the hidden cause of bind flakes).
reap_orphans() {
  pkill -9 -f 'contrib-check/' 2>/dev/null || true
}
reap_orphans

# The LSan gate needs its dlclose shim (see lsan_keep_modules.c: without it the
# driver is unloaded before the leak check and its frames resolve to
# <unknown module>, which no module suppression can match).
if [ "$LSAN" = "1" ]; then
  if ! ${CC:-cc} -shared -fPIC -o "$LSAN_KEEP_SO" "$LSAN_KEEP_SRC" 2>/dev/null; then
    echo "  FAIL  lsan gate: cannot build $LSAN_KEEP_SRC"
    exit 1
  fi
fi

for entry in "${TESTS[@]}"; do
  IFS='|' read -r label src extras leakgate pcmods pchdrs <<< "$entry"

  if [ ! -f "$src" ]; then
    printf '  SKIP  %-22s (%s not found)\n' "$label" "$src"
    continue
  fi

  # LSAN=1 is a focused leak leg: it builds the flagged entries with
  # LeakSanitizer and runs only those. Everything else is already covered by
  # the plain and valgrind legs, and rebuilding it here would only cost time.
  use_lsan=0
  if [ "$LSAN" = "1" ]; then
    if [ "$leakgate" != "lsan" ]; then
      continue
    fi
    use_lsan=1
  fi

  safe="$(echo "$label" | tr '/' '_')"
  # Absolute: the program runs from its own scratch directory, so a relative
  # path would resolve there instead of beside the other build output.
  out="$(pwd)/$run_dir/${safe}${EXE_EXT}"
  log="$(pwd)/$run_dir/${safe}.log"
  # assemble --extra flags
  extra_flags=""
  for c in $extras; do extra_flags="$extra_flags --extra $c"; done

  if [ -n "$pcmods" ] || [ -n "$pchdrs" ] || [ "$use_lsan" = "1" ]; then
    # Needs system libraries. Skip rather than fail when they are absent —
    # a missing FFmpeg is a provisioning gap on this box, not a code defect.
    if [ -n "$pcmods$pchdrs" ] && ! pkg-config --exists $pcmods $pchdrs 2>/dev/null; then
      printf '  SKIP  %-22s (pkg-config: %s not found)\n' "$label" "$pcmods $pchdrs"
      continue
    fi
    # pkg-config existing does NOT prove the headers are installed: split
    # packagings ship the .pc with the loader and the headers separately
    # (Arch: vulkan-icd-loader provides vulkan.pc, vulkan-headers the
    # includes — every vulkan test FAILed at build on such a box instead
    # of SKIPping). Prove an include of each module's headers actually
    # preprocesses before committing to a build.
    hdr_probe_ok=1
    for m in $pcmods $pchdrs; do
      case "$m" in
        vulkan)        probe_inc='#include <vulkan/vulkan.h>' ;;
        libavcodec)    probe_inc='#include <libavcodec/avcodec.h>' ;;
        libavformat)   probe_inc='#include <libavformat/avformat.h>' ;;
        libavutil)     probe_inc='#include <libavutil/avutil.h>' ;;
        libswscale)    probe_inc='#include <libswscale/swscale.h>' ;;
        libswresample) probe_inc='#include <libswresample/swresample.h>' ;;
        *)             probe_inc='' ;;
      esac
      [ -n "$probe_inc" ] || continue
      if ! printf '%s\n' "$probe_inc" |            ${CC:-cc} -E -x c - $(pkg-config --cflags "$m" 2>/dev/null) >/dev/null 2>&1; then
        printf '  SKIP  %-22s (%s: .pc present but headers missing)\n' "$label" "$m"
        hdr_probe_ok=0
        break
      fi
    done
    [ "$hdr_probe_ok" = "1" ] || continue
    # `ae build --extra` cannot pass -l flags, so stage a workspace whose
    # aether.toml carries them; ae threads link_flags into gcc via
    # get_link_flags(). This is what the sqlite entry relies on.
    # A module whose module.ae carries `@link("-laether_<mod> ...")` needs
    # that veneer archive on the link line — `make contrib` normally builds
    # it, but this script may run without one (CI does). Build it on demand
    # so the entry does not depend on a leftover artifact. Failure is not
    # fatal here: the link below reports it properly if the archive really
    # was required.
    veneer="$(sed -n 's/.*@link("-laether_\([a-z0-9_]*\).*/\1/p' \
              "$(dirname "$src")/module.ae" 2>/dev/null | head -1)"
    if [ -n "$veneer" ] && [ ! -f "build/contrib/libaether_$veneer.a" ]; then
      MODULES="$veneer" bash tests/scripts/contrib_build.sh >/dev/null 2>&1 || true
    fi
    work="$run_dir/${safe}.work"
    rm -rf "$work"; mkdir -p "$work"
    ln -s "$(pwd)/contrib" "$work/contrib"
    cp "$src" "$work/probe.ae"
    extra_toml=""
    for c in $extras; do extra_toml="$extra_toml\"$c\", "; done
    {
      printf '[project]\nname = "%s"\nversion = "0.0.0"\n\n' "$safe"
      printf '[[bin]]\nname = "probe"\npath = "probe.ae"\n'
      printf 'extra_sources = [%s]\n\n' "${extra_toml%, }"
      # The sanitizer flags go on both lines: LSan has to be linked in as well
      # as compiled with, or the interceptors are never installed.
      san_c=""; san_l=""
      if [ "$use_lsan" = "1" ]; then san_c=" $LSAN_CFLAGS"; san_l=" -fsanitize=leak"; fi
      printf '[build]\nlink_flags = "%s%s"\n' "${pcmods:+$(pkg-config --libs $pcmods)}" "$san_l"
      printf 'cflags = "%s%s"\n' "${pcmods:+$(pkg-config --cflags $pcmods)}${pchdrs:+ $(pkg-config --cflags $pchdrs)}" "$san_c"
    } > "$work/aether.toml"
    abs_out="$out"; abs_ae="$(pwd)/${AE#./}$EXE_EXT"
    if ! berr="$( cd "$work" && "$abs_ae" build probe.ae -o "$abs_out" 2>&1 )"; then
      printf '  FAIL  %-22s (build)\n' "$label"
      echo "$berr" | grep -iE "error" | head -5
      rc=1
      continue
    fi
  elif ! berr="$("$AE$EXE_EXT" build "$src" $extra_flags -o "$out" 2>&1)"; then
    printf '  FAIL  %-22s (build)\n' "$label"
    echo "$berr" | grep -iE "error" | head -5
    rc=1
    continue
  fi

  # Run (optionally under valgrind), detached from any tty, with a hard timeout
  # so a hung server test can never wedge the nightly. Valgrind applies only to
  # tests flagged as a leak gate (see the table's leakgate column).
  use_vg=0
  [ "$VALGRIND" = "1" ] && [ "$leakgate" = "leak" ] && use_vg=1
  if [ "$use_vg" = "1" ]; then
    runner="$VG $out"
  else
    runner="$out"
  fi
  # LSan is linked into the binary rather than wrapped around it, so the gate
  # is environment only: the suppression list, and the dlclose shim that keeps
  # the driver mapped long enough for those suppressions to match a module.
  # exitcode=23 distinguishes "leaked" from the program's own failure codes.
  lsan_env=""
  if [ "$use_lsan" = "1" ]; then
    lsan_env="LD_PRELOAD=$LSAN_KEEP_SO LSAN_OPTIONS=suppressions=$LSAN_SUPP:print_suppressions=1:exitcode=23"
  fi
  # Run from a scratch directory that mirrors the repo through symlinks.
  # The programs resolve inputs by relative path (shaders, fixtures), so the
  # tree has to look the same; what must NOT be shared is the working
  # directory, or every example that writes an output file drops it in the
  # source tree. Rebuilt per entry so one test cannot see another's output.
  rundir="$run_dir/${safe}.rundir"
  rm -rf "$rundir"; mkdir -p "$rundir"
  for top in */; do
    top="${top%/}"
    [ "$top" = "build" ] && continue
    ln -s "$(pwd)/$top" "$rundir/$top" 2>/dev/null || true
  done
  if ( cd "$rundir" && env $lsan_env timeout 120 $runner > "$log" 2>&1 < /dev/null ); then
    if [ "$use_vg" = "1" ]; then
      printf '  PASS  %-22s (run + valgrind)\n' "$label"
    elif [ "$use_lsan" = "1" ]; then
      # Report what was suppressed, so the driver's share stays visible rather
      # than becoming an invisible allowance. Parsed from LSan's own
      # "Suppressions used" block, not from the log at large.
      supp="$(awk '/Suppressions used:/ {inblock=1; next}
                   inblock && /^-+$/ {inblock=0}
                   inblock && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {printf "%s %sB ", $3, $2}' "$log")"
      printf '  PASS  %-22s (run + lsan%s)\n' "$label" "${supp:+; suppressed: ${supp% }}"
    elif [ "$VALGRIND" = "1" ]; then
      printf '  PASS  %-22s (run; leak-gate n/a)\n' "$label"
    else
      printf '  PASS  %-22s (run)\n' "$label"
    fi
  else
    code=$?
    if [ "$code" = "99" ]; then
      printf '  FAIL  %-22s (valgrind: leak/error)\n' "$label"
      grep -E "definitely lost|ERROR SUMMARY" "$log" | tail -3
    elif [ "$code" = "23" ] && [ "$use_lsan" = "1" ]; then
      printf '  FAIL  %-22s (lsan: leak)\n' "$label"
      # The frames name the function and line; print enough of them to act on.
      sed -n '/LeakSanitizer: detected memory leaks/,$p' "$log" | head -25
    elif [ "$code" = "124" ]; then
      printf '  FAIL  %-22s (timeout — did not terminate)\n' "$label"
    else
      printf '  FAIL  %-22s (run, exit %s)\n' "$label" "$code"
      grep -iE "fail" "$log" | head -5
    fi
    rc=1
  fi
  reap_orphans
done

vg_note=""
[ "$VALGRIND" = "1" ] && vg_note=" (valgrind)"
if [ "$rc" = "0" ]; then
  echo "contrib-check: all contrib tests passed${vg_note}"
else
  echo "contrib-check: one or more contrib tests FAILED"
fi
exit $rc
