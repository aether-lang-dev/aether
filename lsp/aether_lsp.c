#include "aether_lsp.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "../compiler/parser/lexer.h"
#include "../compiler/parser/parser.h"
#include "../compiler/ast.h"
#include "../runtime/utils/aether_compiler.h"

/* Extract a JSON string value for a given key from raw JSON content.
   Returns a newly allocated string or NULL. Caller must free. */
static char* json_extract_string(const char* json, const char* key) {
    if (!json || !key) return NULL;
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* start = strstr(json, search);
    if (!start) return NULL;
    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;
    if (*start != '"') return NULL;
    start++;
    /* Find closing quote, handling escapes */
    char* buf = malloc(strlen(start) + 1);
    if (!buf) return NULL;
    int bi = 0;
    for (const char* p = start; *p && *p != '"'; p++) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': buf[bi++] = '\n'; break;
                case 't': buf[bi++] = '\t'; break;
                case 'r': buf[bi++] = '\r'; break;
                case '\\': buf[bi++] = '\\'; break;
                case '"': buf[bi++] = '"'; break;
                default: buf[bi++] = *p; break;
            }
        } else {
            buf[bi++] = *p;
        }
    }
    buf[bi] = '\0';
    return buf;
}

// LSP Server lifecycle
LSPServer* lsp_server_create() {
    LSPServer* server = (LSPServer*)malloc(sizeof(LSPServer));
    if (!server) return NULL;
    server->input = stdin;
    server->output = stdout;
    /* Opt-in, and to a path the caller names. An editor starts the server in
     * whatever directory it likes, so logging unconditionally dropped
     * `aether-lsp.log` into the user's project on every session. */
    const char* log_path = getenv("AETHER_LSP_LOG");
    server->log_file = (log_path && *log_path) ? fopen(log_path, "w") : NULL;
    server->running = 1;
    server->open_documents = NULL;
    server->document_contents = NULL;
    server->document_count = 0;
    return server;
}

void lsp_server_free(LSPServer* server) {
    if (!server) return;
    
    for (int i = 0; i < server->document_count; i++) {
        free(server->open_documents[i]);
        free(server->document_contents[i]);
    }
    free(server->open_documents);
    free(server->document_contents);
    
    if (server->log_file) {
        fclose(server->log_file);
    }
    
    free(server);
}

void lsp_server_run(LSPServer* server) {
    lsp_log(server, "Aether LSP Server starting...");
    
    while (server->running) {
        JSONRPCMessage* msg = lsp_read_message(server);
        if (!msg) break;
        
        lsp_log(server, "Received: %s (id: %s)", msg->method ? msg->method : "null", msg->id ? msg->id : "null");
        
        if (msg->method) {
            if (strcmp(msg->method, "initialize") == 0) {
                lsp_handle_initialize(server, msg->id);
            } else if (strcmp(msg->method, "textDocument/completion") == 0) {
                // Parse params to extract URI, line, character
                lsp_handle_completion(server, msg->id, "file:///test.ae", 0, 0);
            } else if (strcmp(msg->method, "textDocument/hover") == 0) {
                lsp_handle_hover(server, msg->id, "file:///test.ae", 0, 0);
            } else if (strcmp(msg->method, "textDocument/definition") == 0) {
                lsp_handle_definition(server, msg->id, "file:///test.ae", 0, 0);
            } else if (strcmp(msg->method, "textDocument/didOpen") == 0) {
                char* uri = json_extract_string(msg->params, "uri");
                char* text = json_extract_string(msg->params, "text");
                if (uri && text) {
                    lsp_document_open(server, uri, text);
                    lsp_log(server, "Document opened: %s", uri);
                    lsp_publish_diagnostics(server, uri);
                }
                free(uri);
                free(text);
            } else if (strcmp(msg->method, "textDocument/didChange") == 0) {
                char* uri = json_extract_string(msg->params, "uri");
                char* text = json_extract_string(msg->params, "text");
                if (uri && text) {
                    lsp_document_change(server, uri, text);
                    lsp_log(server, "Document changed: %s", uri);
                    lsp_publish_diagnostics(server, uri);
                }
                free(uri);
                free(text);
            } else if (strcmp(msg->method, "textDocument/didSave") == 0) {
                char* uri = json_extract_string(msg->params, "uri");
                if (uri) {
                    lsp_log(server, "Document saved: %s", uri);
                    lsp_publish_diagnostics(server, uri);
                    free(uri);
                } else {
                    lsp_log(server, "Document saved (unknown URI)");
                }
            } else if (strcmp(msg->method, "initialized") == 0) {
                lsp_log(server, "Client initialized");
            } else if (strcmp(msg->method, "shutdown") == 0) {
                server->running = 0;
                lsp_send_response(server, msg->id, "null");
            } else if (strcmp(msg->method, "exit") == 0) {
                server->running = 0;
            }
        }
        
        lsp_free_message(msg);
    }
    
    lsp_log(server, "Aether LSP Server shutting down...");
}

