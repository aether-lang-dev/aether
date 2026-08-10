#!/usr/bin/env bash
# tests/cachyos/nightly.sh — self-run nightly HEAD build of Aether on a
# rolling-release Arch box (CachyOS: GCC 16 / Clang 22, ~5 GCC majors newer than
# the Ubuntu-22.04 GitHub CI box).
#
# NOT a GitHub Actions runner — the sibling of tests/freebsd/nightly.sh for a
# platform the CI matrix can't cover. Cron it (or a systemd --user timer, since
# CachyOS ships no crond) on a dedicated build box; it syncs origin/main to
# HEAD, runs `make ci` + contrib, type-checks AND runs every contrib module's
# tests (under valgrind where leak-clean), and — the thing GitHub CI never does
# — exercises the whole thing under a much newer toolchain. Surfaces -Werror
# promotions / new warnings + runtime & leak rot before they hit anyone.
# It publishes a topline pass/fail table to the `nightly-results` orphan branch.
#
# Install (systemd --user timer example):
#   ~/.config/systemd/user/aether-nightly.service  ->  ExecStart=<this script>
#   ~/.config/systemd/user/aether-nightly.timer    ->  OnCalendar=*-*-* 03:17:00
#   loginctl enable-linger $USER   (so the timer fires without an active login)
#
# POLICY: fail-on-missing-deps. A dedicated build box should test EVERYTHING, so
# a missing contrib dependency is a provisioning bug that turns the nightly RED
# (deliberately the opposite of contrib_build.sh's probe-and-skip). Provisioning:
#   - pacman -S --needed sqlite expat python ruby perl lua tcl duktape go nodejs
#       jdk-openjdk tinygo racket
#     (racket ships the CS embedding headers/lib/boot images — no source build)
#   - build the aether-lang-dev/factor-language fork: `./build.sh net-bootstrap`
#     (NOT `update`/`self-update` — those git-pull and fail on a shallow clone).
#     NB the fork's `libfactor.a` is actually an ELF shared object; symlink
#     `libfactor.so -> libfactor.a` for AETHER_FACTOR_SONAME.
#
# Toolchain + dep env (set in the timer/cron environment; see AETHER_* below):
#   AETHER_REPO, AETHER_RACKET_INCLUDE/_LIB/_BOOT_DIR,
#   AETHER_FACTOR_SONAME, AETHER_FACTOR_IMAGE
# The script pins PATH/AE/AETHERC/AETHER_HOME to the tree it builds, so a
# version-managed `ae` on PATH (e.g. /usr/local/bin) is never used.

set -u

REPO="${AETHER_REPO:-$HOME/scm/AetherThings/aether}"
OUTDIR="${AETHER_NIGHTLY_OUT:-$HOME/aether-nightly}"
STAMP="$(date +%Y-%m-%d_%H%M)"
LOG="$OUTDIR/nightly_${STAMP}.log"
SUMMARY="$OUTDIR/nightly_${STAMP}.summary.txt"
RESULTS_TSV="$OUTDIR/results_${STAMP}.tsv"
STEPS_TSV="$OUTDIR/steps_${STAMP}.tsv"
# Orphan branch (no shared history, no CI) used purely to publish topline
# results. Set AETHER_NIGHTLY_PUBLISH=0 to skip the push.
RESULTS_BRANCH="${AETHER_NIGHTLY_BRANCH:-nightly-results}"
RESULTS_REMOTE="${AETHER_NIGHTLY_REMOTE:-origin}"

mkdir -p "$OUTDIR"
cd "$REPO" || { echo "no repo at $REPO" > "$SUMMARY"; exit 1; }

# --- toolchain isolation --------------------------------------------------
# This box has a version-managed `ae`/`aetherc` on PATH (/usr/local/bin ->
# ~/.aether/versions/v0.417.0). The nightly must test the compiler IT builds,
# never that stale managed one. The pipeline already calls tools by absolute
# path ($REPO/build/ae), but belt-and-suspenders against any bare `ae`/`aetherc`
# a make recipe / contrib script / bridge build might invoke: put $REPO/build
# FIRST on PATH and export explicit AE/AETHERC pointing at the freshly-built
# binaries. AETHER_HOME is pinned to the repo so no ~/.aether managed state or
# a stray `current` symlink can redirect resolution.
export PATH="$REPO/build:$PATH"
export AE="$REPO/build/ae"
export AETHERC="$REPO/build/aetherc"
export AETHER_HOME="$REPO"

