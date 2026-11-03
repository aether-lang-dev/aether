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
#include "../string/aether_string.h"
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

/* The accumulated bytes as a fresh counted string, leaving `s` empty. A copy:
 * the parser's buffers come from here too and are freed with free(), so the
 * buffer is not a cap-accounted one string_adopt_caps_buffer could take.
 * NULL on OOM. */
static AetherString* sb_finish_string(Sb* s) {
    AetherString* out = s->oom ? NULL : string_new_with_length(s->data, s->len);
    free(s->data);
    sb_init(s);
    return out;
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

/* A C0 control character other than tab, LF and CR: production [2] Char
 * leaves them out of every document, raw as much as by reference (#2510). */
static int is_forbidden_control(char c) {
    unsigned char u = (unsigned char)c;
    return u < 0x20 && u != 0x9 && u != 0xA && u != 0xD;
}

/* ASCII letters, '_' and ':' start a Name; digits, '.' and '-' may follow.
 * A byte of a multi-byte UTF-8 sequence is taken as a name character, so
 * non-ASCII names are not refused (the reader does not decode them). */
static int is_name_start_byte(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u == ':' || u >= 0x80;
}

static int is_name_byte(char c) {
    return is_name_start_byte(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

/* Decode XML character data [p, p+n) into a freshly malloc'd string in
 * *out with references resolved. Returns 0 on success, -1 on allocation
 * failure, or a negative XML_BAD_* with *bad set to the offset of the
 * offending byte: a character reference that is malformed or not a Char
 * (#2471), a '&' that starts no reference, a reference to an entity other
 * than the five predefined ones (this reader declares none and reads no
 * DTD, so XML 1.0's WFC: Entity Declared makes it an error), or a raw
 * control character (#2510). */
enum {
    XML_BAD_CHAR = -2,      /* character reference to a non-Char */
    XML_BAD_CHARREF = -3,   /* malformed character reference */
    XML_BAD_AMP = -4,       /* '&' that starts no reference */
    XML_BAD_ENTITY = -5,    /* undeclared entity */
    XML_BAD_CONTROL = -6    /* raw control character */
};

/* With `keep_undeclared`, a reference to an entity other than the five
 * predefined ones is kept as written: the document's DOCTYPE declared
 * entities this reader does not read, so the reference may well be
 * declared (XML 1.0 WFC: Entity Declared binds only a document with no DTD,
 * or a standalone one). */
static int xml_decode(const char* p, size_t n, char** out_text, size_t* bad,
                      int keep_undeclared) {
    Sb out;
    sb_init(&out);
    size_t i = 0;
    while (i < n) {
        char c = p[i];
        if (c != '&') {
            if (is_forbidden_control(c)) {
                free(out.data);
                *bad = i;
                return XML_BAD_CONTROL;
            }
            sb_putc(&out, c);
            i++;
            continue;
        }
        if (i + 1 < n && p[i + 1] == '#') {
            unsigned long cp = 0;
            size_t next = read_char_ref(p, n, i, &cp);
            if (next == 0 || !is_xml_char(cp)) {
                free(out.data);
                *bad = i;
                return next == 0 ? XML_BAD_CHARREF : XML_BAD_CHAR;
            }
            sb_put_utf8(&out, cp);
            i = next;
            continue;
        }
        /* EntityRef ::= '&' Name ';'. A bare '&' (production [14] keeps
         * it out of character data) and a name other than the five
         * predefined ones used to pass through verbatim. */
        size_t semi = i + 1;
        if (semi < n && is_name_start_byte(p[semi])) {
            while (semi < n && is_name_byte(p[semi])) semi++;
        }
        if (semi == i + 1 || semi >= n || p[semi] != ';') {
            free(out.data);
            *bad = i;
            return XML_BAD_AMP;
        }
        size_t elen = semi - (i + 1);
        const char* e = p + i + 1;
        if (elen == 3 && memcmp(e, "amp", 3) == 0)       sb_putc(&out, '&');
        else if (elen == 2 && memcmp(e, "lt", 2) == 0)   sb_putc(&out, '<');
        else if (elen == 2 && memcmp(e, "gt", 2) == 0)   sb_putc(&out, '>');
        else if (elen == 4 && memcmp(e, "quot", 4) == 0) sb_putc(&out, '"');
        else if (elen == 4 && memcmp(e, "apos", 4) == 0) sb_putc(&out, '\'');
        else if (keep_undeclared) {
            for (size_t q = i; q <= semi; q++) sb_putc(&out, p[q]);
        } else {
            free(out.data);
            *bad = i;
            return XML_BAD_ENTITY;
        }
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
    int      roots;           /* top-level elements started so far */
    int      dtd_entities;    /* the DOCTYPE's internal subset declares entities */
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
    /* A UTF-8 byte order mark may open a document; it is not text outside
     * the root element. */
    if (len >= 3 && (unsigned char)p->buf[0] == 0xEF && (unsigned char)p->buf[1] == 0xBB &&
        (unsigned char)p->buf[2] == 0xBF) {
        p->pos = 3;
    }
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
    int rc = xml_decode(p->buf + start, end - start, &text, &bad, p->dtd_entities);
    if (rc == XML_BAD_CHAR) {
        xml_fail_at(p, start + bad, "character reference to a character XML does not allow");
        return NULL;
    }
    if (rc == XML_BAD_CHARREF) {
        xml_fail_at(p, start + bad, "malformed character reference");
        return NULL;
    }
    if (rc == XML_BAD_AMP) {
        xml_fail_at(p, start + bad, "'&' that does not start a reference (write &amp;)");
        return NULL;
    }
    if (rc == XML_BAD_ENTITY) {
        /* Name the entity: [&, ;] is within the run that was decoded. */
        const char* e = p->buf + start + bad + 1;
        const char* semi = memchr(e, ';', end - (start + bad + 1));
        int nlen = semi ? (int)(semi - e) : 0;
        char msg[160];
        snprintf(msg, sizeof(msg), "reference to undeclared entity &%.*s; (only &amp; &lt; &gt; &quot; &apos; are predefined)",
                 nlen > 60 ? 60 : nlen, e);
        xml_fail_at(p, start + bad, msg);
        return NULL;
    }
    if (rc == XML_BAD_CONTROL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "control character 0x%02X, which XML does not allow",
                 (unsigned)(unsigned char)p->buf[start + bad]);
        xml_fail_at(p, start + bad, msg);
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
            /* A document is exactly one element (production [1]) (#2510). */
            if (p->roots == 0) return xml_fail(p, "no root element");
            return XML_EVENT_EOF;
        }

        if (p->buf[p->pos] != '<') {
            /* Character data up to the next '<'. Outside the root element
             * only whitespace may appear (productions [1] and [27]). */
            size_t start = p->pos;
            while (p->pos < p->len && p->buf[p->pos] != '<') p->pos++;
            if (p->depth == 0) {
                for (size_t k = start; k < p->pos; k++) {
                    if (!is_xml_space(p->buf[k]))
                        return xml_fail_at(p, k, "text outside the root element");
                }
            }
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
            if (p->depth == 0) return xml_fail(p, "CDATA outside the root element");
            size_t s = p->pos + 9;
            const char* end = strstr(p->buf + s, "]]>");
            if (!end) return xml_fail(p, "unterminated CDATA");
            size_t n = (size_t)(end - (p->buf + s));
            for (size_t k = s; k < s + n; k++) {
                if (is_forbidden_control(p->buf[k])) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "control character 0x%02X, which XML does not allow",
                             (unsigned)(unsigned char)p->buf[k]);
                    return xml_fail_at(p, k, msg);
                }
            }
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
                /* An entity declared in the internal subset: references
                 * to names this reader does not know are then kept as
                 * written rather than refused (xml_decode). */
                else if (depth > 0 && c == '<' && p->len - i >= 8 &&
                         memcmp(p->buf + i, "<!ENTITY", 8) == 0) {
                    p->dtd_entities = 1;
                }
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
        size_t tag = p->pos;
        p->pos++;  /* past '<' */
        p->name = read_name(p);
        if (!p->name) return xml_fail(p, "out of memory reading start tag");
        if (p->name[0] == '\0') return xml_fail(p, "empty element name");
        if (p->depth == 0) {
            /* A second top-level element (#2510). */
            if (p->roots > 0) {
                char msg[160];
                snprintf(msg, sizeof(msg), "second root element <%.100s>: a document has one", p->name);
                return xml_fail_at(p, tag, msg);
            }
            p->roots++;
        }

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
            size_t at = p->pos;
            char* aname = read_name(p);
            if (!aname) return xml_fail(p, "out of memory reading attribute");
            if (aname[0] == '\0') { free(aname); return xml_fail(p, "malformed attribute"); }
            /* WFC: Unique Att Spec (#2510). */
            for (int k = 0; k < p->attr_count; k++) {
                if (strcmp(p->attrs[k].name, aname) == 0) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "attribute %.100s appears twice in one tag", aname);
                    free(aname);
                    return xml_fail_at(p, at, msg);
                }
            }
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
    return (char*)sb_finish_string(&out);
}

