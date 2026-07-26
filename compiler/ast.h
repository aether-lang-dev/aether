#ifndef AST_H
#define AST_H

#include <stddef.h>

#include "parser/tokens.h"

typedef enum {
    // Program structure
    AST_PROGRAM,
    AST_MODULE_DECLARATION,
    AST_IMPORT_STATEMENT,
    AST_EXPORT_STATEMENT,
    AST_EXPORTS_LIST,        // top-of-file `exports (a, b, c)` declaration —
                             // children are AST_IDENTIFIER nodes naming the
                             // module's public API. Replaces per-function
                             // AST_EXPORT_STATEMENT for modules using the
                             // Erlang-style list form.
    AST_ACTOR_DEFINITION,
    AST_FUNCTION_DEFINITION,
    AST_FUNCTION_CLAUSE,
    AST_MAIN_FUNCTION,
    AST_STRUCT_DEFINITION,
    AST_STRUCT_FIELD,
    AST_STRUCT_FIELD_UNION,    // Compound field inside `extern struct`:
                               //   field_name: union { sub_fields... }
                               // `value` holds the field name; children are
                               // AST_STRUCT_FIELD / AST_STRUCT_FIELD_UNION /
                               // AST_STRUCT_FIELD_NESTED nodes (the union
                               // members).
    AST_STRUCT_FIELD_NESTED,   // Compound field — a nested struct inside an
                               // extern struct (typically appears inside a
                               // union). Same shape as AST_STRUCT_FIELD_UNION
                               // but emits a `struct { ... }` instead of a
                               // `union { ... }` body.
    AST_C_STRUCT_DEF,          // #891 @c_struct Name { f: type @offset, ... }
                               // A pure-Aether typed overlay over a raw ptr.
                               // `value` = struct name; children are
                               // AST_STRUCT_FIELD nodes whose `bit_width`
                               // carries the byte offset and whose node_type
                               // gives the width. Field access on an
                               // `expr as *Name` value lowers to width-correct
                               // mem_get_*/set_* at the offset (no C struct).
    AST_EXTERN_FUNCTION,      // External C function declaration
    AST_BUILDER_FUNCTION,     // Builder function: block configures first, then function executes
    AST_CONST_DECLARATION,    // Top-level constant: const NAME = value

    // Statements
    AST_BLOCK,
    AST_VARIABLE_DECLARATION,
    AST_TUPLE_DESTRUCTURE,      // a, b = func() — multiple lvalues
    AST_ASSIGNMENT,
    AST_COMPOUND_ASSIGNMENT,  // x += expr, x -= expr, etc.
    AST_IF_STATEMENT,
    AST_WHEN_STATEMENT,        // compile-time `when` / static-if (issue #483)
    AST_FOR_LOOP,
    AST_WHILE_LOOP,
    AST_SWITCH_STATEMENT,
    AST_CASE_STATEMENT,
    AST_RETURN_STATEMENT,
    AST_BREAK_STATEMENT,
    AST_CONTINUE_STATEMENT,
    AST_DEFER_STATEMENT,
    AST_EXPRESSION_STATEMENT,
    AST_MATCH_STATEMENT,
    AST_MATCH_ARM,
    AST_PATTERN_LITERAL,
    AST_PATTERN_VARIABLE,
    AST_PATTERN_STRUCT,
    AST_PATTERN_LIST,
    AST_PATTERN_CONS,
    AST_GUARD_CLAUSE,
    AST_REQUIRES_CLAUSE,       // `requires <expr>` precondition (issue #348)
    AST_ENSURES_CLAUSE,        // `ensures <expr>` postcondition  (issue #348)
    AST_RECEIVE_STATEMENT,
    AST_SEND_STATEMENT,
    AST_SPAWN_ACTOR_STATEMENT,
    AST_STATE_DECLARATION,
    AST_HIDE_DIRECTIVE,        // hide name1, name2  — block named outer bindings in this scope
    AST_SEAL_DIRECTIVE,        // seal except a, b   — block all outer bindings except whitelist
    AST_TRY_STATEMENT,         // try { body } catch e { handler } — cooperative panic recovery
    AST_CATCH_CLAUSE,          // catch name { body }  — attached as child of AST_TRY_STATEMENT
    AST_PANIC_STATEMENT,       // panic("reason") — unwinds to innermost try or actor barrier

    // Actor V2 - Message system
    AST_MESSAGE_DEFINITION,
    AST_MESSAGE_FIELD,
    AST_RECEIVE_ARM,
    AST_MESSAGE_PATTERN,
    AST_PATTERN_FIELD,
    AST_WILDCARD_PATTERN,
    AST_TIMEOUT_ARM,
    AST_REPLY_STATEMENT,
    AST_MESSAGE_CONSTRUCTOR,
    AST_FIELD_INIT,
    AST_SEND_FIRE_FORGET,
    AST_SEND_ASK,
    
    // Expressions
    AST_BINARY_EXPRESSION,
    AST_UNARY_EXPRESSION,
    // `expr!` — unwrap-or-trap on a (value, err) tuple: yields the first
    // slot, panics if the trailing error slot is non-empty. Single child:
    // the tuple-returning operand.
    AST_TUPLE_UNWRAP,
    AST_FUNCTION_CALL,
    AST_ACTOR_REF,
    AST_IDENTIFIER,
    AST_LITERAL,
    AST_ARRAY_LITERAL,
    AST_TUPLE_LITERAL,          // (a, b, ...) — parenthesized expression list.
                                // Only legal as an argument to an extern
                                // whose parameter is tuple-typed (#1033);
                                // codegen packs it into the synthesized
                                // _tuple_* struct by value.
    AST_ARRAY_ACCESS,
    AST_MEMBER_ACCESS,
    AST_STRUCT_LITERAL,
    AST_STRING_INTERP,      // interpolated string "Hello ${expr}"
    AST_NULL_LITERAL,       // null pointer literal
    AST_PTR_AS_STRUCT_CAST, // `expr as *StructName` — view a raw ptr as
                            // a pointer-to-struct. children[0] = expr
                            // (must be ptr-typed); value = struct name.
                            // Result type is TYPE_PTR with element_type
                            // = TYPE_STRUCT{name}; member-access codegen
                            // emits `->field` not `.field`.
    AST_PTR_AS_ARRAY_CAST,  // `expr as T[]` — view a raw ptr as a typed
                            // C array (element_type[]). children[0] = expr
                            // (must be ptr-typed); node_type carries
                            // TYPE_ARRAY with element_type populated and
                            // array_size = -1. Codegen emits
                            // `((T*)(expr))`; AST_ARRAY_ACCESS on the
                            // result then emits `((T*)(expr))[i]` which
                            // C scales by `sizeof(T)`. No bounds check —
                            // matches `as *StructName`'s
                            // trust-the-author posture (the same systems-
                            // programming escape hatch). Used by ports
                            // of C code that need to index a malloc'd
                            // typed buffer without the `mem.get_int(p,
                            // 4*i)` boilerplate.
    AST_PTR_AS_FN_CAST,     // `expr as fn(T1, T2, ...) -> R` — view a
                            // raw ptr as a typed C function pointer.
                            // children[0] = expr (must be ptr-typed);
                            // node_type carries the TYPE_FUNCTION with
                            // signature populated.  Codegen at the
                            // value-use site (call-expression) emits
                            // the matching C function-pointer cast
                            // before invocation.  Storage of the
                            // resulting value stays `void*`; the
                            // signature is only consulted to synthesise
                            // the cast at call sites and to typecheck
                            // arity/types of the call's arguments.
    AST_IF_EXPRESSION,      // if cond { expr } else { expr } — value-producing

    // Compile-time layout builtins over extern/struct types. Both
    // yield an int. `value` holds the struct type name; for OFFSETOF
    // children[0] is an AST_IDENTIFIER naming the field. Codegen emits
    // C's sizeof(T) / offsetof(T, field) so the value always matches
    // the real C struct layout (no hand-maintained offset constants).
    AST_SIZEOF,             // sizeof(TypeName)
    AST_OFFSETOF,           // offsetof(TypeName, fieldName)
    AST_PURITY_QUERY,       // __pure(funcName) — folds to a compile-time bool (#522)

    // heap.new(T) — zero-initialised heap allocation of a POD struct,
    // returning `*T` (issue #564). `value` holds the struct type name;
    // node_type is TYPE_PTR with element_type = TYPE_STRUCT{name}.
    // POD-only: the typechecker rejects any struct with a `string` or
    // other heap-managed field (those need an ownership model first).
    // Codegen emits `((T*)calloc(1, sizeof(T)))`. Freed with the
    // ordinary call `heap.free(p)`, codegen-lowered to `free(p)`.
    AST_HEAP_NEW,           // heap.new(TypeName)

    // C variadic-consumer intrinsics. Let an Aether function declared
    // with a trailing `...` param read its varargs the way C does.
    //   AST_VA_START — `va_start()`; yields an opaque ptr (va_list
    //     cookie). The variadic function's prologue emits the hidden
    //     `va_list` decl + `va_start(..., <last named param>)`; this
    //     node just yields its address. No children.
    //   AST_VA_ARG   — `va_arg(vap, TYPE)`; children[0] = the cookie
    //     expr; node_type = the requested type. Emits
    //     `va_arg(*(va_list*)vap, <ctype>)`.
    //   AST_VA_END   — `va_end(vap)`; children[0] = the cookie expr.
    //     Emits `va_end(*(va_list*)vap)`. Yields void.
    AST_VA_START,
    AST_VA_ARG,
    AST_VA_END,

    // Closures
    AST_CLOSURE,            // |params| -> expr  OR  |params| { block }
    AST_CLOSURE_PARAM,      // parameter in a closure: name [: type]

    // Named arguments
    AST_NAMED_ARG,          // name: expr in function call arguments

    // Types
    AST_TYPE_ANNOTATION,
    AST_ACTOR_REF_TYPE,
    AST_ARRAY_TYPE,
    
    // Special
    AST_PRINT_STATEMENT,

    // #480 distinct types. Appended at the END of the enum on purpose:
    // inserting mid-enum shifts later values and (with incremental builds)
    // leaves stale .o compiled against the old numbering.
    AST_DISTINCT_TYPE_DEF,  // `type Name = distinct Base` — `value` is Name,
                            // `node_type` is the base Type. Zero-cost: emits no
                            // C; the typechecker registers Name as a nominally
                            // distinct type over Base.
    AST_VALUE_CAST,         // `expr as T` for a scalar / distinct target — a
                            // zero-cost nominal (un)wrap or numeric conversion.
                            // children[0] = operand; node_type = target type.
    // #340 Optionals. Appended at END so existing enum values keep their
    // numbering (a mid-enum insert + incremental build leaves stale .o).
    AST_NONE_LITERAL,       // `none` — the empty optional. node_type is the
                            // concrete `T?` inferred from context.
                            // Force-unwrap `x!` reuses AST_TUPLE_UNWRAP, which
                            // is polymorphic on the operand: an optional `T?`
                            // yields T (panics on none), a `(value, err)` tuple
                            // yields the first slot.
    AST_NULL_COALESCE,      // `x ?? d` — yields T (x's value) or d if none.
                            // children[0] = optional, children[1] = default.
    AST_OPTIONAL_CHAIN,     // `x?.field` — none-propagating field access,
                            // yields `fieldT?`. value = field; children[0] = x.
    // #914 sum/variant types. `type Name = A | B | C` — a tagged union over
    // existing struct variants. value = the sum type name; each child is an
    // AST_IDENTIFIER naming a variant struct.
    AST_SUM_TYPE_DEF,
    // #913 error handler `expr or { … }` / `expr or default`. children[0] =
    // the fallible (value, err) expression; children[1] = the handler (a
    // block that yields a value or exits, or a bare default expression). `err`
    // is bound inside a block handler.
    AST_OR_ELSE,
    // #1047 match/switch case-label selectors. Appended at END to keep node
    // numbering stable (incremental builds need `make clean` after this edit).
    AST_MATCH_RANGE,        // `lo..=hi` / `lo..<hi` in a case label. children[0]
                            // = lo, children[1] = hi. `annotation` is
                            // "inclusive" (..=) or "halfopen" (..<).
    AST_MATCH_ALT,          // comma-listed alternatives in one case label
                            // (`1, 2, 5..=9`). Each child is a selector: a
                            // literal expression or an AST_MATCH_RANGE.
    // #1044 first-class enums. Appended at END to keep node numbering stable
    // (incremental builds need `make clean` after this edit).
    AST_ENUM_DEFINITION,    // `enum Name { A, B = 5, C }`. `value` = enum name;
                            // children are AST_ENUM_MEMBER nodes in source order.
    AST_ENUM_MEMBER,        // one member. `value` = member name; children[0] (if
                            // present) is the explicit value expression, else the
                            // value is previous + 1 (first defaults to 0).
    // #1046 bit_set. Appended at END to keep node numbering stable (incremental
    // builds need `make clean` after this edit).
    AST_BITSET_LITERAL,     // `bit_set[E]{ E.A, E.B }`. `node_type` is the
                            // TYPE_BITSET; children are the member expressions
                            // (each an AST_MEMBER_ACCESS `E.Member`, normalized
                            // from bare `Member` at parse time). Empty braces
                            // (`bit_set[E]{}`) yield the empty set (0 children).
    AST_BITSET_CARD,        // `card(s)`, the cardinality (popcount) of a
                            // bit_set. children[0] is the bit_set expression;
                            // lowers to `__builtin_popcountll`. Result is int.
    // #1132 bitstruct. Appended at END to keep node numbering stable (incremental
    // builds need `make clean` after this edit).
    AST_BITSTRUCT_DEFINITION, // `bitstruct Name : uint8_t { f: bool 0, g: int 1..=3 }`
                            // `value` = the bitstruct name; `node_type` is the
                            // backing integer Type (an unsigned C ABI alias).
                            // Children are AST_BITSTRUCT_FIELD nodes in source
                            // order. Unlike an extern-struct bitfield, this never
                            // emits a C bitfield: it lowers to shift/mask on the
                            // backing integer, so the layout is exact and
                            // endianness-independent.
    AST_BITSTRUCT_FIELD,    // one field. `value` = field name; `node_type` = the
                            // field's declared type (bool, or an integer type).
                            // `bit_lo` / `bit_hi` are the INCLUSIVE bit range
                            // (a single-bit field has bit_lo == bit_hi). NB the
                            // shared `bit_width` slot is deliberately NOT reused
                            // here — it already means two different things
                            // (extern-struct bit width, and @c_struct byte
                            // offset), and a third meaning would be a trap.

    // #error-unification P3: `fault NotFound, PermissionDenied, ...` — a set of
    // named error identities. `value` is unused; children are AST_IDENTIFIER
    // nodes, one per member name (in source order). Each member lowers to an
    // interned string constant whose CONTENT is its namespace-qualified name
    // (`"fs.NotFound"`), so a fault value IS a `const char*` string: it prints
    // as its name, satisfies the `e && e[0]` presence convention, and
    // `err == fs.NotFound` compares by content (string `==` is already a
    // strcmp). The qualified name is filled in at module-merge time when the
    // namespace prefix is known (bare name in the main module).
    AST_FAULT_DEFINITION,
    // #1259: top-level `@link("-lfoo -lbar")` in a module declares the
    // native libraries that module needs. Codegen unions these across the
    // resolved import closure into the `// aether-link:` header comment,
    // replacing hardcoded per-module rows in g_link_reqs. Appended at the
    // enum END: inserting mid-enum invalidates every compiled .o (see
    // the enum-insert note in the repo docs).
    AST_LINK_DIRECTIVE
} ASTNodeType;

