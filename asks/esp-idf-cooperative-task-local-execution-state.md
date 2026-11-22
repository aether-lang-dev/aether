# Task-local Aether execution state for cooperative ESP-IDF hosts

2026-11-22. Scope: an explicit supported port contract for multiple FreeRTOS
tasks entering Aether. This is not a claim that today's single-worker S60 runner
is unsafe, nor a confirmed0.935 two-task runtime reproduction.

## Why this matters

The esp32 platform currently runs its RexxAE guest in a cooperative Aether
runtime. Networking/PSK service glue is native host code; guest effects are
injected and checked by the host. We would like to host a pure-Aether HTTP/TLS
service and an independently accounted RexxAE worker on separate FreeRTOS tasks,
without sharing panic/unwind state, execution accounts or outcomes.

An older downstream0.883 HTTPS experiment observed cross-task execution-account
state and failed trial cleanup with 8,260 bytes charged to the script account.
Its report is /Users/paul/scm/esp32/asks/aether-esp32-https.md. That observation
has not been revalidated on 0.935.0 and must not be presented as a fresh regression.

## Current source evidence

In the 0.935.0 release, runtime/utils/aether_compiler.h still defines AETHER_TLS
as empty under AETHER_NO_THREADING. Its comment correctly notes that ESP-IDF
allocates compiler thread-local storage on every FreeRTOS task's stack and that
a large panic-frame TLS block overflowed small system-task stacks.

The downstream firmware/esp-idf/main/CMakeLists.txt uses AETHER_NO_THREADING and
the released cooperative source manifest. Those settings are appropriate for
one Aether execution task. Removing the macro or enabling desktop threading is
not an established solution for a small embedded port.

## Ask

Provide and document a supported way for an ESP-IDF host to attach an Aether
execution context to each task that enters the runtime, without placing large
Aether TLS blocks on every unrelated Wi-Fi/IPC/system task's stack.

A port-supplied context accessor backed by a FreeRTOS TLS pointer slot is one
possible design; explicit context switching or a documented single-task
multiplexing API could also work. The runtime should own the state layout and
lifecycle. Do not make downstream hosts copy private runtime structs.

Cover execution-account selection and budgets, allocation charging, panic/unwind
frames, cancellation, last-outcome reporting and cleanup. Specify scheduler and
reentrancy constraints. A guest must not be able to select another task's
privileged context or inherit its capabilities.

## Acceptance

Run two independent FreeRTOS tasks in QEMU and on an ESP32-C3/S3 board. Charge
them to distinct accounts. Interrupt one through quota, deadline/cancellation
and panic paths while the other continues and completes. Check that outcomes,
allocations and cleanup never cross accounts, repeated attachment/detachment
does not leak, and non-Aether system tasks keep their small-stack budget.

If the supported architecture remains one Aether task, please document that
boundary and the supported integration pattern for a native network task. That
would still let downstream avoid relying on an undocumented concurrency model.
