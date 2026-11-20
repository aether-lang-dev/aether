/* Coverage-guided fuzz target for std.xml (std/xml/aether_xml.c): the pull
 * reader, the escaping builder and the pretty printer.
 *
 * The input is a document, every byte of it, NUL bytes included. Each check
 * is a property, and a failure abort()s with what disagreed:
 *
 *   X1  The events nest: an end tag names the innermost open element, text
 *       outside the root is white space only, there is one root, EOF comes only with
 *       every element closed, and after EOF or an error the reader keeps
 *       answering the same (it does not resume past a refusal).
 *   X2  What the reader hands out is what its contract allows: element and
 *       attribute names are non-empty and hold no byte a name cannot (space,
 *       '<', '>', '=', '/', quotes, '&'), no tag has one attribute twice,
 *       xml_event_attr finds each by its name, and decoded text and values
 *       hold no control byte but tab, LF and CR (a reference to any other is
 *       refused). An error has a message ending in "(line L, column C,
 *       byte N)" with N inside the document.
 *   X3  A document the reader accepts, written again with the builder (each
 *       element, attribute and text escaped as the builder does), reads back
 *       to the same events, adjacent text joined.
 *   X4  xml_pretty agrees with the reader: it formats exactly the documents
 *       the reader accepts, and what it writes is accepted again, with the
 *       same elements and attributes in the same order.
 *   X5  Nothing leaks (LeakSanitizer). */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether_xml.h"
#include "aether_string.h"

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "fuzz_xml: property failed: %s\n  ", #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        abort(); \
    } \
} while (0)

/* One event as the reader gave it, kept past the next xml_next. */
typedef struct {
    int kind;
    char* name;        /* START, END */
    char* text;        /* TEXT (adjacent ones joined) */
    int nattr;
    char** an;
    char** av;
} Ev;

typedef struct {
    Ev* ev;
    int n, cap;
    int error;         /* ended in XML_EVENT_ERROR */
} Trace;

static char* dup_s(const char* s) {
    size_t n = strlen(s);
    char* d = (char*)malloc(n + 1);
    CHECK(d != NULL, "out of memory");
    memcpy(d, s, n + 1);
    return d;
}

static void trace_free(Trace* t) {
    for (int i = 0; i < t->n; i++) {
        Ev* e = &t->ev[i];
        free(e->name);
        free(e->text);
        for (int a = 0; a < e->nattr; a++) { free(e->an[a]); free(e->av[a]); }
        free(e->an);
        free(e->av);
    }
    free(t->ev);
    memset(t, 0, sizeof *t);
}

static Ev* trace_push(Trace* t, int kind) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->ev = (Ev*)realloc(t->ev, (size_t)t->cap * sizeof(Ev));
        CHECK(t->ev != NULL, "out of memory");
    }
    Ev* e = &t->ev[t->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}

static int name_ok(const char* s) {
    if (!*s) return 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        if (*p <= 0x20 || strchr("<>=/\"'&", *p)) return 0;
    return 1;
}

/* What XML 1.0's Char production leaves out of text and attribute values:
 * the C0 controls but tab, LF and CR. DEL (0x7f) is a Char ([#x20-#xD7FF]),
 * so text may hold it; the reader ends a name at one. */
static int chars_ok(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r') return 0;
    return 1;
}