typedef enum {
    TYPE_INT,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_UINT32,        // unsigned 32-bit — underlying kind for the
    TYPE_UINT16,        // uint32_t / uint16_t / uint8_t C ABI aliases.
    TYPE_UINT8,         // No bare keyword; reached only via c_abi_alias.
    TYPE_DURATION,      // signed 64-bit nanosecond count
    TYPE_FLOAT,
    TYPE_LONGDOUBLE,    // C `long double` — widest numeric (#749). Reached
                        // via the `longdouble` type name; no source literal.
    TYPE_FLOAT32,       // C `float` — 32-bit (#1033). Spelled `f32`; exists
                        // for extern tuple params/returns whose C struct
                        // fields are float (raylib Vector2 et al). Aether
                        // arithmetic still promotes to double.
    TYPE_BOOL,
    TYPE_BYTE,          // unsigned 8-bit (`unsigned char` in C). Type-precision
                        // for struct fields, function params, returns, locals.
                        // For bulk byte storage, use std.bytes (the mutable
                        // buffer) — `byte` is the single-octet primitive only.
    TYPE_STRING,
    TYPE_ACTOR_REF,
    TYPE_MESSAGE,
    TYPE_ARRAY,
    TYPE_STRUCT,
    TYPE_VOID,
    TYPE_PTR,           // void* for C interop
    TYPE_WILDCARD,
    TYPE_TUPLE,         // (T1, T2, ...) for multiple return values
    TYPE_FUNCTION,      // |param_types| -> return_type (closures)
    TYPE_UNKNOWN,
    TYPE_OPTIONAL,      // #340 `T?` — element_type is the wrapped type T.
                        // Lowers to a per-T tagged struct `ae_opt_<T>`
                        // `{ bool has; T val; }`. Appended at END to keep the
                        // existing kind numbering stable.
    TYPE_SUM,           // #914 sum/variant type. `struct_name` is the sum's
                        // name; `tuple_types[0..tuple_count)` are the variant
                        // Types (each TYPE_STRUCT). Lowers to a tagged union
                        // `{ <Name>_tag tag; union { ... } data; }`.
    TYPE_ISOLATED,      // #479 `Isolated[T]`, a compile-time-only, move-only
                        // (linear) wrapper for actor message payloads.
                        // element_type is the wrapped T. Nominal (an Isolated
                        // is never assignable to a bare T or vice versa);
                        // lowers to T's C type with zero runtime cost. Appended
                        // at END to keep kind numbering stable (incremental
                        // builds need `make clean` after this edit).
    TYPE_ENUM,          // #1044 first-class enum. `struct_name` = enum name.
                        // A named set of integer constants; lowers to a C
                        // `typedef enum { Name_Member = v, ... } Name;` with
                        // zero runtime cost. Nominal (compares equal only to the
                        // same-named enum; interconverts with int only via the
                        // rules in is_type_compatible). Appended at END.
    TYPE_BITSET,        // #1046 `bit_set[E]`, a set of enum members backed by an
                        // unsigned 64-bit word. element_type is the member enum
                        // (a TYPE_ENUM). Each member occupies the bit at its enum
                        // value (members must lie in 0..63). Nominal: a bit_set is
                        // never an int, and two bit_sets match only when their
                        // element enums match. Lowers to `unsigned long long` with
                        // zero runtime cost; set ops become bitwise ops. Appended
                        // at END to keep kind numbering stable (incremental builds
                        // need `make clean` after this edit).
    TYPE_BITSTRUCT      // #1132 `bitstruct Name : uint8_t { ... }`. `struct_name` =
                        // the bitstruct name; `element_type` = the backing integer
                        // type (always an unsigned fixed-width alias). Nominal: a
                        // bitstruct is never implicitly an int, and two bitstructs
                        // match only when their names match — crossing the boundary
                        // needs an explicit `as`. Lowers to the backing integer with
                        // zero runtime cost; field reads/writes become shift/mask,
                        // never a C bitfield (whose signedness and layout are
                        // implementation-defined). Appended at END to keep kind
                        // numbering stable (incremental builds need `make clean`).
} TypeKind;

