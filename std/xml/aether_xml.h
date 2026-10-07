/*
 * Aether Programming Language - std.xml
 * Copyright (c) 2025 Aether Programming Language Contributors
 * Licensed under the MIT License. See LICENSE file in the project root.
 *
 * A deliberately small XML surface (issue #627): a SAX/pull-style reader
 * and an escaping element builder. Enough for S3 / SOAP-ish / config XML.
 * NOT in scope: XSD, XPath, namespaces, DTD validation, entity
 * definitions beyond the five predefined + numeric character references.
 */
#ifndef AETHER_XML_H
#define AETHER_XML_H

#include <stddef.h>

/* ---- Pull reader -------------------------------------------------- */

/* Event kinds returned by xml_next. */
enum {
    XML_EVENT_NONE = 0,
    XML_EVENT_START_ELEMENT = 1,  /* <name attr="v" ...>  (and the open half of <name/>) */
    XML_EVENT_END_ELEMENT   = 2,  /* </name>              (and the close half of <name/>) */
    XML_EVENT_TEXT          = 3,  /* character data between tags, entity-decoded */
    XML_EVENT_EOF           = 4,  /* no more events */
    XML_EVENT_ERROR         = 5   /* malformed input; see xml_parser_error */
};

typedef struct XmlParser XmlParser;

/* Create a reader over `len` bytes of XML (binary-safe; not NUL-terminated
 * dependent). Returns NULL on allocation failure. The buffer is copied,
 * so the caller may free `data` immediately. */
XmlParser* xml_parser_new(const char* data, size_t len);
/* Build a reader from a NUL-terminated string (text bodies). */
XmlParser* xml_parser_new_str(const char* s);
void       xml_parser_free(XmlParser* p);

/* Advance to the next event and return its kind. After a START_ELEMENT or
 * END_ELEMENT, xml_event_name() is valid; after START_ELEMENT the
 * attribute accessors are valid; after TEXT, xml_event_text() is valid.
 * All returned pointers stay valid only until the next xml_next() call. */
int xml_next(XmlParser* p);

/* Element name for the current START/END event ("" otherwise). */
const char* xml_event_name(XmlParser* p);
/* Decoded character data for the current TEXT event ("" otherwise). */
const char* xml_event_text(XmlParser* p);

/* Attributes of the current START_ELEMENT (0 otherwise). Values are
 * entity-decoded. Index in [0, count). */
int         xml_event_attr_count(XmlParser* p);
const char* xml_event_attr_name(XmlParser* p, int i);
const char* xml_event_attr_value(XmlParser* p, int i);
/* Convenience: value of attribute `name` on the current start element, or
 * NULL if absent. */
const char* xml_event_attr(XmlParser* p, const char* name);
/* Like xml_event_attr but returns "" (never NULL) for an absent attribute. */
const char* xml_event_attr_str(XmlParser* p, const char* name);

/* Human-readable message after an XML_EVENT_ERROR ("" if none), ending in
 * "(line L, column C, byte N)": the 1-based line, the 1-based column in
 * bytes, and the byte offset of the error in the document. The reader stops
 * at the first well-formedness error it checks for: an end tag that does
 * not close the innermost open element (or closes none, or has no name),
 * an element still open at the end of the document, no root element or a
 * second one, text or CDATA outside the root element, attributes not
 * separated by whitespace or named twice in one tag, a raw control
 * character other than tab, LF and CR in text or an attribute value, a
 * character reference that is malformed or names a character outside
 * production [2] Char, a '&' that starts no reference, and a reference to
 * an entity other than the five predefined ones (no DTD is read, so no
 * other entity is declared). */
const char* xml_parser_error(XmlParser* p);

/* ---- Escaping builder --------------------------------------------- */

typedef struct XmlBuilder XmlBuilder;

XmlBuilder* xml_builder_new(void);
void        xml_builder_free(XmlBuilder* b);

/* Emit `<?xml version="1.0" encoding="UTF-8"?>`. */
void xml_builder_declaration(XmlBuilder* b);

/* Open a start tag `<name`. Attributes may follow via xml_builder_attr;
 * the tag is closed (`>`) automatically by the next text/start/end call. */
void xml_builder_start(XmlBuilder* b, const char* name);
/* Add `name="value"` to the currently-open start tag (value escaped). */
void xml_builder_attr(XmlBuilder* b, const char* name, const char* value);
/* Append escaped character data (closes any open start tag first). */
void xml_builder_text(XmlBuilder* b, const char* text);
/* Close element `name` (`</name>`; closes any open start tag first). */
void xml_builder_end(XmlBuilder* b, const char* name);

/* Convenience: `<name>escaped-text</name>` in one call. */
void xml_builder_element(XmlBuilder* b, const char* name, const char* text);

/* Return the accumulated document. Caller owns the returned buffer and
 * must free() it. Returns NULL on allocation failure. */
char* xml_builder_finish(XmlBuilder* b);

/* Escape the five predefined entities (& < > " ') in `s` into a freshly
 * malloc'd NUL-terminated string. Caller frees. NULL on alloc failure.
 * (The std.xml wrappers copy this into an owned Aether string via
 * string_concat — the same ownership handoff std.json uses for
 * json_stringify_raw — so the malloc'd buffer is reclaimed at scope
 * exit and the value joins Aether's refcounted string model.) */
char* xml_escape(const char* s);

#endif /* AETHER_XML_H */