{
    echo "===================================================================="
    echo "Aether nightly — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host:   $(uname -a)"
    echo "distro: $(. /etc/os-release 2>/dev/null; echo "$PRETTY_NAME")"
    echo "gcc:    $(gcc --version | head -1)"
    echo "clang:  $(clang --version | head -1)"
    echo "===================================================================="
} > "$LOG"

# --- sync to HEAD (clean; never carry local state into a nightly) ---
# --tags matters: the Makefile takes the highest git TAG as the authoritative
# version (the VERSION file is only a tarball fallback), so a fetch without tags
# builds and stamps the PREVIOUS release even though the commits are current.
# Seen 2026-08-08: tree at 0.505.0's merge commit, newest local tag v0.504.0,
# binaries stamped 0.504.0 — which then reads as a stale build to anything
# comparing against the VERSION file.
git fetch -q --tags --prune origin 2>>"$LOG"
git checkout -q main 2>>"$LOG"
git reset -q --hard origin/main 2>>"$LOG"
git clean -qfdx -e 'aether-nightly*' 2>>"$LOG" || true
HEAD_LINE="$(git log --oneline -1)"
echo "HEAD: $HEAD_LINE" >> "$LOG"

# --- Dependency gate: this is a dedicated build box, so a MISSING contrib
# --- dependency is a provisioning FAILURE, not a skip. (Deliberately the
# --- opposite of contrib_build.sh / contrib_host_demos.sh, which probe-and-
# --- skip because portable CI runners can't have every language.) Any gap
# --- here turns the nightly RED and names what to install/build.
# A dep is satisfied if EITHER a command is on PATH OR a matching library/
# header exists (many bridges link a .so and never ship a CLI, e.g. duktape,
# sqlite, expat). Args after the kind are candidates; any one present = OK.
have_dep() {
    kind="$1"; shift
    case "$kind" in
        cmd) for c in "$@"; do command -v "$c" >/dev/null 2>&1 && return 0; done ;;
        lib) for l in "$@"; do
                 for d in /usr/lib /usr/lib64 /usr/local/lib /lib \
                          "/usr/lib/$(uname -m)-linux-gnu"; do
                     [ -e "$d/$l" ] && return 0
                     ls "$d/$l".* >/dev/null 2>&1 && return 0
                 done
             done ;;
        # A GENERATED locale. Spellings diverge on both axes: `locale -a`
        # prints de_DE.utf8 while /etc/locale.gen takes de_DE.UTF-8. Compare
        # with case folded AND '-' stripped so utf8/UTF-8 are the same string;
        # matching case-insensitively alone still misses the hyphen.
        locale) _avail=$(locale -a 2>/dev/null | tr 'A-Z' 'a-z' | tr -d '-')
                for L in "$@"; do
                    _want=$(printf '%s' "$L" | tr 'A-Z' 'a-z' | tr -d '-')
                    printf '%s\n' "$_avail" | grep -qx "$_want" && return 0
                done ;;
    esac
    return 1
}