typedef struct Type {
    TypeKind kind;
    struct Type* element_type; // For arrays and actor refs
    int array_size; // For fixed-size arrays
    char* struct_name; // For struct types
    // C ABI scalar alias (size_t, uint32_t, intptr_t, time_t, ...).
    // When non-NULL, codegen emits this exact C spelling instead of
    // the `kind`'s default. `kind` still governs all typechecking,
    // arithmetic, and promotion — the alias is purely the emitted
    // spelling, so a C extern prototype matches the system header.
    // NULL for every non-alias type. See redis-porting-language-gaps.md
    // "P0: C ABI Scalar Aliases".
    char* c_alias;
    // #480 distinct types: when non-NULL, this is a nominally-distinct type
    // named `distinct_name` whose machine representation is `kind` (zero cost —
    // codegen emits the base C type). Two types with different distinct names,
    // or a distinct type vs its base, are NOT compatible without an explicit
    // `as` cast, even when `kind` matches. NULL for every non-distinct type.
    char* distinct_name;
    // #1044 enum-indexed array `[E]T`: when non-NULL on a TYPE_ARRAY, the array
    // has one slot per member of enum `index_enum_name` and is indexed by an
    // `E` value (`labels[Dir.North]`), not a raw int. `element_type` is the
    // element T; `array_size` is the slot count (max member value + 1). NULL for
    // an ordinary integer-indexed array. Zero runtime cost (a plain C array).
    char* index_enum_name;
    // Tuple support (multiple return values)
    struct Type** tuple_types;  // Array of element types (NULL if not tuple)
    int tuple_count;            // Number of tuple elements (0 if not tuple)
    // Per-element heap-ownership tags for the tuple-destructure
    // heap-tracker emit (issue #420). Parallel array, length ==
    // tuple_count. Element value: 1 = the source-position is a
    // fresh heap allocation the destructured LHS now owns
    // (caller must `free` to avoid leak); 0 = borrow / non-heap /
    // unknown. Default 0 so unannotated tuple-returning externs
    // preserve the pre-#420 silent behaviour. Populated by the
    // parser's `@heap` / `@borrow` annotation handler at the
    // tuple-element position; consumed by the
    // `AST_TUPLE_DESTRUCTURE` codegen path. NULL when tuple_count
    // is 0 OR no annotation was supplied.
    int* tuple_heap_flags;
    // Function/closure type support
    struct Type** param_types;  // Parameter types (NULL if not function type)
    int param_count;            // Number of parameters (0 if not function type)
    struct Type* return_type;   // Return type (NULL if not function type)
    // 1 = this TYPE_FUNCTION represents a raw C function pointer
    // (storage = void*, call site emits typed cast).  Default 0 =
    // an _AeClosure-shaped Aether closure value (storage = _AeClosure
    // struct with .fn + .env, call site emits closure dispatch).
    // Set on cast results from `expr as fn(T1, T2, ...) -> R` and on
    // any local/param annotated with `: fn(T1, T2, ...) -> R`.
    int is_fnptr;
    // For anonymous compound types produced by `extern struct` union /
    // nested-struct fields (issue #4 — extern struct unions). Points at
    // the originating AST_STRUCT_FIELD_UNION or AST_STRUCT_FIELD_NESTED
    // node so the typechecker can resolve `expr.u.f64` by walking the
    // compound's children directly — they have no global struct name to
    // look up. NULL on all other types. Borrowed pointer (the AST owns
    // the storage), so don't free on type teardown.
    struct ASTNode* compound_node;
    // #913: a fallible result type `T!`. Represented as the existing
    // `(T, string)` (value, err) TUPLE so it is ABI-interchangeable with the
    // stdlib convention; this flag marks it as a result so `expr!` PROPAGATES
    // (rather than panics) inside a `T!`-returning function and so an `or {}`
    // handler / the "must handle" check apply. 0 on a plain tuple.
    int is_result;
} Type;

