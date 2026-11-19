/* Coverage-guided fuzz target for the WebSocket frame reader in
 * std/net/aether_http_server.c (http_ws_recv and what it sends back: pongs,
 * the echoed close frame, the close a protocol error fails the connection
 * with).
 *
 * The reader takes its bytes from the connection, first what the request
 * loop already buffered past the upgrade's head (conn->buf), then the
 * transport. The harness includes aether_http_server.c to build those two
 * structures, and stands in for the transport through the pure-TLS hooks
 * (aether_callback_register), so no socket is involved and the bytes come in
 * chunks of a size the input picks.
 *
 * Input: byte 0 picks the handle's role (bit 0: a client handle, which sends
 * masked frames and must receive unmasked ones; a server handle the
 * reverse) and its message cap (bits 1-3: the default, or 1 to 4096 bytes);
 * byte 1 how many bytes are buffered ahead (a share of the stream); byte 2
 * the transport's chunk size (1 to 64). The rest is the stream from the
 * peer.
 *
 * Each check is a property, and a failure abort()s with what disagreed:
 *
 *   W1  What each http_ws_recv returns is what a reading of RFC 6455 5
 *       written here gives for the same stream: the type and the bytes of
 *       each message, unmasked and reassembled from its fragments, and -1
 *       once the stream ends, the peer closes or a frame breaks a rule: a
 *       frame masked the wrong way for the role, a reserved opcode or a
 *       reserved bit set (5.2), a control frame that is fragmented or
 *       longer than 125 bytes (5.5), a continuation with no message in
 *       progress or a data frame inside one (5.4), a message past the cap,
 *       a close frame of one byte, or with a code no close frame may
 *       carry (7.4: below 1000, 1004 to 1006, 1015 to 2999, 5000 and up),
 *       a text message or a close reason that is not UTF-8 (8.1: no
 *       overlong form, surrogate or code point past U+10FFFF, checked
 *       across fragments, failing at the frame where the bytes can no
 *       longer be completed to UTF-8). A text message is NUL-terminated.
 *   W2  The frames the reader sends are exactly the ones the reading gives,
 *       masked as the role requires: a pong with each ping's payload, the
 *       peer's close frame echoed, and a close with 1002, 1007 or 1009 when
 *       it fails the connection.
 *   W3  The reassembly buffer grows with what arrived, never with what a
 *       frame declared: it never holds more than the cap and one byte, nor
 *       much more than the bytes received.
 *   W4  A closed handle stays closed: http_ws_recv answers -1 and sends
 *       nothing.
 *   W5  Nothing leaks (LeakSanitizer). */
#include "../../../std/net/aether_http_server.c"

#include <stdint.h>

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_websocket: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

/* ---- the transport ---------------------------------------------------- */

typedef struct {
    const uint8_t* in;
    size_t in_len, in_pos, chunk;
    uint8_t* out;
    size_t out_len, out_cap;
} Wire;

static int wire_recv(void* c, void* buf, int len) {
    Wire* w = (Wire*)c;
    size_t left = w->in_len - w->in_pos;
    if (left == 0 || len <= 0) return 0;
    size_t n = (size_t)len < w->chunk ? (size_t)len : w->chunk;
    if (n > left) n = left;
    memcpy(buf, w->in + w->in_pos, n);
    w->in_pos += n;
    return (int)n;
}

static int wire_send(void* c, const void* buf, int len) {
    Wire* w = (Wire*)c;
    if (len < 0) return -1;
    if (w->out_len + (size_t)len > w->out_cap) {
        size_t cap = w->out_cap ? w->out_cap : 256;
        while (cap < w->out_len + (size_t)len) cap *= 2;
        w->out = (uint8_t*)realloc(w->out, cap);
        CHECK(w->out != NULL, "out of memory");
        w->out_cap = cap;
    }
    memcpy(w->out + w->out_len, buf, (size_t)len);
    w->out_len += (size_t)len;
    return len;
}

/* ---- the reading of RFC 6455 ------------------------------------------ */

typedef struct {
    int kind;            /* 1 text, 2 binary, -1 closed */
    uint8_t* data;
    size_t len;
} Result;

typedef struct {
    int opcode;          /* PONG, CLOSE */
    uint8_t* payload;
    size_t len;
    int code_only;       /* a close the reader failed with: its code, any reason */
} Sent;