// Document management
void lsp_document_open(LSPServer* server, const char* uri, const char* text) {
    char** new_docs = (char**)realloc(server->open_documents, (server->document_count + 1) * sizeof(char*));
    if (!new_docs) {
        lsp_log(server, "Error: Failed to allocate document array");
        return;
    }
    server->open_documents = new_docs;

    char** new_contents = (char**)realloc(server->document_contents, (server->document_count + 1) * sizeof(char*));
    if (!new_contents) {
        lsp_log(server, "Error: Failed to allocate contents array");
        return;
    }
    server->document_contents = new_contents;

    char* uri_copy = strdup(uri);
    char* text_copy = strdup(text);
    if (!uri_copy || !text_copy) {
        free(uri_copy);
        free(text_copy);
        lsp_log(server, "Error: Failed to duplicate document strings");
        return;
    }

    server->open_documents[server->document_count] = uri_copy;
    server->document_contents[server->document_count] = text_copy;
    server->document_count++;
}

const char* lsp_document_get(LSPServer* server, const char* uri) {
    for (int i = 0; i < server->document_count; i++) {
        if (strcmp(server->open_documents[i], uri) == 0) {
            return server->document_contents[i];
        }
    }
    return NULL;
}

void lsp_document_change(LSPServer* server, const char* uri, const char* text) {
    for (int i = 0; i < server->document_count; i++) {
        if (strcmp(server->open_documents[i], uri) == 0) {
            free(server->document_contents[i]);
            server->document_contents[i] = strdup(text);
            return;
        }
    }
    lsp_document_open(server, uri, text);
}

void lsp_document_close(LSPServer* server, const char* uri) {
    for (int i = 0; i < server->document_count; i++) {
        if (strcmp(server->open_documents[i], uri) == 0) {
            free(server->open_documents[i]);
            free(server->document_contents[i]);
            for (int j = i; j < server->document_count - 1; j++) {
                server->open_documents[j] = server->open_documents[j + 1];
                server->document_contents[j] = server->document_contents[j + 1];
            }
            server->document_count--;
            return;
        }
    }
}

// LSP features
void lsp_handle_initialize(LSPServer* server, const char* id) {
    const char* capabilities = 
        "{"
        "\"capabilities\":{"
        "\"textDocumentSync\":1,"
        "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"documentSymbolProvider\":true"
        "}"
        "}";
    lsp_send_response(server, id, capabilities);
}

