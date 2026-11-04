#include "aether_lsp.h"
#include "aether_compile_stack.h"
#include <stdio.h>

static int lsp_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    LSPServer* server = lsp_server_create();
    if (!server) {
        fprintf(stderr, "aether-lsp: out of memory\n");
        return 1;
    }
    lsp_server_run(server);
    lsp_server_free(server);
    return 0;
}

/* The server compiles documents in this process, on the compiler's own
 * stack (aether_compile_stack.h), as aetherc does. */
int main(int argc, char** argv) {
    return aether_run_on_compile_stack(lsp_main, argc, argv);
}
