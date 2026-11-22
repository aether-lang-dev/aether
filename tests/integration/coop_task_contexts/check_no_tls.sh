#!/bin/sh
# check_no_tls.sh FILE...: the objects and archives given define no
# thread-local symbol. For a cooperative build (AETHER_NO_THREADING): ESP-IDF
# puts every thread-local on every FreeRTOS task's stack, and a task's state
# belongs in its execution context (docs/design/task-execution-contexts.md).
# Prints each object that has one with its symbols and exits 1; exits 0 with
# nothing printed when there are none; exits 2 when this host has no tool to
# read them (readelf for ELF, nm -m for Mach-O).
tool=""
if command -v readelf > /dev/null 2>&1; then tool=readelf
elif [ "$(uname -s)" = Darwin ]; then tool=nm
else exit 2
fi
found=0
for f in "$@"; do
    if [ "$tool" = readelf ]; then
        # "File: lib.a(x.o)" heads each member of an archive.
        out="$(readelf -sW "$f" 2>/dev/null | awk -v f="$(basename "$f")" '
            /^File: / { f = $2; sub(/.*\(/, "", f); sub(/\)$/, "", f) }
            $4 == "TLS" && $7 != "UND" { s[f] = s[f] " " $8 }
            END { for (k in s) print k ":" s[k] }')"
    else
        # "lib.a(x.o):" heads each member; Mach-O keeps thread-locals in
        # __thread_vars (and their initial values in __thread_bss/data).
        out="$(nm -m "$f" 2>/dev/null | awk -v f="$(basename "$f")" '
            /^[^ ].*:$/ { f = $0; sub(/.*\(/, "", f); sub(/\)?:$/, "", f) }
            /__thread_vars/ && !/undefined/ && $NF !~ /^ltmp/ { s[f] = s[f] " " $NF }
            END { for (k in s) print k ":" s[k] }')"
    fi
    if [ -n "$out" ]; then printf '%s\n' "$out"; found=1; fi
done
[ $found -eq 0 ]
