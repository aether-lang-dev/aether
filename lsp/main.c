#include "aether_lsp.h"
#include <stdio.h>

int main(int argc, char** argv) {
    LSPServer* server = lsp_server_create();
    if (!server) {
        fprintf(stderr, "aether-lsp: out of memory\n");
        return 1;
    }
    lsp_server_run(server);
    lsp_server_free(server);
    return 0;
}

