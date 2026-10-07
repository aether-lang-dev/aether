/*
 * Aether Programming Language - std.xml
 * Copyright (c) 2025 Aether Programming Language Contributors
 * Licensed under the MIT License. See LICENSE file in the project root.
 *
 * Small, dependency-free XML: a pull/SAX reader and an escaping builder
 * (issue #627). Scope is deliberately limited to what S3 / SOAP-ish /
 * config XML needs — see aether_xml.h.
 */
#include "aether_xml.h"
#include "../mem/aether_grow.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
    int    oom;   /* sticky: a grow failed */
} Sb;

static void sb_init(Sb* s) { s->data = NULL; s->len = 0; s->cap = 0; s->oom = 0; }

static int sb_reserve(Sb* s, size_t extra) {
    if (s->oom) return 0;
    if (s->len + extra + 1 <= s->cap) return 1;
    size_t want = aether_buf_grow_capacity(s->cap, 64, s->len + extra + 1, 1);
    if (!want) { s->oom = 1; return 0; }
    char* p = (char*)realloc(s->data, want);
    if (!p) { s->oom = 1; return 0; }
    s->data = p;
    s->cap = want;
    return 1;
}

static void sb_putc(Sb* s, char c) {
    if (!sb_reserve(s, 1)) return;
    s->data[s->len++] = c;
}

static void sb_append(Sb* s, const char* p, size_t n) {
    if (n == 0) return;
    if (!sb_reserve(s, n)) return;
    memcpy(s->data + s->len, p, n);
    s->len += n;
}

static void sb_puts(Sb* s, const char* str) { sb_append(s, str, strlen(str)); }

/* NUL-terminate and hand off the buffer (caller frees). */
static char* sb_finish(Sb* s) {
    if (s->oom) { free(s->data); return NULL; }
    if (!s->data) { return strdup(""); }
    s->data[s->len] = '\0';
    return s->data;  /* ownership transferred */
}

/* ------------------------------------------------------------------ */
/* Entity decode (& < > " ' and numeric &#NN; / &#xHH;)               */
/* ------------------------------------------------------------------ */

/* Encode a code point as UTF-8 into sb. The caller has checked it is an XML
 * Char (is_xml_char), so it is a scalar value: never a surrogate, never
 * past U+10FFFF. */