require_deps() {
    missing=0
    # "<pkg>|<kind>|<candidate...>"  — kind: cmd (PATH), lib (shared object)
    # or locale (generated locale, per `locale -a`).
    set --                                              \
        "sqlite|lib|libsqlite3.so"                       \
        "expat|lib|libexpat.so"                          \
        "python|cmd|python3"                             \
        "ruby|cmd|ruby"                                  \
        "perl|cmd|perl"                                  \
        "lua|cmd|lua|lua5.4|lua5.3"                      \
        "tcl|cmd|tclsh"                                  \
        "duktape|lib|libduktape.so"                      \
        "ffmpeg|lib|libavcodec.so"                       \
        "ffmpeg-swr|lib|libswresample.so"                \
        "go|cmd|go"                                      \
        "nodejs|cmd|node"                                \
        "java|cmd|java"                                  \
        "tinygo|cmd|tinygo"                              \
        "racket|cmd|racket"                              \
        "de_DE.UTF-8 locale|locale|de_DE.utf8|de_DE.UTF-8"
    pkg_specs="$*"
    for spec in $pkg_specs; do
        pkg="${spec%%|*}"; rest="${spec#*|}"
        kind="${rest%%|*}"; cands="${rest#*|}"
        # candidates are '|'-joined; hand them to have_dep as separate args
        oldIFS="$IFS"; IFS='|'
        # shellcheck disable=SC2086
        set -- $cands
        IFS="$oldIFS"
        if have_dep "$kind" "$@"; then
            echo "  dep OK:   $pkg"
        else
            echo "  dep MISS: $pkg — none of [$cands] found ($kind)"
            missing=$((missing+1))
        fi
    done
    # Fork/source-built bridges: gated on their runtime env var being set.
    # "<label>:<ENVVAR>" — label first, envvar last (labels may have spaces).
    set --                                                        \
        "factor (aether-lang fork libfactor+image):AETHER_FACTOR_SONAME" \
        "racket-CS embedding tree:AETHER_RACKET_INCLUDE"
    for spec in "$@"; do
        label="${spec%:*}"; var="${spec##*:}"
        val="$(eval "printf '%s' \"\${$var:-}\"")"
        if [ -n "$val" ]; then
            echo "  dep OK:   $label ($var set)"
        else
            echo "  dep MISS: $label — $var unset (build required, see TODO)"
            missing=$((missing+1))
        fi
    done
    if [ "$missing" -gt 0 ]; then
        echo "  => $missing required contrib dependency(ies) MISSING — provisioning failure"
        return 1
    fi
    return 0
}