/* ------------------------------------------------------------------ */
/* Builder                                                            */
/* ------------------------------------------------------------------ */

struct XmlBuilder {
    Sb   sb;
    int  tag_open;   /* an emitted start tag still awaits its closing '>' */
    int  pretty_indent; /* -1 for normal mode, 0-16 for pretty mode */
};

XmlBuilder* xml_builder_new(void) {
    XmlBuilder* b = (XmlBuilder*)aether_caps_calloc(1, sizeof(XmlBuilder));
    if (!b) return NULL;
    sb_init(&b->sb);
    b->pretty_indent = -1;
    return b;
}

XmlBuilder* xml_builder_pretty_new(int indent) {
    XmlBuilder* b = (XmlBuilder*)aether_caps_calloc(1, sizeof(XmlBuilder));
    if (!b) return NULL;
    sb_init(&b->sb);
    b->pretty_indent = indent;
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

/* Forward declaration */
char* xml_pretty(const char* data, size_t len, int indent, char** out_err);

char* xml_builder_finish(XmlBuilder* b) {
    if (!b) return NULL;
    close_open_tag(b);
    if (b->pretty_indent < 0) {
        return (char*)sb_finish_string(&b->sb);
    }
    /* Pretty mode: format the accumulated string */
    char* raw = sb_finish(&b->sb);
    sb_init(&b->sb);
    if (!raw) return NULL;
    char* err = NULL;
    size_t raw_len = strlen(raw);
    char* formatted = xml_pretty(raw, raw_len, b->pretty_indent, &err);
    if (err) free(err);
    if (!formatted) {
        /* Not a whole document yet (an element left open, no root): give
         * back what was written, unformatted, as the plain writer does,
         * rather than lose it. */
        formatted = (char*)string_new_with_length(raw, raw_len);
    }
    free(raw);
    return formatted;
}

/* ------------------------------------------------------------------ */
/* Pretty printing                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    NODE_TOP_ITEM = 1,
    NODE_TEXT,
    NODE_CDATA,
    NODE_ELEMENT,
    NODE_MISC        /* comment, PI or DOCTYPE inside an element: kept verbatim */
} PrettyNodeKind;