static void sb_put_utf8(Sb* s, unsigned long cp) {
    if (cp <= 0x7F) {
        sb_putc(s, (char)cp);
    } else if (cp <= 0x7FF) {
        sb_putc(s, (char)(0xC0 | (cp >> 6)));
        sb_putc(s, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        sb_putc(s, (char)(0xE0 | (cp >> 12)));
        sb_putc(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(s, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(s, (char)(0xF0 | (cp >> 18)));
        sb_putc(s, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(s, (char)(0x80 | (cp & 0x3F)));
    }
}

/* XML 1.0 production [2] Char: #x9 | #xA | #xD | [#x20-#xD7FF] |
 * [#xE000-#xFFFD] | [#x10000-#x10FFFF]. Section 4.1 makes it a
 * well-formedness constraint that a character reference names one (#2471):
 * &#0; used to cut the text short at the NUL, a surrogate came out as
 * invalid UTF-8, and a reference past U+10FFFF vanished. */
static int is_xml_char(unsigned long cp) {
    return cp == 0x9 || cp == 0xA || cp == 0xD ||
           (cp >= 0x20 && cp <= 0xD7FF) ||
           (cp >= 0xE000 && cp <= 0xFFFD) ||
           (cp >= 0x10000 && cp <= 0x10FFFF);
}

/* Read the character reference at p[i] == '&', p[i+1] == '#' in [p, p+n):
 * '&#' [0-9]+ ';' or '&#x' [0-9a-fA-F]+ ';' (lowercase x only, as the
 * production has it). Returns the offset just past the ';' and sets *cp, or
 * 0 when the reference is malformed. The value saturates past U+10FFFF, so
 * any number of leading zeros or digits is read without overflow. */
static size_t read_char_ref(const char* p, size_t n, size_t i, unsigned long* cp) {
    size_t k = i + 2;
    int hex = 0;
    if (k < n && p[k] == 'x') { hex = 1; k++; }
    size_t digits = k;
    unsigned long v = 0;
    for (; k < n; k++) {
        char h = p[k];
        int d = (h >= '0' && h <= '9') ? h - '0'
              : (hex && h >= 'a' && h <= 'f') ? h - 'a' + 10
              : (hex && h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * (hex ? 16 : 10) + (unsigned long)d;
        if (v > 0x10FFFF) v = 0x110000;
    }
    if (k == digits || k >= n || p[k] != ';') return 0;
    *cp = v;
    return k + 1;
}

/* Decode XML character data [p, p+n) into a freshly malloc'd string in
 * *out with references resolved. Returns 0 on success, -1 on allocation
 * failure, and -2 (not a Char) or -3 (malformed) with *bad set to the
 * offset of the '&' when a character reference is not well-formed (#2471).
 * Named references other than the five predefined ones are passed through
 * verbatim (lenient, as before: this reader declares no entities). */
static int xml_decode(const char* p, size_t n, char** out_text, size_t* bad) {
    Sb out;
    sb_init(&out);
    size_t i = 0;
    while (i < n) {
        char c = p[i];
        if (c != '&') { sb_putc(&out, c); i++; continue; }
        if (i + 1 < n && p[i + 1] == '#') {
            unsigned long cp = 0;
            size_t next = read_char_ref(p, n, i, &cp);
            if (next == 0 || !is_xml_char(cp)) {
                free(out.data);
                *bad = i;
                return next == 0 ? -3 : -2;
            }
            sb_put_utf8(&out, cp);
            i = next;
            continue;
        }
        /* Find the terminating ';' within a sane window. */
        size_t semi = i + 1;
        while (semi < n && semi < i + 12 && p[semi] != ';') semi++;
        if (semi >= n || p[semi] != ';') { sb_putc(&out, c); i++; continue; }
        size_t elen = semi - (i + 1);
        const char* e = p + i + 1;
        if (elen == 3 && memcmp(e, "amp", 3) == 0)       sb_putc(&out, '&');
        else if (elen == 2 && memcmp(e, "lt", 2) == 0)   sb_putc(&out, '<');
        else if (elen == 2 && memcmp(e, "gt", 2) == 0)   sb_putc(&out, '>');
        else if (elen == 4 && memcmp(e, "quot", 4) == 0) sb_putc(&out, '"');
        else if (elen == 4 && memcmp(e, "apos", 4) == 0) sb_putc(&out, '\'');
        else sb_append(&out, p + i, semi - i + 1);  /* unknown entity: verbatim */
        i = semi + 1;
    }
    *out_text = sb_finish(&out);
    return *out_text ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Pull reader                                                        */
/* ------------------------------------------------------------------ */

typedef struct { char* name; char* value; } XmlAttr;

struct XmlParser {
    char*    buf;
    size_t   len;
    size_t   pos;
    char*    name;            /* current element name */
    char*    text;            /* current decoded text */
    XmlAttr* attrs;
    int      attr_count;
    int      attr_cap;
    char*    pending_end;     /* self-close: deferred </name> */
    char**   open;            /* names of the elements open at pos, outermost first */
    int      depth;
    int      open_cap;
    int      errored;
    char     err[256];
};

static void clear_attrs(XmlParser* p) {
    for (int i = 0; i < p->attr_count; i++) {
        free(p->attrs[i].name);
        free(p->attrs[i].value);
    }
    p->attr_count = 0;
}

static void reset_event(XmlParser* p) {
    free(p->name); p->name = NULL;
    free(p->text); p->text = NULL;
    clear_attrs(p);
}

XmlParser* xml_parser_new(const char* data, size_t len) {
    XmlParser* p = (XmlParser*)aether_caps_calloc(1, sizeof(XmlParser));
    if (!p) return NULL;
    p->buf = (char*)malloc(len + 1);
    if (!p->buf) { aether_caps_free(p, sizeof(XmlParser)); return NULL; }
    if (len) memcpy(p->buf, data, len);
    p->buf[len] = '\0';
    p->len = len;
    return p;
}

/* Convenience for the Aether boundary: build a reader from a
 * NUL-terminated string (S3/SOAP/config bodies are text, so strlen is the
 * right length). For binary-safe input use xml_parser_new with an
 * explicit length. */
XmlParser* xml_parser_new_str(const char* s) {
    return xml_parser_new(s ? s : "", s ? strlen(s) : 0);
}

void xml_parser_free(XmlParser* p) {
    if (!p) return;
    reset_event(p);
    free(p->attrs);
    free(p->pending_end);
    for (int i = 0; i < p->depth; i++) free(p->open[i]);
    free(p->open);
    free(p->buf);
    aether_caps_free(p, sizeof(XmlParser));
}

/* Record the first error, positioned at byte `at` of the document as a
 * 1-based line and column (in bytes) and the byte offset. Every later
 * xml_next returns XML_EVENT_ERROR again. */
static int xml_fail_at(XmlParser* p, size_t at, const char* msg) {
    if (at > p->len) at = p->len;
    size_t line = 1, col = 1;
    for (size_t i = 0; i < at; i++) {
        if (p->buf[i] == '\n') { line++; col = 1; }
        else col++;
    }
    p->errored = 1;
    snprintf(p->err, sizeof(p->err), "%s (line %zu, column %zu, byte %zu)", msg, line, col, at);
    return XML_EVENT_ERROR;
}

static int xml_fail(XmlParser* p, const char* msg) {
    return xml_fail_at(p, p->pos, msg);
}

/* xml_decode for the run [start, end) of the document, failing the parser
 * at the offending reference. Returns the decoded text or NULL (the parser
 * has then failed). */
static char* decode_or_fail(XmlParser* p, size_t start, size_t end) {
    char* text = NULL;
    size_t bad = 0;
    int rc = xml_decode(p->buf + start, end - start, &text, &bad);
    if (rc == -2) {
        xml_fail_at(p, start + bad, "character reference to a character XML does not allow");
        return NULL;
    }
    if (rc == -3) {
        xml_fail_at(p, start + bad, "malformed character reference");
        return NULL;
    }
    if (rc != 0) {
        xml_fail_at(p, start, "out of memory decoding character data");
        return NULL;
    }
    return text;
}

static int push_open(XmlParser* p, const char* name) {
    if (p->depth >= p->open_cap) {
        int ncap = p->open_cap ? p->open_cap * 2 : 16;
        char** no = (char**)realloc(p->open, (size_t)ncap * sizeof(char*));
        if (!no) return 0;
        p->open = no;
        p->open_cap = ncap;
    }
    char* copy = strdup(name);
    if (!copy) return 0;
    p->open[p->depth++] = copy;
    return 1;
}

/* XML name chars (lenient): not whitespace, and not one of < > / = ? */
static int is_name_char(char c) {
    return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
             c == '<' || c == '>' || c == '/' || c == '=' || c == '?' ||
             c == '\0');
}

/* Production [3] S: space, tab, CR, LF. */
static int is_xml_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(XmlParser* p) {
    while (p->pos < p->len && is_xml_space(p->buf[p->pos])) p->pos++;
}

/* Read a name into a freshly malloc'd string (advances pos). */
static char* read_name(XmlParser* p) {
    size_t start = p->pos;
    while (p->pos < p->len && is_name_char(p->buf[p->pos])) p->pos++;
    size_t n = p->pos - start;
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, p->buf + start, n);
    s[n] = '\0';
    return s;
}

static int add_attr(XmlParser* p, char* name, char* value) {
    if (p->attr_count >= p->attr_cap) {
        int ncap = p->attr_cap ? p->attr_cap * 2 : 8;
        XmlAttr* na = (XmlAttr*)realloc(p->attrs, (size_t)ncap * sizeof(XmlAttr));
        if (!na) { free(name); free(value); return 0; }
        p->attrs = na;
        p->attr_cap = ncap;
    }
    p->attrs[p->attr_count].name = name;
    p->attrs[p->attr_count].value = value;
    p->attr_count++;
    return 1;
}

int xml_next(XmlParser* p) {
    if (!p) return XML_EVENT_EOF;
    if (p->errored) return XML_EVENT_ERROR;

    /* Deferred end from a self-closing element. */
    if (p->pending_end) {
        reset_event(p);
        p->name = p->pending_end;
        p->pending_end = NULL;
        return XML_EVENT_END_ELEMENT;
    }

    reset_event(p);

    for (;;) {
        if (p->pos >= p->len) {
            /* A document that ends inside an element is truncated (#2471). */
            if (p->depth > 0) {
                char msg[160];
                snprintf(msg, sizeof(msg), "unexpected end of document: <%.100s> is not closed",
                         p->open[p->depth - 1]);
                return xml_fail(p, msg);
            }
            return XML_EVENT_EOF;
        }

        if (p->buf[p->pos] != '<') {
            /* Character data up to the next '<'. */
            size_t start = p->pos;
            while (p->pos < p->len && p->buf[p->pos] != '<') p->pos++;
            p->text = decode_or_fail(p, start, p->pos);
            if (!p->text) return XML_EVENT_ERROR;
            return XML_EVENT_TEXT;
        }

        /* Markup. Look at what follows '<'. */
        size_t remain = p->len - p->pos;
        if (remain >= 4 && memcmp(p->buf + p->pos, "<!--", 4) == 0) {
            const char* end = strstr(p->buf + p->pos + 4, "-->");
            if (!end) return xml_fail(p, "unterminated comment");
            p->pos = (size_t)(end - p->buf) + 3;
            continue;
        }
        if (remain >= 9 && memcmp(p->buf + p->pos, "<![CDATA[", 9) == 0) {
            size_t s = p->pos + 9;
            const char* end = strstr(p->buf + s, "]]>");
            if (!end) return xml_fail(p, "unterminated CDATA");
            size_t n = (size_t)(end - (p->buf + s));
            p->text = (char*)malloc(n + 1);
            if (!p->text) return xml_fail(p, "out of memory in CDATA");
            memcpy(p->text, p->buf + s, n);
            p->text[n] = '\0';
            p->pos = (size_t)(end - p->buf) + 3;
            return XML_EVENT_TEXT;
        }
        if (remain >= 2 && p->buf[p->pos + 1] == '?') {
            /* Prolog <?xml ...?> or processing instruction — skip. */
            const char* end = strstr(p->buf + p->pos + 2, "?>");
            if (!end) return xml_fail(p, "unterminated processing instruction");
            p->pos = (size_t)(end - p->buf) + 2;
            continue;
        }
        if (remain >= 2 && p->buf[p->pos + 1] == '!') {
            /* DOCTYPE or other declaration — skip to matching '>',
             * tolerating a single bracketed internal subset. */
            size_t i = p->pos + 2;
            int depth = 0;
            while (i < p->len) {
                char c = p->buf[i];
                if (c == '[') depth++;
                else if (c == ']') { if (depth > 0) depth--; }
                else if (c == '>' && depth == 0) break;
                i++;
            }
            if (i >= p->len) return xml_fail(p, "unterminated declaration");
            p->pos = i + 1;
            continue;
        }
        if (remain >= 2 && p->buf[p->pos + 1] == '/') {
            /* End element </name>. It must close the innermost open element
             * (XML 1.0 WFC: Element Type Match); a pull reader that let a
             * mismatched, unopened or nameless end tag through handed back
             * events for a broken tree (#2471). */
            size_t tag = p->pos;
            p->pos += 2;
            p->name = read_name(p);
            if (!p->name) return xml_fail(p, "out of memory reading end tag");
            if (p->name[0] == '\0') return xml_fail_at(p, tag, "end tag without a name");
            skip_ws(p);
            if (p->pos >= p->len || p->buf[p->pos] != '>')
                return xml_fail(p, "malformed end tag");
            char msg[256];
            if (p->depth == 0) {
                snprintf(msg, sizeof(msg), "end tag </%.100s> has no open element to close", p->name);
                return xml_fail_at(p, tag, msg);
            }
            if (strcmp(p->name, p->open[p->depth - 1]) != 0) {
                snprintf(msg, sizeof(msg), "end tag </%.80s> does not match the open <%.80s>",
                         p->name, p->open[p->depth - 1]);
                return xml_fail_at(p, tag, msg);
            }
            p->pos++;
            free(p->open[--p->depth]);
            return XML_EVENT_END_ELEMENT;
        }

        /* Start element <name attr="v" ... > or <name/> */
        p->pos++;  /* past '<' */
        p->name = read_name(p);
        if (!p->name) return xml_fail(p, "out of memory reading start tag");
        if (p->name[0] == '\0') return xml_fail(p, "empty element name");

        for (;;) {
            /* Attributes are separated by whitespace (production [40]):
             * `<a x="1"y="2"/>` is not well-formed (#2471). */
            int spaced = p->pos < p->len && is_xml_space(p->buf[p->pos]);
            skip_ws(p);
            if (p->pos >= p->len) return xml_fail(p, "unterminated start tag");
            char c = p->buf[p->pos];
            if (c == '>') {
                p->pos++;
                if (!push_open(p, p->name)) return xml_fail(p, "out of memory");
                break;
            }
            if (c == '/') {
                if (p->pos + 1 >= p->len || p->buf[p->pos + 1] != '>')
                    return xml_fail(p, "malformed self-closing tag");
                p->pos += 2;
                /* Defer the matching END_ELEMENT to the next xml_next. */
                p->pending_end = strdup(p->name);
                if (!p->pending_end) return xml_fail(p, "out of memory");
                break;
            }
            if (!spaced) return xml_fail(p, "missing whitespace before attribute");
            /* attribute: name (ws) = (ws) quote value quote */
            char* aname = read_name(p);
            if (!aname) return xml_fail(p, "out of memory reading attribute");
            if (aname[0] == '\0') { free(aname); return xml_fail(p, "malformed attribute"); }
            skip_ws(p);
            if (p->pos >= p->len || p->buf[p->pos] != '=') {
                free(aname);
                return xml_fail(p, "expected '=' in attribute");
            }
            p->pos++;  /* past '=' */
            skip_ws(p);
            if (p->pos >= p->len || (p->buf[p->pos] != '"' && p->buf[p->pos] != '\'')) {
                free(aname);
                return xml_fail(p, "expected quoted attribute value");
            }
            char q = p->buf[p->pos++];
            size_t vstart = p->pos;
            while (p->pos < p->len && p->buf[p->pos] != q) p->pos++;
            if (p->pos >= p->len) { free(aname); return xml_fail(p, "unterminated attribute value"); }
            char* aval = decode_or_fail(p, vstart, p->pos);
            if (!aval) { free(aname); return XML_EVENT_ERROR; }
            p->pos++;  /* past closing quote */
            if (!add_attr(p, aname, aval)) return xml_fail(p, "out of memory storing attribute");
        }
        return XML_EVENT_START_ELEMENT;
    }
}

const char* xml_event_name(XmlParser* p) { return (p && p->name) ? p->name : ""; }
const char* xml_event_text(XmlParser* p) { return (p && p->text) ? p->text : ""; }
int         xml_event_attr_count(XmlParser* p) { return p ? p->attr_count : 0; }

const char* xml_event_attr_name(XmlParser* p, int i) {
    if (!p || i < 0 || i >= p->attr_count) return "";
    return p->attrs[i].name;
}
const char* xml_event_attr_value(XmlParser* p, int i) {
    if (!p || i < 0 || i >= p->attr_count) return "";
    return p->attrs[i].value;
}
const char* xml_event_attr(XmlParser* p, const char* name) {
    if (!p || !name) return NULL;
    for (int i = 0; i < p->attr_count; i++)
        if (strcmp(p->attrs[i].name, name) == 0) return p->attrs[i].value;
    return NULL;
}
/* Aether-boundary variant: "" (never NULL) for an absent attribute, so the
 * caller can string_concat it without a NULL guard. */
const char* xml_event_attr_str(XmlParser* p, const char* name) {
    const char* v = xml_event_attr(p, name);
    return v ? v : "";
}
const char* xml_parser_error(XmlParser* p) { return (p && p->errored) ? p->err : ""; }

/* ------------------------------------------------------------------ */
/* Escaping                                                           */
/* ------------------------------------------------------------------ */

static void sb_put_escaped(Sb* s, const char* p) {
    if (!p) return;
    for (; *p; p++) {
        switch (*p) {
            case '&':  sb_puts(s, "&amp;");  break;
            case '<':  sb_puts(s, "&lt;");   break;
            case '>':  sb_puts(s, "&gt;");   break;
            case '"':  sb_puts(s, "&quot;"); break;
            case '\'': sb_puts(s, "&apos;"); break;
            default:   sb_putc(s, *p);       break;
        }
    }
}

char* xml_escape(const char* s) {
    Sb out; sb_init(&out);
    sb_put_escaped(&out, s);
    return sb_finish(&out);
}

/* ------------------------------------------------------------------ */
/* Builder                                                            */
/* ------------------------------------------------------------------ */

struct XmlBuilder {
    Sb   sb;
    int  tag_open;   /* an emitted start tag still awaits its closing '>' */
};

XmlBuilder* xml_builder_new(void) {
    XmlBuilder* b = (XmlBuilder*)aether_caps_calloc(1, sizeof(XmlBuilder));
    if (!b) return NULL;
    sb_init(&b->sb);
    return b;
}

void xml_builder_free(XmlBuilder* b) {
    if (!b) return;
    free(b->sb.data);
    aether_caps_free(b, sizeof(XmlBuilder));
}

static void close_open_tag(XmlBuilder* b) {
    if (b->tag_open) { sb_putc(&b->sb, '>'); b->tag_open = 0; }
}

void xml_builder_declaration(XmlBuilder* b) {
    if (!b) return;
    close_open_tag(b);
    sb_puts(&b->sb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
}

void xml_builder_start(XmlBuilder* b, const char* name) {
    if (!b || !name) return;
    close_open_tag(b);
    sb_putc(&b->sb, '<');
    sb_puts(&b->sb, name);
    b->tag_open = 1;
}

void xml_builder_attr(XmlBuilder* b, const char* name, const char* value) {
    if (!b || !name || !b->tag_open) return;  /* only valid inside an open start tag */
    sb_putc(&b->sb, ' ');
    sb_puts(&b->sb, name);
    sb_puts(&b->sb, "=\"");
    sb_put_escaped(&b->sb, value);
    sb_putc(&b->sb, '"');
}

void xml_builder_text(XmlBuilder* b, const char* text) {
    if (!b) return;
    close_open_tag(b);
    sb_put_escaped(&b->sb, text);
}

void xml_builder_end(XmlBuilder* b, const char* name) {
    if (!b || !name) return;
    close_open_tag(b);
    sb_puts(&b->sb, "</");
    sb_puts(&b->sb, name);
    sb_putc(&b->sb, '>');
}

void xml_builder_element(XmlBuilder* b, const char* name, const char* text) {
    if (!b || !name) return;
    xml_builder_start(b, name);
    xml_builder_text(b, text);
    xml_builder_end(b, name);
}

char* xml_builder_finish(XmlBuilder* b) {
    if (!b) return NULL;
    close_open_tag(b);
    char* result = sb_finish(&b->sb);  /* transfers ownership of the buffer */
    /* Detach so a subsequent xml_builder_free() doesn't double-free the
     * buffer we just handed to the caller (sb_finish also frees it on OOM,
     * leaving a dangling pointer — nulling covers both). */
    b->sb.data = NULL;
    b->sb.len = 0;
    b->sb.cap = 0;
    return result;
}

