/* aether_lib_meta.h — symbol-catalog metadata for `--emit=lib`
 * artifacts (issue #403, MVP).
 *
 * Aether compiles to C, which inherits a flat-symbol-table linker
 * model — once a `.ae` is compiled to a `.so`/`.dylib`/`.dll`, the
 * Aether-side namespace, parameter-naming, and source-location
 * info that the source carried is gone from the consumer's
 * perspective. This header defines a small in-binary surface that
 * preserves it: every `--emit=lib` artifact exports a
 * `aether_lib_meta()` function returning a pointer to a static
 * `AetherLibMeta` struct describing the artifact's exports.
 *
 * The format is plain C structs of `const char*` and `int`. No
 * dynamic allocation, no parsing. Any FFI consumer (Python ctypes,
 * Java Panama, Ruby Fiddle, Node-API, hand-rolled `dlsym`) can
 * walk the struct directly. The CLI tool `ae lib-info <path>`
 * dlopens an artifact, calls `aether_lib_meta()`, and prints the
 * catalog in human-readable form.
 *
 * v2 (schema "1.1") fills the `closure_count` / `closures` slots the
 * v1 layout reserved: one `AetherLibClosure` record per closure
 * surface reachable from an exported function — builder/trailing-block
 * DSL entry points, closure-typed parameters, and capturing closure
 * literals in an export's body, each with its rendered signature and
 * captured-variable list. This is what lets a *downstream Aether*
 * consumer reconstruct a closure-with-context builder DSL at full
 * fidelity instead of seeing the flattened C ABI. See
 * docs/emit-lib.md → "Two kinds of consumer".
 *
 * v3 (schema "1.2") fills the `constant_count` / `constants` slots:
 * one `AetherLibConstant` record per exported module-level `const`
 * declaration (scalar/string consts — `int`, `long`, `bool`, `float`,
 * `string`), so a downstream Aether consumer's `foo.SOME_CONST`
 * resolves against a `.so` exactly as it does against source. Typed
 * const *arrays* are out of scope (the emitter skips them rather than
 * emit a half-record). See emit-lib-export-constants-ask.md.
 *
 * v4 (schema "1.3", #2297) appends `struct_count` / `structs` and
 * `source_signatures`. A struct record carries a struct an export uses
 * (or the library defines), field by field, with each field's type as
 * Aether SOURCE ("*Vec3", "fn(ptr, int) -> int", "float[3]"); a
 * `source_signatures[i]` is functions[i]'s signature in the same spelling,
 * with parameter names ("(m: *Model, dt: float) -> void"), where
 * `signature` stays the display form that flattens a typed pointer to
 * "ptr". Together they let `ae` declare, for a binary import, the same
 * structs and the same typed externs the library was built with. Either
 * slot may be absent (0 / NULL), and an entry of `source_signatures` is ""
 * when that export has no source spelling.
 *
 * v5 (schema "1.4", #2297) appends `function_modules` and
 * `constant_modules`, parallel to `functions` and `constants`: in a library
 * built from a whole package (`ae build --emit=lib --package <pkg>`), the
 * module each export belongs to ("ae3d.core"; "" for the entry's own). An
 * export of a package module is named by its bare name in `aether_name`
 * and has the C symbol `aether_<module, dots as _>__<name>`.
 *
 * v6 (schema "1.5", #2297) appends `runtime`: "shared" when the library was
 * linked against the shared runtime (`--shared-runtime`), NULL otherwise. A
 * program importing such a library links the shared runtime too, so the two
 * run on one runtime: a panic raised in the library reaches the program's
 * catch, on every platform.
 *
 * v7 (schema "1.6", #2349) appends `closure_modules`, parallel to
 * `closures`: in a package library, the module a record belongs to (""
 * for the entry's own). A package module's builder is exported as
 * `aether_<module, dots as _>__<name>`, taking its parameters and the
 * trailing `void* _builder` config.
 *
 * v8 (schema "1.7", #2297) appends `actors`: 1 when the library runs actors,
 * its own or those of a binary library it imports, else 0. A program that
 * imports such a library runs the scheduler from its main() as it does for
 * its own actors, so the library's in-flight messages are delivered before
 * the program exits. A host that does not (a C program, say) still gets a
 * running scheduler on the library's first spawn, and calls
 * scheduler_shutdown() before it exits to drain it.
 *
 * Schema versioning: `schema_version` is "1.0" for function-only
 * artifacts, "1.1" once closure records are present, "1.2" once
 * constant records are present, "1.3" once struct records or source
 * signatures are, "1.4" for a package library, "1.5" for a library
 * linked against the shared runtime, "1.6" for a package library
 * that exports builders, and "1.7" for a library that runs actors. Hosts that read the metadata should
 * accept any "1.<minor>" — within "1.x" fields are only ever appended,
 * and a reader that predates a field stops at the count/pointer it
 * knows (a "1.0" reader ignores `closures` and `constants` exactly as
 * before; the slots were always there). The "all-zero / NULL means
 * absent" contract holds for every appended field.
 */

#ifndef AETHER_LIB_META_H
#define AETHER_LIB_META_H