/* Read every event of `data`, checking X1 and X2 as they come. */
static void read_doc(const char* data, size_t len, Trace* t) {
    memset(t, 0, sizeof *t);
    XmlParser* p = xml_parser_new(data, len);
    CHECK(p != NULL, "xml_parser_new returned NULL");
    int depth = 0, roots = 0, last = XML_EVENT_NONE;
    char** stack = NULL;
    int stack_cap = 0;
    for (;;) {
        int k = xml_next(p);
        CHECK(k >= XML_EVENT_START_ELEMENT && k <= XML_EVENT_ERROR, "xml_next returned %d", k);
        if (k == XML_EVENT_START_ELEMENT) {
            const char* name = xml_event_name(p);
            CHECK(name_ok(name), "element name '%s'", name);
            if (depth == 0) {
                roots++;
                CHECK(roots == 1, "a second root element <%s> was accepted", name);
            }
            Ev* e = trace_push(t, k);
            e->name = dup_s(name);
            e->nattr = xml_event_attr_count(p);
            CHECK(e->nattr >= 0, "attribute count %d", e->nattr);
            if (e->nattr) {
                e->an = (char**)calloc((size_t)e->nattr, sizeof(char*));
                e->av = (char**)calloc((size_t)e->nattr, sizeof(char*));
                CHECK(e->an && e->av, "out of memory");
            }
            for (int a = 0; a < e->nattr; a++) {
                const char* an = xml_event_attr_name(p, a);
                const char* av = xml_event_attr_value(p, a);
                CHECK(an && av, "attribute %d of <%s> is NULL", a, name);
                CHECK(name_ok(an), "attribute name '%s' on <%s>", an, name);
                CHECK(chars_ok(av), "attribute %s of <%s> holds a control byte", an, name);
                for (int b = 0; b < a; b++)
                    CHECK(strcmp(e->an[b], an) != 0, "<%s> has attribute %s twice", name, an);
                const char* found = xml_event_attr(p, an);
                CHECK(found && strcmp(found, av) == 0, "xml_event_attr(%s) on <%s> is not its value", an, name);
                e->an[a] = dup_s(an);
                e->av[a] = dup_s(av);
            }
            CHECK(xml_event_attr(p, "\x01no such attribute") == NULL, "an absent attribute was found");
            if (depth == stack_cap) {
                stack_cap = stack_cap ? stack_cap * 2 : 16;
                stack = (char**)realloc(stack, (size_t)stack_cap * sizeof(char*));
                CHECK(stack != NULL, "out of memory");
            }
            stack[depth++] = e->name;
        } else if (k == XML_EVENT_END_ELEMENT) {
            const char* name = xml_event_name(p);
            CHECK(depth > 0, "an end tag </%s> with no element open", name);
            CHECK(strcmp(stack[depth - 1], name) == 0, "</%s> closed <%s>", name, stack[depth - 1]);
            depth--;
            trace_push(t, k)->name = dup_s(name);
        } else if (k == XML_EVENT_TEXT) {
            const char* text = xml_event_text(p);
            /* Outside the root only white space is text (Misc, production [27]). */
            CHECK(depth > 0 || strspn(text, " \t\r\n") == strlen(text),
                  "text outside the root element: '%s'", text);
            CHECK(chars_ok(text), "text holds a control byte");
            if (last == XML_EVENT_TEXT) {
                Ev* e = &t->ev[t->n - 1];
                size_t a = strlen(e->text), b = strlen(text);
                e->text = (char*)realloc(e->text, a + b + 1);
                CHECK(e->text != NULL, "out of memory");
                memcpy(e->text + a, text, b + 1);
            } else {
                trace_push(t, k)->text = dup_s(text);
            }
        } else if (k == XML_EVENT_EOF) {
            CHECK(depth == 0, "EOF with <%s> still open", stack[depth - 1]);
            CHECK(roots == 1, "EOF with no root element");
            CHECK(xml_next(p) == XML_EVENT_EOF, "the reader went on after EOF");
            break;
        } else {
            const char* msg = xml_parser_error(p);
            CHECK(msg && *msg, "an error with no message");
            const char* at = strstr(msg, "(line ");
            unsigned long line = 0, col = 0, byte = 0;
            CHECK(at && sscanf(at, "(line %lu, column %lu, byte %lu)", &line, &col, &byte) == 3,
                  "the message '%s' does not say where", msg);
            CHECK(line >= 1 && col >= 1 && byte <= len, "the error is placed at line %lu column %lu byte %lu of %zu bytes",
                  line, col, byte, len);
            CHECK(xml_next(p) == XML_EVENT_ERROR, "the reader went on after an error");
            t->error = 1;
            break;
        }
        last = k;
    }
    free(stack);
    xml_parser_free(p);
}

