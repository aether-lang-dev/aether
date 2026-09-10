#ifndef AETHER_LSP_H
#define AETHER_LSP_H

#include <stdio.h>
#include "../compiler/parser/parser.h"
#include "../compiler/analysis/typechecker.h"

// LSP Server state
typedef struct {
    FILE* input;
    FILE* output;
    FILE* log_file;
    int running;
    
    // Document state
    char** open_documents;
    char** document_contents;
    int document_count;
} LSPServer;

// LSP Server lifecycle
LSPServer* lsp_server_create(void);
void lsp_server_free(LSPServer* server);
void lsp_server_run(LSPServer* server);

// Document management
void lsp_document_open(LSPServer* server, const char* uri, const char* text);
void lsp_document_close(LSPServer* server, const char* uri);
void lsp_document_change(LSPServer* server, const char* uri, const char* text);
const char* lsp_document_get(LSPServer* server, const char* uri);

// LSP features
void lsp_handle_initialize(LSPServer* server, const char* id);
void lsp_handle_completion(LSPServer* server, const char* id, const char* uri, int line, int character);
void lsp_handle_hover(LSPServer* server, const char* id, const char* uri, int line, int character);
void lsp_handle_definition(LSPServer* server, const char* id, const char* uri, int line, int character);
void lsp_handle_document_symbol(LSPServer* server, const char* id, const char* uri);

// Diagnostics
void lsp_publish_diagnostics(LSPServer* server, const char* uri);

// JSON-RPC
typedef struct {
    char* method;
    char* id;
    char* params;
} JSONRPCMessage;

JSONRPCMessage* lsp_read_message(LSPServer* server);
void lsp_free_message(JSONRPCMessage* msg);
void lsp_send_response(LSPServer* server, const char* id, const char* result);
void lsp_send_error(LSPServer* server, const char* id, int code, const char* message);
void lsp_send_notification(LSPServer* server, const char* method, const char* params);

// Utility
void lsp_log(LSPServer* server, const char* format, ...);

#endif // AETHER_LSP_H

