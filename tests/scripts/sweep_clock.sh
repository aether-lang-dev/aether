# Sourced by the sweep's runners (run_ae_test.sh, run_ae_sh_dir.sh) for the
# per-test timings tests/scripts/sweep_timings.sh reports (#2673).
#
# sweep_clock sets $sweep_ms to milliseconds since an arbitrary epoch, or to
# nothing where this shell has no cheap clock, and the timing is then left
# out. bash's EPOCHREALTIME starts no process, which is what matters on
# Windows, where starting one costs 15 to 40 ms and the sweep is already the
# slowest there. Elsewhere GNU date gives milliseconds (dash, the Linux sh,
# has no EPOCHREALTIME); BSD date (macOS) does not, and prints a literal
# "3N", which the check below refuses.
sweep_clock() {
    if [ -n "${EPOCHREALTIME:-}" ]; then
        # Seconds, then the fraction's first three digits. The separator is
        # the locale's decimal point in some bash versions, so either one.
        _sweep_s=${EPOCHREALTIME%[.,]*}
        _sweep_f=${EPOCHREALTIME#*[.,]}
        _sweep_f=${_sweep_f%???}
        # A leading zero would make the arithmetic below read it as octal.
        while :; do
            case $_sweep_f in
                0?*) _sweep_f=${_sweep_f#0} ;;
                *) break ;;
            esac
        done
        sweep_ms=$(( _sweep_s * 1000 + _sweep_f ))
    else
        sweep_ms=$(date +%s%3N 2>/dev/null)
        case $sweep_ms in
            ''|*[!0-9]*) sweep_ms= ;;
        esac
    fi
}