void lsp_handle_completion(LSPServer* server, const char* id, const char* uri, int line, int character) {
    /* Hard-coded completion list — kept in sync by hand for now. The
     * full symbol-aware path (query the typechecker's symbol table at
     * the cursor's scope) is tracked in lsp/README.md as the next
     * upgrade. Until that lands, this table covers:
     *   - language keywords (control flow + storage/declaration)
     *   - core actor and message-passing constructs
     *   - the most-used stdlib surfaces (string, collections, intarr,
     *     bytes, http, fs, json, os, math, log, io, cryptography)
     *
     * LSP CompletionItemKind reference (the integer in `kind`):
     *    3 = Function     12 = Value         14 = Keyword
     *    6 = Variable     15 = Snippet       21 = Constant
     *    7 = Class        22 = Struct        25 = TypeParameter
     */
    const char* completions =
        "{"
        "\"isIncomplete\":false,"
        "\"items\":["

        /* ---- Storage / declaration keywords ---- */
        "{\"label\":\"actor\",\"kind\":14,\"detail\":\"actor definition\",\"documentation\":\"Define a new actor\"},"
        "{\"label\":\"struct\",\"kind\":14,\"detail\":\"struct definition\",\"documentation\":\"Define a struct type\"},"
        "{\"label\":\"message\",\"kind\":14,\"detail\":\"message definition\",\"documentation\":\"Define an actor message type\"},"
        "{\"label\":\"state\",\"kind\":14,\"detail\":\"actor state field\",\"documentation\":\"Declare a state field inside an actor body\"},"
        "{\"label\":\"extern\",\"kind\":14,\"detail\":\"C function declaration\",\"documentation\":\"Declare a C-side function\"},"
        "{\"label\":\"func\",\"kind\":14,\"detail\":\"function definition\",\"documentation\":\"Define a function\"},"
        "{\"label\":\"main\",\"kind\":3,\"detail\":\"main function\",\"documentation\":\"Program entry point\"},"
        "{\"label\":\"let\",\"kind\":14,\"detail\":\"local binding\"},"
        "{\"label\":\"var\",\"kind\":14,\"detail\":\"local binding\"},"
        "{\"label\":\"const\",\"kind\":14,\"detail\":\"compile-time constant\"},"
        "{\"label\":\"import\",\"kind\":14,\"detail\":\"import std.* / module\"},"
        "{\"label\":\"export\",\"kind\":14,\"detail\":\"export declaration\"},"
        "{\"label\":\"as\",\"kind\":14,\"detail\":\"alias / pointer-overlay cast\"},"
        "{\"label\":\"hide\",\"kind\":14,\"detail\":\"hide a name from this scope\"},"
        "{\"label\":\"seal\",\"kind\":14,\"detail\":\"seal scope (with `except`)\"},"
        "{\"label\":\"builder\",\"kind\":14,\"detail\":\"builder function\"},"
        "{\"label\":\"callback\",\"kind\":14,\"detail\":\"hoisted-closure trailing block\"},"

        /* ---- Control flow ---- */
        "{\"label\":\"if\",\"kind\":14,\"detail\":\"if statement\",\"documentation\":\"Conditional statement\"},"
        "{\"label\":\"else\",\"kind\":14,\"detail\":\"else clause\"},"
        "{\"label\":\"for\",\"kind\":14,\"detail\":\"for loop\"},"
        "{\"label\":\"while\",\"kind\":14,\"detail\":\"while loop\"},"
        "{\"label\":\"match\",\"kind\":14,\"detail\":\"match statement\",\"documentation\":\"Pattern-match dispatch — supports literals, struct patterns, list `[a, b]`, cons `[h | t]`\"},"
        "{\"label\":\"switch\",\"kind\":14,\"detail\":\"switch statement\"},"
        "{\"label\":\"case\",\"kind\":14,\"detail\":\"switch arm\"},"
        "{\"label\":\"default\",\"kind\":14,\"detail\":\"switch default\"},"
        "{\"label\":\"return\",\"kind\":14,\"detail\":\"return statement\"},"
        "{\"label\":\"break\",\"kind\":14,\"detail\":\"break loop\"},"
        "{\"label\":\"continue\",\"kind\":14,\"detail\":\"continue loop\"},"
        "{\"label\":\"defer\",\"kind\":14,\"detail\":\"defer statement\",\"documentation\":\"Run an expression at scope exit (LIFO order)\"},"
        "{\"label\":\"panic\",\"kind\":14,\"detail\":\"abort with message\"},"
        "{\"label\":\"try\",\"kind\":14,\"detail\":\"try / catch block\"},"
        "{\"label\":\"catch\",\"kind\":14,\"detail\":\"try / catch arm\"},"
        "{\"label\":\"after\",\"kind\":14,\"detail\":\"receive timeout arm\"},"
        "{\"label\":\"receive\",\"kind\":14,\"detail\":\"receive message\",\"documentation\":\"Receive messages in actor\"},"
        "{\"label\":\"send\",\"kind\":3,\"detail\":\"send message\"},"
        "{\"label\":\"spawn\",\"kind\":3,\"detail\":\"spawn(Actor()) — launch a new actor\"},"
        "{\"label\":\"spawn_actor\",\"kind\":3,\"detail\":\"alternate spawn form\"},"
        "{\"label\":\"self\",\"kind\":21,\"detail\":\"the current actor's reference\"},"

        /* ---- Literals ---- */
        "{\"label\":\"true\",\"kind\":12,\"detail\":\"boolean literal\"},"
        "{\"label\":\"false\",\"kind\":12,\"detail\":\"boolean literal\"},"
        "{\"label\":\"null\",\"kind\":12,\"detail\":\"null pointer / empty value\"},"

        /* ---- Primitive types ---- */
        "{\"label\":\"int\",\"kind\":25,\"detail\":\"primitive — 32-bit signed\"},"
        "{\"label\":\"int64\",\"kind\":25,\"detail\":\"primitive — 64-bit signed\"},"
        "{\"label\":\"long\",\"kind\":25,\"detail\":\"primitive — 64-bit signed (alias)\"},"
        "{\"label\":\"float\",\"kind\":25,\"detail\":\"primitive — IEEE-754 float\"},"
        "{\"label\":\"double\",\"kind\":25,\"detail\":\"primitive — IEEE-754 double\"},"
        "{\"label\":\"bool\",\"kind\":25,\"detail\":\"primitive — boolean\"},"
        "{\"label\":\"string\",\"kind\":25,\"detail\":\"primitive — refcounted string / const char*\"},"
        "{\"label\":\"void\",\"kind\":25,\"detail\":\"primitive — no value\"},"
        "{\"label\":\"ptr\",\"kind\":25,\"detail\":\"primitive — opaque void*\"},"
        "{\"label\":\"actor_ref\",\"kind\":25,\"detail\":\"primitive — handle to a spawned actor\"},"
        "{\"label\":\"StringSeq\",\"kind\":22,\"detail\":\"struct — cons-cell of strings (use as *StringSeq)\"},"

        /* ---- Builtins ---- */
        "{\"label\":\"print\",\"kind\":3,\"detail\":\"print(value)\",\"documentation\":\"Print to stdout (no newline)\"},"
        "{\"label\":\"println\",\"kind\":3,\"detail\":\"println(value)\",\"documentation\":\"Print with newline\"},"
        "{\"label\":\"sleep\",\"kind\":3,\"detail\":\"sleep(ms)\"},"
        "{\"label\":\"clock_ns\",\"kind\":3,\"detail\":\"clock_ns() -> long — monotonic ns\"},"
        "{\"label\":\"wait_for_idle\",\"kind\":3,\"detail\":\"wait_for_idle() — drain actor scheduler\"},"
        "{\"label\":\"exit\",\"kind\":3,\"detail\":\"exit(code: int)\"},"
        "{\"label\":\"len\",\"kind\":3,\"detail\":\"len(array)\"},"

        /* ---- std.string (full surface) ---- */
        "{\"label\":\"string.length\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string.equals\",\"kind\":3,\"detail\":\"-> int (1 if equal)\"},"
        "{\"label\":\"string.compare\",\"kind\":3,\"detail\":\"-> int (negative/zero/positive)\"},"
        "{\"label\":\"string.concat\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.concat_wrapped\",\"kind\":3,\"detail\":\"-> ptr (length-aware concat)\"},"
        "{\"label\":\"string.starts_with\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string.ends_with\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string.contains\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string.index_of\",\"kind\":3,\"detail\":\"-> int (-1 if not found)\"},"
        "{\"label\":\"string.index_of_from\",\"kind\":3,\"detail\":\"-> int — search starting at offset\"},"
        "{\"label\":\"string.substring\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.to_upper\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.to_lower\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.trim\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.from_int\",\"kind\":3,\"detail\":\"-> ptr (refcounted string)\"},"
        "{\"label\":\"string.from_long\",\"kind\":3,\"detail\":\"-> ptr (refcounted string)\"},"
        "{\"label\":\"string.from_float\",\"kind\":3,\"detail\":\"-> ptr (refcounted string)\"},"
        "{\"label\":\"string.from_char\",\"kind\":3,\"detail\":\"-> ptr — 1-byte string from a code\"},"
        "{\"label\":\"string.copy\",\"kind\":3,\"detail\":\"-> string — independently-owned snapshot\"},"
        "{\"label\":\"string.format\",\"kind\":3,\"detail\":\"format(fmt, args) -> string\"},"
        "{\"label\":\"string.split\",\"kind\":3,\"detail\":\"-> ptr (AetherStringArray*)\"},"
        "{\"label\":\"string.split_to_seq\",\"kind\":3,\"detail\":\"-> *StringSeq — cons-cell sibling of split\"},"
        "{\"label\":\"string.array_size\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string.array_get\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.array_free\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"string.seq_empty\",\"kind\":3,\"detail\":\"-> *StringSeq (NULL)\"},"
        "{\"label\":\"string.seq_cons\",\"kind\":3,\"detail\":\"(head, tail) -> *StringSeq\"},"
        "{\"label\":\"string.seq_head\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"string.seq_tail\",\"kind\":3,\"detail\":\"-> *StringSeq\"},"
        "{\"label\":\"string.seq_is_empty\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string.seq_length\",\"kind\":3,\"detail\":\"-> int (O(1) cached)\"},"
        "{\"label\":\"string.seq_retain\",\"kind\":3,\"detail\":\"-> *StringSeq (refcount++)\"},"
        "{\"label\":\"string.seq_free\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"string.seq_from_array\",\"kind\":3,\"detail\":\"(arr: ptr, count: int) -> *StringSeq\"},"
        "{\"label\":\"string.seq_to_array\",\"kind\":3,\"detail\":\"-> ptr (AetherStringArray*)\"},"
        "{\"label\":\"string.seq_reverse\",\"kind\":3,\"detail\":\"-> *StringSeq (fresh independent spine)\"},"
        "{\"label\":\"string.seq_concat\",\"kind\":3,\"detail\":\"(a, b) -> *StringSeq (a copied, b shared)\"},"
        "{\"label\":\"string.seq_take\",\"kind\":3,\"detail\":\"(s, n) -> *StringSeq (first n elements, fresh spine)\"},"
        "{\"label\":\"string.seq_drop\",\"kind\":3,\"detail\":\"(s, n) -> *StringSeq (n-th tail, retained)\"},"
        "{\"label\":\"string.to_int\",\"kind\":3,\"detail\":\"-> (int, error)\"},"
        "{\"label\":\"string.to_long\",\"kind\":3,\"detail\":\"-> (long, error)\"},"
        "{\"label\":\"string.to_float\",\"kind\":3,\"detail\":\"-> (float, error)\"},"
        "{\"label\":\"string.to_double\",\"kind\":3,\"detail\":\"-> (double, error)\"},"

        /* ---- std.collections (list / map / string_list) ---- */
        "{\"label\":\"list_new\",\"kind\":3,\"detail\":\"-> ptr (ArrayList)\"},"
        "{\"label\":\"list_add\",\"kind\":3,\"detail\":\"(list, item) -> string err\"},"
        "{\"label\":\"list_get\",\"kind\":3,\"detail\":\"(list, i) -> (item, err)\"},"
        "{\"label\":\"list_size\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"list_remove\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"list_clear\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"list_free\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"map_new\",\"kind\":3,\"detail\":\"-> ptr (HashMap)\"},"
        "{\"label\":\"map_put\",\"kind\":3,\"detail\":\"(map, key, value) -> string err\"},"
        "{\"label\":\"map_get\",\"kind\":3,\"detail\":\"(map, key) -> (value, err)\"},"
        "{\"label\":\"map_has\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"map_remove\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"map_size\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"map_keys\",\"kind\":3,\"detail\":\"-> (ptr, err)\"},"
        "{\"label\":\"map_free\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"string_list_new\",\"kind\":3,\"detail\":\"-> ptr — refcount-aware list of strings (#274)\"},"
        "{\"label\":\"string_list_add\",\"kind\":3,\"detail\":\"(list, s) -> int\"},"
        "{\"label\":\"string_list_get\",\"kind\":3,\"detail\":\"(list, i) -> string\"},"
        "{\"label\":\"string_list_set\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"string_list_size\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"string_list_remove\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"string_list_clear\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"string_list_free\",\"kind\":3,\"detail\":\"\"},"

        /* ---- std.intarr ---- */
        "{\"label\":\"intarr.new\",\"kind\":3,\"detail\":\"(size) -> (arr, err)\"},"
        "{\"label\":\"intarr.new_filled\",\"kind\":3,\"detail\":\"(size, init) -> (arr, err)\"},"
        "{\"label\":\"intarr.get\",\"kind\":3,\"detail\":\"(arr, i) -> (value, err)\"},"
        "{\"label\":\"intarr.set\",\"kind\":3,\"detail\":\"(arr, i, value) -> err\"},"
        "{\"label\":\"intarr.fill\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"intarr.free\",\"kind\":3,\"detail\":\"\"},"

        /* ---- std.bytes (#288) ---- */
        "{\"label\":\"bytes.new\",\"kind\":3,\"detail\":\"(size) -> ptr\"},"
        "{\"label\":\"bytes.set\",\"kind\":3,\"detail\":\"(b, i, byte: int)\"},"
        "{\"label\":\"bytes.length\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"bytes.copy_from_string\",\"kind\":3,\"detail\":\"(b, offset, src) -> int\"},"
        "{\"label\":\"bytes.copy_within\",\"kind\":3,\"detail\":\"(b, dst, src, len)\"},"
        "{\"label\":\"bytes.finish\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"bytes.free\",\"kind\":3,\"detail\":\"\"},"

        /* ---- std.fs ---- */
        "{\"label\":\"fs.read\",\"kind\":3,\"detail\":\"-> (string, err)\"},"
        "{\"label\":\"fs.read_binary\",\"kind\":3,\"detail\":\"-> (ptr, err)\"},"
        "{\"label\":\"fs.write\",\"kind\":3,\"detail\":\"-> err\"},"
        "{\"label\":\"fs.write_atomic\",\"kind\":3,\"detail\":\"-> err\"},"
        "{\"label\":\"fs.exists\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"fs.mkdir_p\",\"kind\":3,\"detail\":\"-> err\"},"
        "{\"label\":\"fs.unlink\",\"kind\":3,\"detail\":\"-> err\"},"

        /* ---- std.json ---- */
        "{\"label\":\"json.parse\",\"kind\":3,\"detail\":\"-> (value, err)\"},"
        "{\"label\":\"json.stringify\",\"kind\":3,\"detail\":\"-> string\"},"

        /* ---- std.net.http ---- */
        "{\"label\":\"http.get\",\"kind\":3,\"detail\":\"-> (body, err)\"},"
        "{\"label\":\"http.get_raw\",\"kind\":3,\"detail\":\"-> ptr (HttpResponse*)\"},"
        "{\"label\":\"http.server_create\",\"kind\":3,\"detail\":\"-> ptr\"},"
        "{\"label\":\"http.server_get\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"http.server_start\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"http.server_stop\",\"kind\":3,\"detail\":\"\"},"

        /* ---- std.os ---- */
        "{\"label\":\"os.getenv\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"os.setenv\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"os.exec\",\"kind\":3,\"detail\":\"-> err\"},"
        "{\"label\":\"os.system\",\"kind\":3,\"detail\":\"-> int\"},"
        "{\"label\":\"os.getpid\",\"kind\":3,\"detail\":\"-> int\"},"

        /* ---- std.cryptography ---- */
        "{\"label\":\"cryptography.sha256\",\"kind\":3,\"detail\":\"-> string (hex)\"},"
        "{\"label\":\"cryptography.base64_encode\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"cryptography.base64_encode_padded\",\"kind\":3,\"detail\":\"-> string\"},"
        "{\"label\":\"cryptography.base64_decode\",\"kind\":3,\"detail\":\"-> (data, len, err)\"},"

        /* ---- std.math ---- */
        "{\"label\":\"math.sqrt\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"math.pow\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"math.abs\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"math.min\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"math.max\",\"kind\":3,\"detail\":\"\"},"

        /* ---- std.log ---- */
        "{\"label\":\"log.write\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"log.info\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"log.warn\",\"kind\":3,\"detail\":\"\"},"
        "{\"label\":\"log.error\",\"kind\":3,\"detail\":\"\"}"

        "]"
        "}";
    lsp_send_response(server, id, completions);
}

