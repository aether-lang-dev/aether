/* Copyright (c) 2026 Aether Developers. */
/* BytesCursor — a forward read-position over a borrowed AetherBytes.
 *
 * The cursor holds a pointer to the AetherBytes plus a read offset; it
 * does not copy the data and does not own the buffer (see the BORROW
 * note in the header). All reads go through the public aether_bytes_get
 * / aether_bytes_length accessors so the cursor never pokes the
 * AetherBytes internals and always honours the buffer's logical
 * length. */

#include "aether_bytes_cursor.h"
#include "../../../runtime/aether_resource_caps.h"

struct BytesCursor {
    AetherBytes*         bytes;  /* borrowed buffer, or NULL for a slice */
    const unsigned char* data;   /* borrowed slice memory (#2301), or NULL */
    int                  len;    /* snapshot of the logical length */
    int                  pos;    /* current read offset, in [0, len] */
};

/* Byte `i` (known to be in range) of whichever backing the cursor has. */
static int cursor_byte(const BytesCursor* c, int i) {
    return c->data ? (int)c->data[i] : aether_bytes_get(c->bytes, i);
}

BytesCursor* bytes_cursor_new(AetherBytes* b) {
    if (!b) return NULL;
    int len = aether_bytes_length(b);
    if (len < 0) return NULL;
    BytesCursor* c = (BytesCursor*)aether_caps_malloc(sizeof(*c));
    if (!c) return NULL;
    c->bytes = b;
    c->data = NULL;
    c->len = len;
    c->pos = 0;
    return c;
}

BytesCursor* bytes_cursor_new_from_ptr(const void* data, int len) {
    if ((!data && len > 0) || len < 0) return NULL;
    BytesCursor* c = (BytesCursor*)aether_caps_malloc(sizeof(*c));
    if (!c) return NULL;
    c->bytes = NULL;
    c->data = (const unsigned char*)data;
    c->len = len;
    c->pos = 0;
    return c;
}

const void* bytes_cursor_read_view(BytesCursor* c, int n) {
    if (!c || n < 0 || n > c->len - c->pos) return NULL;
    const unsigned char* base = c->data ? c->data
                                        : (const unsigned char*)aether_bytes_data(c->bytes);
    if (!base) return NULL;
    const void* at = base + c->pos;
    c->pos += n;
    return at;
}

int bytes_cursor_read_u8(BytesCursor* c) {
    if (!c || c->pos >= c->len) return -1;
    int v = cursor_byte(c, c->pos);
    if (v < 0) return -1;
    c->pos += 1;
    return v;
}

int bytes_cursor_read_be_u16(BytesCursor* c) {
    if (!c || c->pos + 2 > c->len) return -1;
    int hi = cursor_byte(c, c->pos);
    int lo = cursor_byte(c, c->pos + 1);
    if (hi < 0 || lo < 0) return -1;
    c->pos += 2;
    return (hi << 8) | lo;
}

int bytes_cursor_read_be_u32(BytesCursor* c) {
    if (!c || c->pos + 4 > c->len) return -1;
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        int b = cursor_byte(c, c->pos + i);
        if (b < 0) return -1;
        v = (v << 8) | (unsigned int)b;
    }
    c->pos += 4;
    return (int)v;
}

long long bytes_cursor_read_be_u64(BytesCursor* c) {
    if (!c || c->pos + 8 > c->len) return -1;
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) {
        int b = cursor_byte(c, c->pos + i);
        if (b < 0) return -1;
        v = (v << 8) | (unsigned long long)b;
    }
    c->pos += 8;
    return (long long)v;
}

int bytes_cursor_read_le_u16(BytesCursor* c) {
    if (!c || c->pos + 2 > c->len) return -1;
    int lo = cursor_byte(c, c->pos);
    int hi = cursor_byte(c, c->pos + 1);
    if (lo < 0 || hi < 0) return -1;
    c->pos += 2;
    return (hi << 8) | lo;
}

int bytes_cursor_read_le_u32(BytesCursor* c) {
    if (!c || c->pos + 4 > c->len) return -1;
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        int b = cursor_byte(c, c->pos + i);
        if (b < 0) return -1;
        v |= (unsigned int)b << (8 * i);
    }
    c->pos += 4;
    return (int)v;
}

long long bytes_cursor_read_le_u64(BytesCursor* c) {
    if (!c || c->pos + 8 > c->len) return -1;
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) {
        int b = cursor_byte(c, c->pos + i);
        if (b < 0) return -1;
        v |= (unsigned long long)b << (8 * i);
    }
    c->pos += 8;
    return (long long)v;
}

AetherBytes* bytes_cursor_read_slice(BytesCursor* c, int n) {
    if (!c || n < 0 || c->pos + n > c->len) return NULL;
    AetherBytes* out = aether_bytes_new(n);
    if (!out) return NULL;
    if (n > 0) {
        int copied = c->data
            ? aether_bytes_copy_from_ptr(out, 0, c->data + c->pos, n)
            : aether_bytes_copy_from_bytes(out, 0, c->bytes, c->pos, n);
        if (!copied) {
            aether_bytes_free(out);
            return NULL;
        }
    }
    c->pos += n;
    return out;
}

int bytes_cursor_remaining(BytesCursor* c) {
    if (!c) return 0;
    return c->len - c->pos;
}

int bytes_cursor_peek(BytesCursor* c) {
    if (!c || c->pos >= c->len) return -1;
    return cursor_byte(c, c->pos);
}

int bytes_cursor_eof(BytesCursor* c) {
    if (!c) return 1;
    return c->pos >= c->len ? 1 : 0;
}

int bytes_cursor_pos(BytesCursor* c) {
    return c ? c->pos : -1;
}

void bytes_cursor_seek(BytesCursor* c, int pos) {
    if (!c) return;
    if (pos < 0) pos = 0;
    if (pos > c->len) pos = c->len;
    c->pos = pos;
}

void bytes_cursor_free(BytesCursor* c) {
    if (!c) return;
    /* Cursor borrows its AetherBytes or slice: do NOT free either here.
     * Only the cursor struct is ours to reclaim. */
    aether_caps_free(c, sizeof(*c));
}