typedef struct PrettyNode PrettyNode;

struct PrettyNode {
    PrettyNodeKind kind;
    size_t span_start;
    size_t span_end;       /* For leaf/verbatim: [span_start, span_end) */
    size_t open_tag_start;  /* For element: <tag ...> */
    size_t open_tag_end;
    size_t close_tag_start; /* </tag> or 0 if self-closing */
    size_t close_tag_end;
    int    is_self_closing;
    int    xml_space_preserve;
    int    has_non_ws_text;
    int    has_element_child;
    int    child_count;
    int    child_cap;
    PrettyNode* children;
};

static void free_pretty_node(PrettyNode* n) {
    if (!n) return;
    for (int i = 0; i < n->child_count; i++) {
        free_pretty_node(&n->children[i]);
    }
    free(n->children);
    n->children = NULL;
    n->child_count = 0;
    n->child_cap = 0;
}

static int add_child_node(PrettyNode* parent, PrettyNode child) {
    if (parent->child_count >= parent->child_cap) {
        int ncap = parent->child_cap ? parent->child_cap * 2 : 8;
        PrettyNode* nc = (PrettyNode*)realloc(parent->children, (size_t)ncap * sizeof(PrettyNode));
        if (!nc) return 0;
        parent->children = nc;
        parent->child_cap = ncap;
    }
    parent->children[parent->child_count++] = child;
    return 1;
}