void lsp_handle_hover(LSPServer* server, const char* id, const char* uri, int line, int character) {
    // Return hover information
    const char* hover =
        "{"
        "\"contents\":{"
        "\"kind\":\"markdown\","
        "\"value\":\"**Aether Actor**\\n\\nLightweight concurrent actor\""
        "}"
        "}";
    lsp_send_response(server, id, hover);
}

void lsp_handle_definition(LSPServer* server, const char* id, const char* uri, int line, int character) {
    // Return definition location
    lsp_send_response(server, id, "null");
}

void lsp_handle_document_symbol(LSPServer* server, const char* id, const char* uri) {
    // Return document symbols (functions, actors, etc.)
    lsp_send_response(server, id, "[]");
}

void lsp_publish_diagnostics(LSPServer* server, const char* uri) {
    const char* source = lsp_document_get(server, uri);
    if (!source) {
        lsp_log(server, "No document content for URI: %s", uri);
        return;
    }

    char diagnostics[16384];
    char diag_items[15000];
    int diag_offset = 0;
    int diag_count = 0;

    /* Phase 1: Lex the source and collect TOKEN_ERROR tokens */
    lexer_init(source);
    Token* tok;
    while ((tok = next_token()) != NULL) {
        if (tok->type == TOKEN_ERROR) {
            int line = tok->line > 0 ? tok->line - 1 : 0;
            int col = tok->column > 0 ? tok->column - 1 : 0;
            if (diag_offset > 0) {
                diag_items[diag_offset++] = ',';
            }
            int n = snprintf(diag_items + diag_offset, sizeof(diag_items) - diag_offset,
                "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                "\"end\":{\"line\":%d,\"character\":%d}},"
                "\"severity\":1,\"source\":\"aether\","
                "\"message\":\"Unexpected token: %s\"}",
                line, col, line, col + 1,
                tok->value ? tok->value : "?");
            if (n > 0 && diag_offset + n < (int)sizeof(diag_items)) {
                diag_offset += n;
                diag_count++;
            }
        }
        int is_eof = (tok->type == TOKEN_EOF);
        free_token(tok);
        if (is_eof) break;
    }

    /* Phase 2: If no lex errors, try parsing to catch syntax errors */
    if (diag_count == 0) {
        lexer_init(source);
        Token* tokens[4096];
        int token_count = 0;
        while (token_count < 4095) {
            Token* t = next_token();
            tokens[token_count++] = t;
            if (t->type == TOKEN_EOF || t->type == TOKEN_ERROR) break;
        }

        /* Capture parser errors via fd-level redirection. Do NOT assign to
         * `stderr`: it is not an assignable lvalue on musl or mingw (glibc
         * and macOS merely tolerate it). dup2 onto stderr's fd is the
         * portable capture; the block stays gated off on Windows like the
         * fmemopen version it replaces.
         */
        char parse_errors[4096] = {0};
#if AETHER_HAS_FMEMOPEN
        int saved_err_fd = -1;
        FILE* err_capture = tmpfile();
        if (err_capture) {
            fflush(stderr);
            saved_err_fd = dup(fileno(stderr));
            if (saved_err_fd >= 0) {
                dup2(fileno(err_capture), fileno(stderr));
            } else {
                fclose(err_capture);
                err_capture = NULL;
            }
        }
#endif

        Parser* parser = create_parser(tokens, token_count);
        ASTNode* ast = parse_program(parser);

#if AETHER_HAS_FMEMOPEN
        if (err_capture) {
            fflush(stderr);
            dup2(saved_err_fd, fileno(stderr));
            close(saved_err_fd);
            rewind(err_capture);
            size_t err_got = fread(parse_errors, 1, sizeof(parse_errors) - 1, err_capture);
            parse_errors[err_got] = '\0';
            fclose(err_capture);
        }
#endif

        /* Extract line/column from captured error messages if parse failed */
        if (parse_errors[0] != '\0') {
            char* line_ptr = parse_errors;
            while (line_ptr && *line_ptr && diag_count < 20) {
                char* newline = strchr(line_ptr, '\n');
                if (newline) *newline = '\0';

                if (strlen(line_ptr) > 2) {
                    if (diag_offset > 0) {
                        diag_items[diag_offset++] = ',';
                    }
                    /* Escape quotes in the error message */
                    char safe_msg[512];
                    int si = 0;
                    for (const char* p = line_ptr; *p && si < 500; p++) {
                        if (*p == '"' || *p == '\\') safe_msg[si++] = '\\';
                        safe_msg[si++] = *p;
                    }
                    safe_msg[si] = '\0';

                    int n = snprintf(diag_items + diag_offset, sizeof(diag_items) - diag_offset,
                        "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                        "\"end\":{\"line\":0,\"character\":1}},"
                        "\"severity\":1,\"source\":\"aether\","
                        "\"message\":\"%s\"}", safe_msg);
                    if (n > 0 && diag_offset + n < (int)sizeof(diag_items)) {
                        diag_offset += n;
                        diag_count++;
                    }
                }

                if (newline) {
                    *newline = '\n';
                    line_ptr = newline + 1;
                } else {
                    break;
                }
            }
        }

        if (ast) free_ast_node(ast);
        free_parser(parser);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
    }

    diag_items[diag_offset] = '\0';

    lsp_log(server, "Publishing %d diagnostics for %s", diag_count, uri);

    int written = snprintf(diagnostics, sizeof(diagnostics),
                          "{\"uri\":\"%s\",\"diagnostics\":[%s]}", uri, diag_items);

    if (written < 0 || (size_t)written >= sizeof(diagnostics)) {
        lsp_log(server, "Warning: diagnostics buffer overflow, sending empty");
        snprintf(diagnostics, sizeof(diagnostics),
                "{\"uri\":\"%s\",\"diagnostics\":[]}", uri);
    }

    lsp_send_notification(server, "textDocument/publishDiagnostics", diagnostics);
}