# Type-check every contrib/*/module.ae (plus any test/example .ae beside it).
# Returns nonzero if any module fails to compile — the contrib-rot guard.
# NOTE: `make ci` ends with clean/release-archive steps that wipe build/, so
# the `ae` binary is gone by the time this runs — (re)build it first.
contrib_ae_check() {
    local rc=0 mod out ae name t0 t1 want got
    ae="$PWD/build/ae"
    # `make ci` ends by wiping build/, so the sweep (re)builds ae from current
    # source before type-checking contrib.
    echo "  (building current ae for the sweep)"
    if ! make -j"$(nproc)" ae stdlib >/dev/null 2>>"$LOG"; then
        echo "  ae-check ABORT: could not build ae for the sweep"
        return 1
    fi
    if [ ! -x "$ae" ]; then
        echo "  ae-check ABORT: build/ae still absent after build"
        return 1
    fi
    # Staleness guard on the COMPILED version. Note `ae version` prints the
    # version MANAGER's active install (from ~/.aether/active_version), NOT the
    # compiled-in version — so it can read e.g. 0.417.0 for a correctly-built
    # 0.500.0 binary. Check the version string compiled into the binary instead
    # (VERSION-file value must appear in `strings build/ae`).
    want="$(cat VERSION 2>/dev/null)"
    if [ -n "$want" ] && ! strings "$ae" 2>/dev/null | grep -qxF "$want"; then
        echo "  ae-check ABORT: built ae does not carry VERSION=$want (stale build?)"
        return 1
    fi
    echo "  (sweep ae built at VERSION ${want:-unknown})"

    # Per-module topline row -> $RESULTS_TSV: "<module>\t<PASS|FAIL>\t<ms>".
    # One row per contrib MODULE (module.ae), not every helper .ae, so the
    # published table stays topline. publish_results() renders these to markdown.
    : > "$RESULTS_TSV"
    for mod in $(find contrib -name module.ae | sort); do
        name="$(dirname "${mod#contrib/}")"   # e.g. tinyweb, host/python
        t0="$(now_ms)"
        if out="$("$ae" check "$mod" 2>&1)"; then
            status="PASS"; echo "  ae-check OK: $mod"
        else
            status="FAIL"; rc=1
            echo "  ae-check FAIL: $mod"
            echo "$out" | grep -iE "error" | head -5
        fi
        t1="$(now_ms)"
        printf '%s\t%s\t%s\n' "$name" "$status" "$((t1 - t0))" >> "$RESULTS_TSV"
    done
    return $rc
}

# Render RESULTS_TSV to markdown and publish it to the orphan results branch.
# Uses git plumbing (hash-object/mktree/commit-tree) so it NEVER checks out or
# touches the working tree — safe to call from a repo that's mid-build. The
# branch keeps only the latest snapshot (single commit, force-pushed).
# Render both the top-level STEPS and the per-module sweep to markdown. The
# headline reflects the OVERALL run outcome (the `fails` counter across all
# steps), so a failed dependency gate / make ci turns the dashboard RED even
# when every module type-checks. Times are milliseconds.
render_row() { # <name> <PASS|FAIL> <ms>
    if [ "$2" = "PASS" ]; then
        echo "| \`$1\` | ✅ PASS | $3 |"
    else
        echo "| \`$1\` | ❌ FAIL | $3 |"
    fi
}

publish_results() {
    [ "${AETHER_NIGHTLY_PUBLISH:-1}" = "0" ] && { echo "  (publish skipped)"; return 0; }

    md="$OUTDIR/RESULTS_${STAMP}.md"
    {
        echo "# Aether contrib nightly — results"
        echo ""
        echo "Nightly HEAD build on CachyOS (newer GCC/Clang than the GitHub CI box)."
        echo "Orphan branch (\`$RESULTS_BRANCH\`, no shared history, no CI). Latest snapshot only."
        echo ""
        echo "- **run:** $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "- **HEAD:** $(git log --oneline -1)"
        echo "- **toolchain:** $(gcc --version | head -1) / $(clang --version | head -1)"
        echo "- **host:** $(. /etc/os-release 2>/dev/null; echo "$PRETTY_NAME"), kernel $(uname -r)"
        echo ""
        # Truthful headline: overall pass/fail is driven by `fails` (all steps),
        # NOT just the module sweep — that was the bug that published green on a
        # red run.
        if [ "${fails:-0}" -eq 0 ]; then
            echo "## ✅ ALL GREEN"
        else
            echo "## ❌ ${fails} step(s) FAILED"
        fi
        echo ""
        echo "### Pipeline steps"
        echo ""
        echo "| step | status | ms |"
        echo "|------|--------|----|"
        if [ -s "$STEPS_TSV" ]; then
            while IFS="$(printf '\t')" read -r n s t; do
                [ -z "$n" ] && continue
                render_row "$n" "$s" "$t"
            done < "$STEPS_TSV"
        fi
        echo ""
        echo "### Contrib module type-check (\`ae check\`)"
        echo ""
        echo "| module | status | ms |"
        echo "|--------|--------|----|"
        if [ -s "$RESULTS_TSV" ]; then
            # failures first, then passes, each alphabetical
            awk -F'\t' '$2=="FAIL"' "$RESULTS_TSV" | sort | while IFS="$(printf '\t')" read -r n s t; do
                render_row "$n" "FAIL" "$t"
            done
            awk -F'\t' '$2=="PASS"' "$RESULTS_TSV" | sort | while IFS="$(printf '\t')" read -r n s t; do
                render_row "$n" "PASS" "$t"
            done
        else
            echo "| _(sweep did not run)_ | ❌ FAIL | 0 |"
        fi
    } > "$md"

    # Commit the single file onto the orphan branch via plumbing, then push.
    blob="$(git hash-object -w "$md")"
    tree="$(printf '100644 blob %s\tRESULTS.md\n' "$blob" | git mktree)"
    parent=""
    # fetch current tip so we could parent onto it — but this branch is
    # snapshot-only, so we deliberately commit with NO parent (stays 1 commit).
    commit="$(printf 'chore(nightly): results %s\n' "$STAMP" | git commit-tree "$tree")"
    if git push -f "$RESULTS_REMOTE" "$commit:refs/heads/$RESULTS_BRANCH" 2>>"$LOG"; then
        echo "  results published to $RESULTS_REMOTE/$RESULTS_BRANCH"
    else
        echo "  results publish FAILED (see log) — run itself still reported below"
    fi
}

run_step() {
    name="$1"; shift
    echo "" >> "$LOG"
    echo "############### $name ###############" >> "$LOG"
    if "$@" >> "$LOG" 2>&1; then
        echo "  [OK]   $name"
        return 0
    else
        rc=$?
        echo "  [FAIL] $name (exit $rc)"
        return 1
    fi
}

# Wall-clock milliseconds (date +%s only has second resolution, which made every
# sub-second step report 0s — the "suspicious all-zero" symptom).
now_ms() { date +%s%3N; }

# run_and_record <label> <cmd...> — run a top-level step, time it in ms, append
# "<label>\t<PASS|FAIL>\t<ms>" to STEPS_TSV, and count failures in `fails`. This
# is what makes the published dashboard reflect the REAL outcome (gate/ci/etc.),
# not just the per-module sweep.
run_and_record() {
    label="$1"; shift
    t0="$(now_ms)"
    if run_step "$label" "$@"; then
        st="PASS"
    else
        st="FAIL"; fails=$((fails + 1))
    fi
    t1="$(now_ms)"
    printf '%s\t%s\t%s\n' "$label" "$st" "$((t1 - t0))" >> "$STEPS_TSV"
}

# Thin wrappers so make steps are named commands run_and_record can invoke.
make_ci_step() { make ci; }
make_contrib_step() { make contrib; }
make_host_check_step() { make contrib-host-check; }
# Build + RUN every contrib test_*.ae, under valgrind where the test is
# leak-clean by design (see .github/scripts/contrib_check.sh). This is the
# RUNTIME coverage the type-check sweep can't provide — the class of bug that
# let tinyweb's WebSocket server path rot (it type-checked fine, ran broken).
make_contrib_check_step() { make contrib-check-valgrind; }

# Refresh the $HOME/.local install from the tree we just built.
#
# Why this is a pipeline step and not a provisioning chore: a handful of tests
# link against the INSTALLED artifacts rather than the build tree —
# ci_coverage_smoke is the current one, and it skips outright when no install
# is present, so simply deleting the install would silence a test rather than
# fix it. Left unrefreshed, the install silently drifts behind the tree and
# fails with a mystery linker error instead:
#
#     undefined reference to `aether_unwind_forget'
#
# (Seen 2026-08-08: a Jul-19 install against an Aug-8 tree. The symbol had been
# added in between. The nightly log shows only "gcc --coverage link failed"
# without the linker's line, which makes it very easy to misread as a compiler
# bug on this box's newer GCC — it is not.)
#
# Refreshing every run makes that drift impossible by construction.
#
# PREFIX is under $HOME deliberately: this must never need sudo. The OTHER
# install on this box — the root-owned /usr/local/bin/ae that shadows PATH — is
# not ours to touch, which is exactly why the pinning block above puts
# $REPO/build first on PATH rather than trusting whatever is installed.
make_install_step() { make install PREFIX="$HOME/.local"; }

# Assert the refresh actually took. Deliberately separate from the install step
# so the summary distinguishes "install failed" from "install succeeded but
# produced something stale".
#
# Checks the COMPILED-IN version, exactly as contrib_ae_check does above, and
# for the same reason: `ae --version` prints the version MANAGER's active
# install (from ~/.aether/active_version), not the version compiled into the
# binary you just ran. On this box that state reads 0.417.0, so a correctly
# built and installed 0.504.0 binary still SAYS 0.417.0. Comparing against
# `ae --version` here would report STALE forever.
#
# Also verifies the archive, since that is what the drift actually broke:
# ci_coverage_smoke links $PREFIX/lib/aether/libaether.a, and a Jul-19 archive
# against an Aug-8 tree fails with `undefined reference to aether_unwind_forget`
# — a linker error that reads like a compiler bug if you do not know to look
# here. Comparing mtimes catches that class directly.
verify_install_step() {
    # Take the version the way the Makefile does — the highest git TAG is
    # authoritative, with the VERSION file only a tarball fallback. Reading the
    # VERSION file here instead would produce false STALE reports whenever the
    # sync fetched commits but not tags (`git pull` does not always bring tags),
    # since the build would legitimately stamp the older tag.
    want="$(cd "$REPO" && git tag -l 'v*.*.*' 2>/dev/null \
              | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
    [ -n "$want" ] || want="$(cat "$REPO/VERSION" 2>/dev/null)"
    installed_ae="$HOME/.local/bin/ae"
    installed_lib="$HOME/.local/lib/aether/libaether.a"

    if [ ! -x "$installed_ae" ] || [ ! -f "$installed_lib" ]; then
        echo "  install verify: no install at \$HOME/.local (ae and/or libaether.a missing)"
        return 1
    fi
    if [ -n "$want" ] && ! strings "$installed_ae" 2>/dev/null | grep -qxF "$want"; then
        echo "  install verify: installed ae does not carry VERSION=$want (stale install?)"
        return 1
    fi
    if [ "$REPO/build/libaether.a" -nt "$installed_lib" ]; then
        echo "  install verify: installed libaether.a is OLDER than the freshly built one"
        return 1
    fi
    echo "  install verify: \$HOME/.local carries VERSION ${want:-unknown}, archive current"
    return 0
}

fails=0
{
    echo "Aether nightly summary — $STAMP"
    echo "HEAD: $HEAD_LINE"
    echo "toolchain: $(gcc --version | head -1) / $(clang --version | head -1)"
    echo ""
    # Record each top-level step's outcome to STEPS_TSV ("<step>\t<PASS|FAIL>\t<secs>")
    # so the published dashboard reflects the REAL run outcome, not just the
    # per-module sweep. (A failed dependency gate must turn the dashboard red.)
    : > "$STEPS_TSV"
    run_and_record "contrib dependency gate" require_deps
    run_and_record "make ci" make_ci_step
    # Refresh $HOME/.local from the tree we just built, then prove it took.
    # Runs AFTER `make ci` (which builds everything) and BEFORE the sweeps, so
    # anything linking against the installed artifacts sees today's toolchain
    # rather than whatever was installed weeks ago. See make_install_step.
    run_and_record "make install (PREFIX=\$HOME/.local)" make_install_step
    run_and_record "install freshness check" verify_install_step
    run_and_record "make contrib" make_contrib_step
    run_and_record "make contrib-host-check" make_host_check_step
    # `make ci` and `make contrib` never compile the contrib Aether code
    # (test-ae globs only tests/*, `make contrib` builds C shims). This sweep
    # type-checks every contrib module under the newer toolchain — the gap that
    # let tinyweb's DSL rot (issue #1442).
    run_and_record "contrib ae-check sweep" contrib_ae_check
    # ...and this RUNS each contrib test (under valgrind where leak-clean),
    # catching runtime + leak rot that type-checking alone misses — the deeper
    # half of the same #1442 gap (the WS server path type-checked but was
    # runtime-broken until it was actually exercised).
    run_and_record "contrib test run (+valgrind)" make_contrib_check_step
    echo ""
    echo "--- publishing topline results to $RESULTS_REMOTE/$RESULTS_BRANCH ---"
    publish_results
    echo ""
    # Surface the signal Nic cares about most: new warnings/errors from the
    # newer toolchain, deduped, with counts.
    echo "--- warning/error lines (newer-toolchain signal) ---"
    grep -hoE "(error|warning):.*" "$LOG" 2>/dev/null \
        | sed -E 's/[0-9]+/N/g' | sort | uniq -c | sort -rn | head -40
    echo ""
    if [ "$fails" -eq 0 ]; then
        echo "RESULT: ALL GREEN on CachyOS HEAD"
    else
        echo "RESULT: $fails step(s) FAILED — see $LOG"
    fi
} > "$SUMMARY" 2>&1

# keep the last 14 nightlies, prune older
ls -1t "$OUTDIR"/nightly_*.log 2>/dev/null | tail -n +15 | xargs -r rm -f
ls -1t "$OUTDIR"/nightly_*.summary.txt 2>/dev/null | tail -n +15 | xargs -r rm -f
ls -1t "$OUTDIR"/results_*.tsv 2>/dev/null | tail -n +15 | xargs -r rm -f
ls -1t "$OUTDIR"/steps_*.tsv 2>/dev/null | tail -n +15 | xargs -r rm -f
ls -1t "$OUTDIR"/RESULTS_*.md 2>/dev/null | tail -n +15 | xargs -r rm -f

echo ""
echo "Summary written: $SUMMARY"
cat "$SUMMARY"
exit "$fails"