static void emit_indent(Sb* s, int level, int indent) {
    int total = level * indent;
    for (int i = 0; i < total; i++) sb_putc(s, ' ');
}

static void emit_verbatim_span(Sb* s, const char* data, size_t start, size_t end) {
    sb_append(s, data + start, end - start);
}

static void render_pretty_node(Sb* s, const PrettyNode* n, const char* data, int level, int indent, int parent_preserve) {
    int preserve = parent_preserve || n->xml_space_preserve;

    if (n->kind == NODE_TOP_ITEM) {
        emit_verbatim_span(s, data, n->span_start, n->span_end);
        sb_putc(s, '\n');
        return;
    }

    if (preserve) {
        emit_indent(s, level, indent);
        emit_verbatim_span(s, data, n->span_start, n->span_end);
        sb_putc(s, '\n');
        return;
    }

    if (n->is_self_closing) {
        emit_indent(s, level, indent);
        emit_verbatim_span(s, data, n->span_start, n->span_end);
        sb_putc(s, '\n');
        return;
    }

    if (n->has_element_child && n->has_non_ws_text) {
        /* Mixed content: emit whole element verbatim on its own line */
        emit_indent(s, level, indent);
        emit_verbatim_span(s, data, n->span_start, n->span_end);
        sb_putc(s, '\n');
        return;
    }

    if (!n->has_element_child) {
        /* Leaf element:
         * If it only has text/CDATA or comments/PIs, emit on one line:
         * <tag>content</tag>
         */
        emit_indent(s, level, indent);
        emit_verbatim_span(s, data, n->open_tag_start, n->open_tag_end);
        for (int i = 0; i < n->child_count; i++) {
            const PrettyNode* ch = &n->children[i];
            emit_verbatim_span(s, data, ch->span_start, ch->span_end);
        }
        emit_verbatim_span(s, data, n->close_tag_start, n->close_tag_end);
        sb_putc(s, '\n');
        return;
    }

    /* Element with child elements (and possibly comments, PIs, whitespace text) */
    emit_indent(s, level, indent);
    emit_verbatim_span(s, data, n->open_tag_start, n->open_tag_end);
    sb_putc(s, '\n');

    for (int i = 0; i < n->child_count; i++) {
        const PrettyNode* ch = &n->children[i];
        if (ch->kind == NODE_TEXT && !ch->has_non_ws_text) {
            /* Whitespace-only text child between elements: replaced by indentation */
            continue;
        }
        if (ch->kind == NODE_ELEMENT) {
            render_pretty_node(s, ch, data, level + 1, indent, 0);
        } else {
            /* Top-item-like child: comment, PI, CDATA, text */
            emit_indent(s, level + 1, indent);
            emit_verbatim_span(s, data, ch->span_start, ch->span_end);
            sb_putc(s, '\n');
        }
    }

    emit_indent(s, level, indent);
    emit_verbatim_span(s, data, n->close_tag_start, n->close_tag_end);
    sb_putc(s, '\n');
}

