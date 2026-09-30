/* Compiled module artifacts (`.aea`, issue #1746).
 *
 * An installed module's source is parsed by every program that imports it.
 * A `.aea` holds that module already parsed, beside a header that says which
 * source, which front end and which build symbols it was parsed from. The
 * resolver still resolves an import to the module's source path exactly as
 * before; loading that path then takes the AST from a compatible artifact
 * instead of lexing and parsing the text. Everything after the parse (merge,
 * type check, codegen, diagnostics) sees the same AST, stamped with the same
 * source path, so a program compiles to the same C either way.
 *
 * This file is the format only: encode an AST, decode and validate one, and
 * map an installed source path to its artifact path. It does no file I/O and
 * reads no global state, so every rule it applies is unit-testable. The glue
 * that reads files and decides when to try an artifact lives in
 * aether_module.c (consumer) and aetherc.c (producer).
 *
 * Layout: a text header of `key value` lines, then the AST payload.
 *
 *   AEA 1
 *   aether_version 0.741.0
 *   frontend <fingerprint of the lexer/parser/AST sources>
 *   module std.cryptography.md2
 *   source std/cryptography/md2/module.ae
 *   source_hash <fnv-1a 64 of the source text, hex>
 *   define NAME 0|1                  (one per `defined(NAME)` the parse asked)
 *   payload_hash <fnv-1a 64 of the payload, hex>
 *   payload <byte length>
 *   <payload>
 */
#ifndef AETHER_AEA_H
#define AETHER_AEA_H

#include <stddef.h>
#include <stdint.h>

#include "ast.h"

#define AEA_FORMAT_VERSION 1

/* One `defined(NAME)` answer the module's parse depended on. */
typedef struct {
    const char* name;
    int value;
} AeaDefine;

/* What the producer knows about the parse it is recording. */
typedef struct {
    const char* aether_version;
    const char* frontend_id;
    const char* source_rel;    /* install-relative, `/`-separated: std/x/module.ae */
    const char* source_text;
    size_t source_len;
    const AeaDefine* defines;
    int define_count;
} AeaProducer;

/* What the consumer requires of an artifact before trusting it. */
typedef struct {
    const char* aether_version;
    const char* frontend_id;
    const char* source_rel;
    const char* source_text;
    size_t source_len;
    /* Answers `defined(NAME)` for the build doing the import. */
    int (*define_is_set)(const char* name);
} AeaConsumer;

/* FNV-1a 64-bit. Not cryptographic: the artifact tree is as trusted as the
 * compiler installed beside it; the hash only has to notice a changed file. */
uint64_t aea_hash(const char* data, size_t len);

/* The front-end fingerprint this compiler was built with. */
const char* aea_frontend_id(void);

/* Encodes `ast` (a freshly parsed module) as a complete artifact. Returns a
 * malloc'd buffer and sets *out_len, or returns NULL and points *err at a
 * static reason when the AST carries state a parse never produces (a type
 * checker back-pointer, merge flags, a node from another file) or the
 * producer description is unusable. */
char* aea_encode(const ASTNode* ast, const AeaProducer* producer,
                 size_t* out_len, const char** err);

/* Decodes and validates an artifact. Returns the module AST (source_file
 * unset: the caller stamps its own path) or NULL with *why pointing at a
 * static reason: malformed, another format, another compiler, another
 * source, a build symbol answered differently. */
ASTNode* aea_decode(const char* data, size_t len, const AeaConsumer* consumer,
                    const char** why);

/* Maps an installed source path to its artifact path. Artifacts live under
 * the same prefix as the source they were built from:
 *
 *   <prefix>/share/aether/std/cryptography/md2/module.ae
 *   <prefix>/lib/aether/modules/std/cryptography/md2.aea
 *
 * and a package's own file keeps its name:
 *
 *   <prefix>/share/aether/std/jsonpath/parser.ae
 *   <prefix>/lib/aether/modules/std/jsonpath/parser.aea
 *
 * Returns NULL for a path that is not under a `share/aether/` install root
 * (a project's own source, a development tree), which never uses artifacts.
 * On success sets *source_rel_out to a malloc'd install-relative source path
 * (`std/cryptography/md2/module.ae`), the value the artifact must record. */
char* aea_artifact_path_for(const char* source_path, char** source_rel_out);

/* `std/cryptography/md2/module.ae` -> `std.cryptography.md2`, and
 * `std/jsonpath/parser.ae` -> `std.jsonpath.parser`. malloc'd. */
char* aea_module_name_for(const char* source_rel);

#endif /* AETHER_AEA_H */