typedef struct {
    Result* res;
    int nres;
    Sent* sent;
    int nsent;
} Model;

static void model_send(Model* m, int opcode, const uint8_t* p, size_t n) {
    m->sent = (Sent*)realloc(m->sent, (size_t)(m->nsent + 1) * sizeof(Sent));
    CHECK(m->sent != NULL, "out of memory");
    Sent* s = &m->sent[m->nsent++];
    s->opcode = opcode;
    s->code_only = 0;
    s->len = n;
    s->payload = (uint8_t*)malloc(n ? n : 1);
    CHECK(s->payload != NULL, "out of memory");
    if (n) memcpy(s->payload, p, n);
}

static void model_fail(Model* m, int code) {
    uint8_t p[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    model_send(m, WS_OP_CLOSE, p, 2);
    m->sent[m->nsent - 1].code_only = 1;
}

static void model_result(Model* m, int kind, const uint8_t* d, size_t n) {
    m->res = (Result*)realloc(m->res, (size_t)(m->nres + 1) * sizeof(Result));
    CHECK(m->res != NULL, "out of memory");
    Result* r = &m->res[m->nres++];
    r->kind = kind;
    r->len = n;
    r->data = (uint8_t*)malloc(n ? n : 1);
    CHECK(r->data != NULL, "out of memory");
    if (n) memcpy(r->data, d, n);
}

/* UTF-8 as RFC 3629 has it, decoded rather than matched against byte
 * ranges, so the reading here does not share the reader's method: a lead
 * byte gives a length and the bits it carries, each continuation byte six
 * more, and the code points the sequence can still name must reach a valid
 * one: no less than the least its length may encode (an overlong form
 * cannot), not a surrogate, not past U+10FFFF. -1 when `b` cannot be
 * completed to UTF-8, 1 when it ends inside a code point that can still be
 * completed, 0 when it is whole code points. */
static int utf8_prefix(const uint8_t* b, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = b[i];
        int len;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return -1;
        int have = 1;
        while (have < len && i + (size_t)have < n) {
            if ((b[i + (size_t)have] & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (b[i + (size_t)have] & 0x3F);
            have++;
        }
        int missing = len - have;
        uint32_t lo = cp << (6 * missing);
        uint32_t hi = lo | ((1u << (6 * missing)) - 1);
        uint32_t least = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (lo < least) lo = least;
        if (hi > 0x10FFFF) hi = 0x10FFFF;
        if (lo > hi) return -1;
        /* What is left must not lie wholly among the surrogates. */
        if (lo >= 0xD800 && hi <= 0xDFFF) return -1;
        if (missing > 0) return 1;   /* only the last sequence can be unfinished */
        i += (size_t)len;
    }
    return 0;
}

/* The close codes RFC 6455 7.4 lets a close frame carry: the ones 7.4.1
 * defines that may be sent (1000 to 1003, 1007 to 1011), the ones IANA
 * registered since (1012 to 1014), and 3000 to 4999 for libraries and
 * applications. */
static int close_code_ok(int c) {
    if (c >= 3000 && c <= 4999) return 1;
    switch (c) {
    case 1000: case 1001: case 1002: case 1003: case 1007: case 1008: case 1009:
    case 1010: case 1011: case 1012: case 1013: case 1014: return 1;
    default: return 0;
    }
}

/* The whole stream, read as the reader must read it: each message it
 * returns, then -1, and every frame it sends. */
static void model_stream(const uint8_t* s, size_t n, int mask_tx, size_t max, Model* m) {
    size_t p = 0;
    uint8_t* msg = NULL;
    size_t msg_len = 0;
    for (;;) {
        /* one call of http_ws_recv: a message begins and ends within it */
        msg_len = 0;
        int in_message = 0, msg_opcode = 0, done = 0;
        while (!done) {
            if (n - p < 2) goto closed;
            int fin = s[p] >> 7, rsv = s[p] & 0x70, opcode = s[p] & 15, masked = s[p + 1] >> 7;
            uint64_t pl = s[p + 1] & 127;
            p += 2;
            if (pl == 126) {
                if (n - p < 2) goto closed;
                pl = ((uint64_t)s[p] << 8) | s[p + 1];
                p += 2;
            } else if (pl == 127) {
                if (n - p < 8) goto closed;
                pl = 0;
                for (int i = 0; i < 8; i++) pl = (pl << 8) | s[p + i];
                p += 8;
            }
            uint8_t key[4] = {0, 0, 0, 0};
            if (masked) {
                if (n - p < 4) goto closed;
                memcpy(key, s + p, 4);
                p += 4;
            }
            if (masked == mask_tx) { model_fail(m, 1002); goto closed; }
            if ((opcode >= 3 && opcode <= 7) || opcode >= 11) { model_fail(m, 1002); goto closed; }
            if (rsv) { model_fail(m, 1002); goto closed; }
            if (opcode >= 8) {
                if (!fin || pl > 125) { model_fail(m, 1002); goto closed; }
                if (n - p < pl) goto closed;
                uint8_t ctl[125];
                for (uint64_t i = 0; i < pl; i++) ctl[i] = s[p + i] ^ key[i & 3];
                p += (size_t)pl;
                if (opcode == WS_OP_PING) { model_send(m, WS_OP_PONG, ctl, (size_t)pl); continue; }
                if (opcode == WS_OP_PONG) continue;
                if (pl == 1) { model_fail(m, 1002); goto closed; }
                if (pl >= 2 && !close_code_ok((ctl[0] << 8) | ctl[1])) { model_fail(m, 1002); goto closed; }
                if (pl > 2 && utf8_prefix(ctl + 2, (size_t)pl - 2) != 0) { model_fail(m, 1007); goto closed; }
                model_send(m, WS_OP_CLOSE, ctl, (size_t)pl);
                goto closed;
            }
            if (opcode == WS_OP_CONT && !in_message) { model_fail(m, 1002); goto closed; }
            if (opcode != WS_OP_CONT && in_message) { model_fail(m, 1002); goto closed; }
            if (opcode != WS_OP_CONT) {
                msg_opcode = opcode;
                in_message = 1;
            }
            if (msg_len > max || pl > (uint64_t)(max - msg_len)) { model_fail(m, 1009); goto closed; }
            if (n - p < pl) goto closed;
            msg = (uint8_t*)realloc(msg, msg_len + (size_t)pl + 1);
            CHECK(msg != NULL, "out of memory");
            for (uint64_t i = 0; i < pl; i++) msg[msg_len + i] = s[p + i] ^ key[i & 3];
            msg_len += (size_t)pl;
            p += (size_t)pl;
            if (msg_opcode == WS_OP_TEXT) {
                int u = utf8_prefix(msg, msg_len);
                if (u < 0 || (fin && u != 0)) { model_fail(m, 1007); goto closed; }
            }
            if (fin) done = 1;
        }
        model_result(m, msg_opcode == WS_OP_TEXT ? 1 : 2, msg, msg_len);
    }
closed:
    model_result(m, -1, NULL, 0);
    free(msg);
}

static void model_free(Model* m) {
    for (int i = 0; i < m->nres; i++) free(m->res[i].data);
    for (int i = 0; i < m->nsent; i++) free(m->sent[i].payload);
    free(m->res);
    free(m->sent);
}

/* ---- the frames the reader sent --------------------------------------- */

static void check_sent(const Wire* w, const Model* m, int mask_tx) {
    size_t p = 0;
    int k = 0;
    while (p < w->out_len) {
        CHECK(w->out_len - p >= 2, "a frame cut short in what was sent");
        int fin = w->out[p] >> 7, opcode = w->out[p] & 15, masked = w->out[p + 1] >> 7;
        size_t pl = w->out[p + 1] & 127;
        p += 2;
        CHECK(fin == 1, "a sent frame without FIN");
        CHECK(masked == mask_tx, "a sent frame masked %d by a handle that masks %d", masked, mask_tx);
        CHECK(pl <= 125, "a sent control frame of %zu bytes", pl);
        uint8_t key[4] = {0, 0, 0, 0};
        if (masked) {
            CHECK(w->out_len - p >= 4, "a sent frame's mask cut short");
            memcpy(key, w->out + p, 4);
            p += 4;
        }
        CHECK(w->out_len - p >= pl, "a sent frame's payload cut short");
        CHECK(k < m->nsent, "the reader sent frame %d (opcode %d), the reading sends %d", k + 1, opcode, m->nsent);
        const Sent* want = &m->sent[k];
        CHECK(opcode == want->opcode, "sent frame %d is opcode %d, the reading's is %d", k, opcode, want->opcode);
        if (want->code_only) {
            CHECK(pl >= 2, "the close frame %d carries no code", k);
            for (size_t i = 2; i < pl; i++)
                CHECK((uint8_t)(w->out[p + i] ^ key[i & 3]) >= 0x20, "the close frame %d's reason holds a control byte", k);
        } else {
            CHECK(pl == want->len, "sent frame %d holds %zu bytes, the reading's %zu", k, pl, want->len);
        }
        for (size_t i = 0; i < (want->code_only ? 2 : pl); i++)
            CHECK((uint8_t)(w->out[p + i] ^ key[i & 3]) == want->payload[i], "sent frame %d differs at byte %zu", k, i);
        p += pl;
        k++;
    }
    CHECK(k == m->nsent, "the reader sent %d frames, the reading %d", k, m->nsent);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3) return 0;
    static int registered = 0;
    if (!registered) {
        aether_callback_register("aether_pure_tls_server_recv", (void*)wire_recv);
        aether_callback_register("aether_pure_tls_server_send", (void*)wire_send);
        registered = 1;
    }
    int mask_tx = data[0] & 1;
    static const size_t caps[8] = { 0, 1, 7, 64, 125, 1000, 4096, 126 };
    size_t cap_sel = caps[(data[0] >> 1) & 7];
    size_t max = cap_sel ? cap_sel : HTTP_WS_DEFAULT_MAX_MESSAGE;
    const uint8_t* stream = data + 3;
    size_t n = size - 3;
    size_t ahead = n * data[1] / 255;

    Model m;
    memset(&m, 0, sizeof m);
    model_stream(stream, n, mask_tx, max, &m);

    Wire w;
    memset(&w, 0, sizeof w);
    w.in = stream + ahead;
    w.in_len = n - ahead;
    w.chunk = 1 + (data[2] & 63);

    HttpConn* conn = (HttpConn*)calloc(1, sizeof(HttpConn));
    CHECK(conn != NULL, "out of memory");
    conn->fd = -1;
    conn->pure_tls = &w;
    conn->buf = (char*)malloc(ahead ? ahead : 1);
    CHECK(conn->buf != NULL, "out of memory");
    if (ahead) memcpy(conn->buf, stream, ahead);
    conn->buf_cap = (int)(ahead ? ahead : 1);
    conn->write_pos = (int)ahead;

    HttpWsConn* ws = (HttpWsConn*)calloc(1, sizeof(HttpWsConn));
    CHECK(ws != NULL, "out of memory");
    ws->conn = conn;
    ws->mask_tx = mask_tx;
    if (cap_sel) CHECK(*http_ws_set_max_message_raw(ws, (int64_t)cap_sel) == '\0', "a cap of %zu was refused", cap_sel);

    for (int i = 0; i < m.nres; i++) {
        const Result* want = &m.res[i];
        int got = http_ws_recv(ws);
        CHECK(got == want->kind, "receive %d returned %d, the reading gives %d", i, got, want->kind);
        /* W3 */
        CHECK(ws->msg_cap <= max + 1 || ws->msg_cap <= 4096, "a buffer of %zu bytes under a cap of %zu", ws->msg_cap, max);
        CHECK(ws->msg_cap <= 2 * (n + 65536) + 4096, "a buffer of %zu bytes for %zu bytes received", ws->msg_cap, n);
        if (got < 0) break;
        CHECK((size_t)http_ws_message_length(ws) == want->len, "message %d is %d bytes, the reading's %zu", i,
              http_ws_message_length(ws), want->len);
        CHECK(want->len == 0 || memcmp(http_ws_message_data(ws), want->data, want->len) == 0,
              "message %d differs from the reading's", i);
        if (got == 1) CHECK(http_ws_message_data(ws)[want->len] == '\0', "text message %d is not NUL-terminated", i);
    }
    /* W4 */
    size_t sent_before = w.out_len;
    CHECK(http_ws_recv(ws) == -1, "a closed handle received again");
    CHECK(w.out_len == sent_before, "a closed handle sent again");
    /* W2 */
    check_sent(&w, &m, mask_tx);

    aether_caps_free(ws->msg_buf, ws->msg_cap);
    free(ws);
    free(conn->buf);
    free(conn);
    free(w.out);
    model_free(&m);
    return 0;
}
