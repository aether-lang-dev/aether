/* LD_PRELOAD shim for the contrib LeakSanitizer gate: make dlclose a no-op.
 *
 * LSan symbolizes leak stacks at exit, and the Vulkan loader unloads the ICD
 * before then. Every driver-owned allocation therefore reported as
 *
 *     #1 0x7ffffd859f48  (<unknown module>)
 *
 * which no `leak:<module>` suppression can match: the frame no longer belongs
 * to any mapped object. Keeping the modules mapped is what makes suppression
 * by module possible at all, so this is load-bearing for the gate rather than
 * a convenience.
 *
 * Preloaded, not linked in: the binary under test stays exactly the one the
 * correctness leg runs. Leaving a library mapped at process exit changes
 * nothing else, the process is about to end.
 *
 * Build: cc -shared -fPIC -o lsan_keep_modules.so lsan_keep_modules.c
 */
int dlclose(void* handle) {
    (void)handle;
    return 0;
}