/* realloc that cannot fail. The `p = realloc(p, n)` idiom used across
 * the compiler both loses the original pointer when realloc returns
 * NULL and leaves the next statement dereferencing NULL. A compiler
 * cannot meaningfully continue past OOM, so this reports and exits
 * rather than corrupting the caller. */
void* aether_xrealloc(void* ptr, size_t size);

typedef struct ASTNode {
    ASTNodeType type;
    char* value;                // For literals, identifiers, etc.
    Type* node_type;           // Type information for this node
    struct ASTNode** children;  // Array of child nodes
    int child_count;
    int line;
    int column;
    char* annotation;          // Optional metadata (e.g., defer factory name)
    int is_imported;           // 1 if cloned in from another module by
                               // module_merge_into_program; codegen emits
                               // such functions as `static` so each TU gets
                               // a private copy and the linker doesn't see
                               // them as duplicate symbols.
    int bit_width;             // For AST_STRUCT_FIELD nodes: bit-width
                               // annotation `name: type : NN`. 0 = plain
                               // field (no bitfield). >0 = emitted as
                               // `type name : NN;` so the C compiler
                               // handles bit-extract on access. Bitfields
                               // are only meaningful on extern structs
                               // (AST_STRUCT_DEFINITION with extern flag
                               // — see annotation slot below).
                               // ALSO overloaded as the byte offset on the
                               // AST_STRUCT_FIELD children of an AST_C_STRUCT_DEF
                               // (#891). Do NOT give it a third meaning; #1132's
                               // bitstruct fields use bit_lo/bit_hi below.
    int bit_lo;                // AST_BITSTRUCT_FIELD (#1132): the INCLUSIVE low and
    int bit_hi;                // high bit indices of the field within its
                               // bitstruct's backing integer. A single-bit field
                               // (`f: bool 3`) has bit_lo == bit_hi == 3. The
                               // source may spell the range either inclusively
                               // (`1..=3`) or exclusively (`1..<4`); the parser
                               // normalises both to the inclusive pair here, so
                               // codegen never has to know which was written.
                               // Width is (bit_hi - bit_lo + 1).
    char* source_file;         // Originating .ae path (set by ast_stamp_source_file
                               // after parse). Codegen uses this to emit `#line N
                               // "path"` directives so gcc/gdb/gcov see .ae line
                               // numbers, not the merged-.c position. NULL for
                               // synthetic nodes the parser/typechecker invent
                               // out of thin air; codegen falls back to the last
                               // known file in that case.
    int type_inferred;         // AST_VARIABLE_DECLARATION only: 1 when the
                               // declaration had NO explicit type annotation
                               // (Python-style `x = expr`), so its type is
                               // inferred from the initializer. Survives the
                               // pre-typecheck inference that fills node_type,
                               // unlike a TYPE_UNKNOWN sentinel. Drives the
                               // #698 silent-narrowing guard.

    /* Allocated slots in `children`. Only add_child maintains this;
     * code that replaces the array wholesale resets it to 0, which
     * merely forces the next add_child to regrow. The invariant that
     * matters is capacity <= slots actually allocated. */
    int child_capacity;
} ASTNode;