#ifdef __cplusplus
extern "C" {
#endif

/* One exported function. */
typedef struct {
    const char* aether_name;     /* "my_concat" or "std.fs.copy"        */
    const char* c_symbol;         /* unmangled C symbol callers dlsym    */
    const char* signature;        /* "(string, string) -> string"        */
    const char* source_file;      /* path of the .ae that defined it     */
    int         source_line;      /* 1-based                              */
} AetherLibFunction;

/* One captured variable in a closure's environment (v2). `type` is the
 * Aether-readable rendering ("int", "string", "|int| -> int"), not the
 * lowered C type — the point of these records is source-level fidelity
 * for an Aether consumer. */
typedef struct {
    const char* name;             /* captured variable name              */
    const char* type;             /* rendered Aether type                */
} AetherLibCapture;

/* One closure surface reachable from an exported function (v2).
 *
 * `role` is one of:
 *   "builder"        — the export is a builder / trailing-block DSL
 *                      entry point (takes an injected `_ctx`); call it
 *                      with a trailing block. `signature` is the
 *                      export's own signature; `capture_count` is 0.
 *   "param"          — a closure-typed parameter of the export.
 *                      `name` is the parameter name, `signature` its
 *                      `|...| -> R` shape; `capture_count` is 0
 *                      (captures belong to the value the caller passes).
 *   "trailing-block" — a trailing-block closure literal in the export's
 *                      body.
 *   "literal"        — any other closure literal in the export's body.
 * For the last two, `captures` lists the variables the closure closes
 * over, with their rendered types, and `name` is the hoisted closure
 * symbol ("" if anonymous).
 *
 * Stable layout — append only, never reorder. */
typedef struct {
    const char* name;                 /* param / hoisted-closure name, or "" */
    const char* role;                 /* see above                           */
    const char* enclosing_export;     /* aether_name it is reachable from     */
    const char* signature;            /* "|int, string| -> bool"              */
    int                       capture_count;
    const AetherLibCapture*   captures;     /* NULL when capture_count == 0   */
    const char* source_file;          /* path of the .ae that defined it      */
    int         source_line;          /* 1-based                              */
} AetherLibClosure;

/* One exported module-level scalar/string constant (v3).
 *
 * `value` is the rendered source literal, ready to drop verbatim into a
 * synthesized `const NAME = <value>` stub: an int/long/float as its
 * digits ("0", "-1", "3.14"), a bool as "true"/"false", a string as a
 * quoted, escaped Aether string literal ("\"...\""). Typed const arrays
 * (#745) are out of scope and the emitter skips them. Stable layout —
 * append only, never reorder. */
typedef struct {
    const char* name;    /* "ENTRY_NORMAL"                                  */
    const char* type;    /* "int" | "long" | "string" | "bool" | "float"   */
    const char* value;   /* rendered literal: "0", "\"...\"", "true", ...   */
} AetherLibConstant;

/* One field of a struct record (v4). `type` is Aether source; `flags` is
 * "using" for an embedded field (#1048), else "". */
typedef struct {
    const char* name;
    const char* type;
    const char* flags;
} AetherLibField;

/* One struct an importer can re-declare (v4). `kind` is "" for an Aether
 * struct, "extern" / "extern_packed" for an `extern struct` with a layout.
 * Stable layout — append only, never reorder. */
typedef struct {
    const char* name;
    const char* kind;
    int                    field_count;
    const AetherLibField*  fields;
    const char* source_file;
    int         source_line;
} AetherLibStruct;

/* Top-level catalog. Stable layout — never reorder fields, only
 * append. New optional fields go at the end with a documented
 * "all-zero means absent" contract. */
typedef struct {
    const char* schema_version;   /* "1.0" funcs; "1.1" closures; "1.2" consts; "1.3" structs; "1.4" package */
    const char* aether_version;   /* compiler version that produced this   */
    const char* primary_source;   /* the main .ae file passed to aetherc   */
    int                       function_count;
    const AetherLibFunction*  functions;
    int                       closure_count;   /* 0 if no closure surface   */
    const AetherLibClosure*   closures;        /* NULL when closure_count==0 */
    int                       constant_count;  /* 0 if no exported consts   */
    const AetherLibConstant*  constants;       /* NULL when constant_count==0 */
    int                       struct_count;    /* schema >= 1.3; 0 if none   */
    const AetherLibStruct*    structs;         /* NULL when struct_count==0  */
    const char* const*        source_signatures; /* schema >= 1.3; parallel to
                                                    functions, or NULL      */
    const char* const*        function_modules;  /* schema >= 1.4; parallel to
                                                    functions, or NULL      */
    const char* const*        constant_modules;  /* schema >= 1.4; parallel to
                                                    constants, or NULL      */
    const char*               runtime;           /* schema >= 1.5: "shared" when
                                                    the library links the
                                                    shared runtime, else NULL */
    const char* const*        closure_modules;   /* schema >= 1.6; parallel to
                                                    closures, or NULL       */
    int                       actors;            /* schema >= 1.7: 1 when the
                                                    library runs actors     */
} AetherLibMeta;

/* The single entry point. Every `--emit=lib` artifact exports this
 * symbol; consumers `dlsym` for "aether_lib_meta" and call it.
 * Returns a pointer to a static (process-lifetime) struct — never
 * free, never modify. */
const AetherLibMeta* aether_lib_meta(void);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_LIB_META_H */