// JSON-RPC (simplified implementation)
JSONRPCMessage* lsp_read_message(LSPServer* server) {
    char header[1024];
    int content_length = 0;
    
    // Read headers
    while (fgets(header, sizeof(header), server->input)) {
        if (strstr(header, "Content-Length:")) {
            sscanf(header, "Content-Length: %d", &content_length);
        }
        if (strcmp(header, "\r\n") == 0 || strcmp(header, "\n") == 0) {
            break;
        }
    }
    
    if (content_length == 0) return NULL;

    // Read content
    char* content = (char*)malloc(content_length + 1);
    if (!content) {
        lsp_log(server, "Error: Failed to allocate content buffer");
        return NULL;
    }
    size_t bytes_read = fread(content, 1, content_length, server->input);
    if (bytes_read != (size_t)content_length) {
        lsp_log(server, "Warning: Read fewer bytes than expected");
    }
    content[bytes_read] = '\0';

    // Parse JSON (simplified - would use a proper JSON parser in production)
    JSONRPCMessage* msg = (JSONRPCMessage*)malloc(sizeof(JSONRPCMessage));
    if (!msg) {
        lsp_log(server, "Error: Failed to allocate message struct");
        free(content);
        return NULL;
    }
    msg->method = NULL;
    msg->id = NULL;
    msg->params = NULL;
    
    // Extract method
    char* method_start = strstr(content, "\"method\":");
    if (method_start) {
        method_start = strchr(method_start, '"');
        method_start = strchr(method_start + 1, '"') + 1;
        char* method_end = strchr(method_start, '"');
        if (method_end) {
            msg->method = strndup(method_start, method_end - method_start);
        }
    }

    // Extract id (can be number or string)
    char* id_start = strstr(content, "\"id\":");
    if (id_start) {
        id_start += 5;
        while (*id_start == ' ') id_start++;
        if (*id_start == '"') {
            char* id_end = strchr(id_start + 1, '"');
            if (id_end) msg->id = strndup(id_start, id_end - id_start + 1);
        } else {
            char* id_end = id_start;
            while (*id_end >= '0' && *id_end <= '9') id_end++;
            if (id_end > id_start) msg->id = strndup(id_start, id_end - id_start);
        }
    }

    // Store full content as params for handlers to extract fields
    msg->params = content;
    return msg;
}

