/* The C entry point of an --emit=obj object, on Windows (MinGW).
 *
 * Elsewhere the object carries this as a WEAK main() (emit_lib_weak_main in
 * compiler/codegen/codegen.c): a plain `cc app.o $(ae cflags --libs)` links a
 * working program, and a host's own main() wins. A COFF weak definition does
 * not do that with every GNU ld: up to binutils 2.46 at least, the weak main
 * left the symbol open to archive search, the linker pulled libmingw32.a's
 * crtexewin.o (a strong main() that calls WinMain) and the link failed with
 * `undefined reference to 'WinMain'`.
 *
 * So on Windows the entry is this one archive member, libaether_main.a, which
 * `ae cflags --libs` names ahead of -laether and of the compiler's own
 * -lmingw32. An archive member is linked only when something still needs the
 * symbol it defines: a program with no main() of its own gets this one; a
 * host that brings its own main() never pulls it. That is the weak main's
 * behaviour with nothing left to a linker's handling of weak externals.
 *
 * Not part of libaether.a: the shared runtime (aether.dll) is linked from the
 * whole archive, and must define no main. */
int aether_main(int argc, char** argv);
void aether_main_exit(void);

int main(int argc, char** argv) {
    int rc = aether_main(argc, argv);
    aether_main_exit();
    return rc;
}
