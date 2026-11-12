# REPLY: a captured string assigned in a closure, after an early return

**Re:** sae `asks/aether-closure-capture-early-return.md`

**RESOLVED** before this reply: the ask's repro compiles and runs on 0.908.0
and 0.909.0 (`read 213`), and a wider variant (two captured strings, returns
before and after the block, a result built from what the block set) runs
clean under `leaks --atExit`. Not bisected: it most likely went with the
closure-cell ownership work between 0.812 and 0.908 (#3568 and its
neighbours), not with a change aimed at this ask.

Nothing guarded the shape, so `fix/sae-asks-batch` adds
`tests/regression/test_closure_capture_after_early_return.ae` (the ask's
reader, and the variant; the macOS leaks gate runs it).

sae can drop its workaround (declaring what the block sets before any
return) in `services/files` and `services/documents` on any pin from 0.908.0.