void lsp_free_message(JSONRPCMessage* msg) {
    if (!msg) return;
    free(msg->method);
    free(msg->id);
    free(msg->params);
    free(msg);
}

void lsp_send_response(LSPServer* server, const char* id, const char* result) {
    const char* id_str = id ? id : "null";
    // Compute required size, then allocate exactly that much
    int needed = snprintf(NULL, 0,
                          "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
                          id_str, result);
    if (needed < 0) return;
    char* response = (char*)malloc((size_t)needed + 1);
    if (!response) return;
    snprintf(response, (size_t)needed + 1,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
             id_str, result);
    fprintf(server->output, "Content-Length: %zu\r\n\r\n%s", (size_t)needed, response);
    fflush(server->output);
    free(response);
}

void lsp_send_notification(LSPServer* server, const char* method, const char* params) {
    // Compute required size, then allocate exactly that much
    int needed = snprintf(NULL, 0,
                          "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
                          method, params);
    if (needed < 0) return;
    char* notification = (char*)malloc((size_t)needed + 1);
    if (!notification) return;
    snprintf(notification, (size_t)needed + 1,
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
             method, params);
    fprintf(server->output, "Content-Length: %zu\r\n\r\n%s", (size_t)needed, notification);
    fflush(server->output);
    free(notification);
}

void lsp_log(LSPServer* server, const char* format, ...) {
    if (!server || !server->log_file) return;
    
    va_list args;
    va_start(args, format);
    vfprintf(server->log_file, format, args);
    fprintf(server->log_file, "\n");
    fflush(server->log_file);
    va_end(args);
}