char* xml_pretty(const char* data, size_t len, int indent, char** out_err) {
    if (out_err) *out_err = NULL;

    if (indent < 0 || indent > 16) {
        if (out_err) {
            char msg[128];
            snprintf(msg, sizeof(msg), "indent must be 0 to 16, got %d", indent);
            *out_err = strdup(msg);
        }
        return NULL;
    }

    /* First, fully validate well-formedness using the pull reader */
    XmlParser* parser = xml_parser_new(data, len);
    if (!parser) {
        if (out_err) *out_err = strdup("out of memory creating parser");
        return NULL;
    }

    for (;;) {
        int ev = xml_next(parser);
        if (ev == XML_EVENT_ERROR) {
            if (out_err) *out_err = strdup(xml_parser_error(parser));
            xml_parser_free(parser);
            return NULL;
        }
        if (ev == XML_EVENT_EOF) break;
    }
    xml_parser_free(parser);

    /* The document is well-formed. Now build the PrettyNode tree. */
    PrettyNode root;
    memset(&root, 0, sizeof(root));

    /* Explicit stack for elements */
    size_t stack_cap = 32;
    PrettyNode** stack = (PrettyNode**)malloc(stack_cap * sizeof(PrettyNode*));
    if (!stack) {
        if (out_err) *out_err = strdup("out of memory");
        return NULL;
    }
    size_t stack_depth = 0;
    stack[stack_depth++] = &root;

    /* Handle UTF-8 BOM if present at start */
    size_t pos = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        pos = 3;
    }

    while (pos < len) {
        if (data[pos] != '<') {
            size_t start = pos;
            while (pos < len && data[pos] != '<') pos++;
            if (stack_depth > 1) {
                /* Text inside an element */
                PrettyNode* cur = stack[stack_depth - 1];
                int non_ws = 0;
                for (size_t k = start; k < pos; k++) {
                    if (!is_xml_space(data[k])) { non_ws = 1; break; }
                }
                if (non_ws) cur->has_non_ws_text = 1;

                PrettyNode tn;
                memset(&tn, 0, sizeof(tn));
                tn.kind = NODE_TEXT;
                tn.span_start = start;
                tn.span_end = pos;
                tn.has_non_ws_text = non_ws;
                if (!add_child_node(cur, tn)) goto parse_oom;
            }
            continue;
        }

        /* Markup '<' */
        size_t remain = len - pos;
        if (remain >= 4 && memcmp(data + pos, "<!--", 4) == 0) {
            const char* end = strstr(data + pos + 4, "-->");
            size_t end_pos = (size_t)(end - data) + 3;
            PrettyNode* cur = stack[stack_depth - 1];
            PrettyNode cn;
            memset(&cn, 0, sizeof(cn));
            cn.kind = (stack_depth == 1) ? NODE_TOP_ITEM : NODE_MISC;
            cn.span_start = pos;
            cn.span_end = end_pos;
            if (!add_child_node(cur, cn)) goto parse_oom;
            pos = end_pos;
            continue;
        }

        if (remain >= 9 && memcmp(data + pos, "<![CDATA[", 9) == 0) {
            const char* end = strstr(data + pos + 9, "]]>");
            size_t end_pos = (size_t)(end - data) + 3;
            PrettyNode* cur = stack[stack_depth - 1];
            cur->has_non_ws_text = 1; /* CDATA counts as non-whitespace content */
            PrettyNode cdn;
            memset(&cdn, 0, sizeof(cdn));
            cdn.kind = NODE_CDATA;
            cdn.span_start = pos;
            cdn.span_end = end_pos;
            cdn.has_non_ws_text = 1;
            if (!add_child_node(cur, cdn)) goto parse_oom;
            pos = end_pos;
            continue;
        }

        if (remain >= 2 && data[pos + 1] == '?') {
            const char* end = strstr(data + pos + 2, "?>");
            size_t end_pos = (size_t)(end - data) + 2;
            PrettyNode* cur = stack[stack_depth - 1];
            PrettyNode pin;
            memset(&pin, 0, sizeof(pin));
            pin.kind = (stack_depth == 1) ? NODE_TOP_ITEM : NODE_MISC;
            pin.span_start = pos;
            pin.span_end = end_pos;
            if (!add_child_node(cur, pin)) goto parse_oom;
            pos = end_pos;
            continue;
        }

        if (remain >= 2 && data[pos + 1] == '!') {
            /* DOCTYPE */
            size_t i = pos + 2;
            int bdepth = 0;
            while (i < len) {
                char c = data[i];
                if (c == '[') bdepth++;
                else if (c == ']') { if (bdepth > 0) bdepth--; }
                else if (c == '>' && bdepth == 0) break;
                i++;
            }
            size_t end_pos = i + 1;
            PrettyNode* cur = stack[stack_depth - 1];
            PrettyNode dtn;
            memset(&dtn, 0, sizeof(dtn));
            dtn.kind = (stack_depth == 1) ? NODE_TOP_ITEM : NODE_MISC;
            dtn.span_start = pos;
            dtn.span_end = end_pos;
            if (!add_child_node(cur, dtn)) goto parse_oom;
            pos = end_pos;
            continue;
        }

        if (remain >= 2 && data[pos + 1] == '/') {
            /* End element </name> */
            size_t start = pos;
            while (pos < len && data[pos] != '>') pos++;
            size_t end_pos = pos + 1;
            PrettyNode* elem = stack[stack_depth - 1];
            elem->close_tag_start = start;
            elem->close_tag_end = end_pos;
            elem->span_end = end_pos;
            stack_depth--;
            pos = end_pos;
            continue;
        }

        /* Start element <name ... > or <name ... /> */
        size_t elem_start = pos;
        size_t p = pos + 1;
        while (p < len && is_name_char(data[p])) p++;
        int is_self_closing = 0;
        int xml_space_preserve = 0;

        for (;;) {
            while (p < len && is_xml_space(data[p])) p++;
            if (p >= len) break;
            if (data[p] == '>') {
                p++;
                break;
            }
            if (data[p] == '/' && p + 1 < len && data[p + 1] == '>') {
                is_self_closing = 1;
                p += 2;
                break;
            }
            /* Attribute */
            size_t aname_start = p;
            while (p < len && is_name_char(data[p])) p++;
            size_t aname_len = p - aname_start;
            while (p < len && is_xml_space(data[p])) p++;
            if (p < len && data[p] == '=') p++;
            while (p < len && is_xml_space(data[p])) p++;
            if (p < len && (data[p] == '"' || data[p] == '\'')) {
                char q = data[p++];
                size_t vstart = p;
                while (p < len && data[p] != q) p++;
                size_t vend = p;
                if (p < len) p++; /* past quote */

                if (aname_len == 9 && memcmp(data + aname_start, "xml:space", 9) == 0) {
                    if (vend - vstart == 8 && memcmp(data + vstart, "preserve", 8) == 0) {
                        xml_space_preserve = 1;
                    }
                }
            }
        }
        size_t open_tag_end = p;
        PrettyNode* cur = stack[stack_depth - 1];
        cur->has_element_child = 1;

        PrettyNode el;
        memset(&el, 0, sizeof(el));
        el.kind = NODE_ELEMENT;
        el.span_start = elem_start;
        el.open_tag_start = elem_start;
        el.open_tag_end = open_tag_end;
        el.is_self_closing = is_self_closing;
        el.xml_space_preserve = xml_space_preserve;

        if (is_self_closing) {
            el.span_end = open_tag_end;
            if (!add_child_node(cur, el)) goto parse_oom;
        } else {
            if (!add_child_node(cur, el)) goto parse_oom;
            PrettyNode* pushed = &cur->children[cur->child_count - 1];
            if (stack_depth >= stack_cap) {
                size_t ncap = stack_cap * 2;
                PrettyNode** ns = (PrettyNode**)realloc(stack, ncap * sizeof(PrettyNode*));
                if (!ns) goto parse_oom;
                stack = ns;
                stack_cap = ncap;
            }
            stack[stack_depth++] = pushed;
        }
        pos = open_tag_end;
    }

    /* Output buffer */
    Sb out;
    sb_init(&out);

    /* If BOM was present at the start of input, output it directly */
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        sb_append(&out, data, 3);
    }

    for (int i = 0; i < root.child_count; i++) {
        render_pretty_node(&out, &root.children[i], data, 0, indent, 0);
    }

    free_pretty_node(&root);
    free(stack);

    AetherString* astr = sb_finish_string(&out);
    if (!astr) {
        if (out_err) *out_err = strdup("out of memory assembling output");
        return NULL;
    }
    return (char*)astr;

parse_oom:
    free_pretty_node(&root);
    free(stack);
    if (out_err) *out_err = strdup("out of memory");
    return NULL;
}

char* xml_pretty_str(const char* s, int indent, char** out_err) {
    return xml_pretty(s ? s : "", s ? strlen(s) : 0, indent, out_err);
}

/* The (formatted, error) tuple xml.pretty's extern returns; generated code
 * declares the same layout. */
typedef struct { const char* _0; const char* _1; } _tuple_string_string;

_tuple_string_string xml_pretty_tuple(const char* s, int indent);
_tuple_string_string xml_pretty_tuple(const char* s, int indent) {
    char* err = NULL;
    char* formatted = xml_pretty_str(s, indent, &err);
    _tuple_string_string out;
    if (formatted) {
        out._0 = formatted;
        out._1 = "";
    } else {
        out._0 = (const char*)string_empty();
        out._1 = err ? (const char*)string_new_with_length(err, strlen(err)) : "";
        if (err) free(err);
    }
    return out;
}