// Type functions
Type* create_type(TypeKind kind);
Type* create_array_type(Type* element_type, int size);
Type* create_optional_type(Type* inner);   // #340 `T?`
Type* create_actor_ref_type(Type* actor_type);
Type* create_tuple_type(int count, ...);  // create_tuple_type(2, type_a, type_b)
Type* create_sum_type(const char* name);  // #914 `type Name = A | B | C`
Type* create_bitset_type(Type* element_enum);  // #1046 `bit_set[E]`
Type* create_bitstruct_type(const char* name, Type* backing);  // #1132 `bitstruct`
Type* create_result_type(Type* inner);    // #913 fallible `T!` -> (T, string)
Type* create_function_type(int param_count, Type** param_types, Type* return_type);
void free_type(Type* type);
const char* type_to_string(Type* type);
int types_equal(Type* a, Type* b);
Type* clone_type(Type* type);

/* True when `t` is a typed pointer to the cons-cell `StringSeq`
 * runtime struct (see std/collections/aether_stringseq.h) — i.e.
 * Aether-side `*StringSeq`. Used by typechecker + codegen to
 * dispatch on cons-cell-typed match expressions, literal targets,
 * and field types. Centralised here so the struct-name literal
 * lives in exactly one place. */
int is_string_seq_ptr_type(const Type* t);

/* Build a fresh `*StringSeq` Type. Caller owns and must `free_type`
 * it. */
Type* make_string_seq_ptr_type(void);

// AST Node functions
ASTNode* create_ast_node(ASTNodeType type, const char* value, int line, int column);
void add_child(ASTNode* parent, ASTNode* child);
void free_ast_node(ASTNode* node);
ASTNode* clone_ast_node(ASTNode* node);
void print_ast(ASTNode* node, int indent);
const char* ast_node_type_to_string(ASTNodeType type);

// Recursively stamp `source_file` on every node in the subtree that
// doesn't already have one. Idempotent — nodes cloned from imported
// modules already carry their original file, so re-stamping the merged
// program leaves them alone. Called once per file right after
// parse_program returns. Codegen reads node->source_file to emit
// `#line N "path"` directives so gcc/gdb/gcov see .ae line numbers.
void ast_stamp_source_file(ASTNode* node, const char* path);

// Utility functions
ASTNode* create_literal_node(Token* token);
ASTNode* create_identifier_node(Token* token);
ASTNode* create_binary_expression(ASTNode* left, ASTNode* right, Token* operator);
ASTNode* create_unary_expression(ASTNode* operand, Token* operator);

#endif