/* X3: write `t` again with the builder. */
static char* rebuild(const Trace* t) {
    XmlBuilder* b = xml_builder_new();
    CHECK(b != NULL, "xml_builder_new returned NULL");
    for (int i = 0; i < t->n; i++) {
        const Ev* e = &t->ev[i];
        if (e->kind == XML_EVENT_START_ELEMENT) {
            xml_builder_start(b, e->name);
            for (int a = 0; a < e->nattr; a++) xml_builder_attr(b, e->an[a], e->av[a]);
        } else if (e->kind == XML_EVENT_END_ELEMENT) {
            xml_builder_end(b, e->name);
        } else {
            xml_builder_text(b, e->text);
        }
    }
    char* out = xml_builder_finish(b);
    xml_builder_free(b);
    CHECK(out != NULL, "xml_builder_finish returned NULL");
    char* copy = dup_s(aether_string_data(out));
    string_release(out);
    return copy;
}

static void same_events(const Trace* a, const Trace* b, int skip_blank_text, const char* what) {
    int i = 0, j = 0;
    for (;;) {
        if (skip_blank_text) {
            while (i < a->n && a->ev[i].kind == XML_EVENT_TEXT && strspn(a->ev[i].text, " \t\r\n") == strlen(a->ev[i].text)) i++;
            while (j < b->n && b->ev[j].kind == XML_EVENT_TEXT && strspn(b->ev[j].text, " \t\r\n") == strlen(b->ev[j].text)) j++;
        }
        if (i == a->n || j == b->n) break;
        const Ev* x = &a->ev[i];
        const Ev* y = &b->ev[j];
        CHECK(x->kind == y->kind, "%s: event %d is kind %d, then kind %d", what, i, x->kind, y->kind);
        if (x->kind == XML_EVENT_TEXT) {
            if (!skip_blank_text)
                CHECK(strcmp(x->text, y->text) == 0, "%s: text '%s' became '%s'", what, x->text, y->text);
        } else {
            CHECK(strcmp(x->name, y->name) == 0, "%s: <%s> became <%s>", what, x->name, y->name);
            CHECK(x->nattr == y->nattr, "%s: <%s> had %d attributes, then %d", what, x->name, x->nattr, y->nattr);
            for (int k = 0; k < x->nattr; k++)
                CHECK(strcmp(x->an[k], y->an[k]) == 0 && strcmp(x->av[k], y->av[k]) == 0,
                      "%s: <%s>'s attribute %s=\"%s\" became %s=\"%s\"", what, x->name, x->an[k], x->av[k],
                      y->an[k], y->av[k]);
        }
        i++;
        j++;
    }
    CHECK(i == a->n && j == b->n, "%s: %d events, then %d", what, a->n, b->n);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Trace t;
    read_doc((const char*)data, size, &t);

    if (!t.error) {
        /* X3 */
        char* again = rebuild(&t);
        Trace t2;
        read_doc(again, strlen(again), &t2);
        CHECK(!t2.error, "the builder's copy of an accepted document is refused:\n%s", again);
        same_events(&t, &t2, 0, "builder round trip");
        trace_free(&t2);
        free(again);
    }

    /* X4 */
    int indent = size > 0 ? data[0] % 5 : 2;
    char* err = NULL;
    char* pretty = xml_pretty((const char*)data, size, indent, &err);
    if (t.error) {
        CHECK(pretty == NULL, "xml_pretty formatted a document the reader refuses");
        CHECK(err && *err, "xml_pretty refused with no message");
    } else {
        CHECK(pretty != NULL, "xml_pretty refused a document the reader accepts: %s", err ? err : "(no message)");
        const char* pd = aether_string_data(pretty);
        Trace t3;
        read_doc(pd, strlen(pd), &t3);
        CHECK(!t3.error, "xml_pretty's output is refused:\n%s", pd);
        same_events(&t, &t3, 1, "xml_pretty");
        trace_free(&t3);
        string_release(pretty);
    }
    free(err);
    trace_free(&t);
    return 0;
}
