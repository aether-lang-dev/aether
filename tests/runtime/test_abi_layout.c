/**
 * The layouts a shipped library and its host rely on, as recorded at host
 * ABI level 1, catalog schema 1.11 and runtime ABI revision 3 (#2957,
 * docs/abi-policy.md).
 *
 * Each recorded layout is a copy of the type as it stands at that level,
 * compared member by member with the type the headers define now. A change
 * fails here first, with what the policy asks for it:
 *
 * - libaether.h's structs and constants are the host ABI: changing one
 *   bumps AETHER_HOST_ABI, and the copies of the new level go here beside
 *   these.
 * - The catalog (aether_lib_meta.h) only grows within schema 1: its records
 *   never change (an importer walks their arrays with its own stride), and
 *   AetherLibMeta gains fields at its end only, each with a new minor.
 * - A rendezvous the copies of the runtime share carries its version and
 *   size: changing its layout bumps its version. The record by which the
 *   copies find each other (aether_rv_mark.h) never changes.
 * - The runtime ABI is AETHER_VERSION, AETHER_RUNTIME_ABI_REV and the flags
 *   that change layouts; the runtime reports the one it was built with.
 */

#include "test_harness.h"
#include "../../include/libaether.h"
#include "../../runtime/aether_lib_meta.h"
#include "../../runtime/aether_abi.h"
#include "../../runtime/aether_version.h"
#include "../../runtime/aether_rv_mark.h"
#include "../../runtime/aether_slice.h"
#include <stddef.h>
#include <string.h>

#define SAME_MEMBER(T, R, m) \
    do { \
        ASSERT_EQ(offsetof(R, m), offsetof(T, m)); \
        ASSERT_EQ(sizeof(((R*)0)->m), sizeof(((T*)0)->m)); \
    } while (0)

/* ---- Host ABI level 1 (libaether.h) ---- */

typedef struct { void* cell; uint64_t serial; } Token_L1;
typedef struct {
    uint64_t memory_bytes;
    int64_t  deadline_ms;
    int      deadline_counts;
    void*    parent;
} Limits_L1;

TEST_CATEGORY(abi_host_level_1_layouts, TEST_CATEGORY_RUNTIME) {
    /* These copies are level 1's: a new level records its own. */
    ASSERT_EQ(1, AETHER_HOST_ABI);

    ASSERT_EQ(sizeof(Token_L1), sizeof(AetherExecToken));
    SAME_MEMBER(AetherExecToken, Token_L1, cell);
    SAME_MEMBER(AetherExecToken, Token_L1, serial);

    ASSERT_EQ(sizeof(Limits_L1), sizeof(AetherExecLimits));
    SAME_MEMBER(AetherExecLimits, Limits_L1, memory_bytes);
    SAME_MEMBER(AetherExecLimits, Limits_L1, deadline_ms);
    SAME_MEMBER(AetherExecLimits, Limits_L1, deadline_counts);
    SAME_MEMBER(AetherExecLimits, Limits_L1, parent);

    /* An outcome's value is what a host compares with. */
    ASSERT_EQ(0, AETHER_OK);
    ASSERT_EQ(1, AETHER_PANICKED);
    ASSERT_EQ(2, AETHER_DEADLINE);
    ASSERT_EQ(3, AETHER_CANCELLED);
    ASSERT_EQ(4, AETHER_MEMORY_CAP);
    ASSERT_EQ(5, AETHER_OUT_OF_MEMORY);
    ASSERT_EQ(6, AETHER_INCOMPATIBLE);
    ASSERT_EQ(0, AETHER_DEADLINE_ELAPSED);
    ASSERT_EQ(1, AETHER_DEADLINE_ACTIVE);
}

/* ---- Catalog schema 1.11 (aether_lib_meta.h) ---- */

typedef struct {
    const char* aether_name; const char* c_symbol; const char* signature;
    const char* source_file; int source_line;
} Function_1;
typedef struct { const char* name; const char* type; } Capture_1;
typedef struct {
    const char* name; const char* role; const char* enclosing_export; const char* signature;
    int capture_count; const Capture_1* captures; const char* source_file; int source_line;
} Closure_1;
typedef struct { const char* name; const char* type; const char* value; } Constant_1;
typedef struct { const char* name; const char* type; const char* flags; } Field_1;
typedef struct {
    const char* name; const char* kind; int field_count; const Field_1* fields;
    const char* source_file; int source_line;
} Struct_1;
typedef struct {
    const char* schema_version;
    const char* aether_version;
    const char* primary_source;
    int function_count;
    const Function_1* functions;
    int closure_count;
    const Closure_1* closures;
    int constant_count;
    const Constant_1* constants;
    int struct_count;                       /* 1.3 */
    const Struct_1* structs;
    const char* const* source_signatures;
    const char* const* function_modules;    /* 1.4 */
    const char* const* constant_modules;
    const char* runtime;                    /* 1.5 */
    const char* const* closure_modules;     /* 1.6 */
    int actors;                             /* 1.7 */
    const unsigned char* closure_config_unused; /* 1.8 */
    const char* string_release;             /* 1.9 */
    const char* outcome;                    /* 1.10 */
    int host_abi;                           /* 1.11 */
    const char* runtime_abi;
    const char* requires;
} Meta_1_11;

TEST_CATEGORY(abi_catalog_schema_1_11_layouts, TEST_CATEGORY_RUNTIME) {
    /* Records never change: an importer steps through their arrays. */
    ASSERT_EQ(sizeof(Function_1), sizeof(AetherLibFunction));
    SAME_MEMBER(AetherLibFunction, Function_1, aether_name);
    SAME_MEMBER(AetherLibFunction, Function_1, c_symbol);
    SAME_MEMBER(AetherLibFunction, Function_1, signature);
    SAME_MEMBER(AetherLibFunction, Function_1, source_file);
    SAME_MEMBER(AetherLibFunction, Function_1, source_line);
    ASSERT_EQ(sizeof(Capture_1), sizeof(AetherLibCapture));
    SAME_MEMBER(AetherLibCapture, Capture_1, name);
    SAME_MEMBER(AetherLibCapture, Capture_1, type);
    ASSERT_EQ(sizeof(Closure_1), sizeof(AetherLibClosure));
    SAME_MEMBER(AetherLibClosure, Closure_1, name);
    SAME_MEMBER(AetherLibClosure, Closure_1, role);
    SAME_MEMBER(AetherLibClosure, Closure_1, enclosing_export);
    SAME_MEMBER(AetherLibClosure, Closure_1, signature);
    SAME_MEMBER(AetherLibClosure, Closure_1, capture_count);
    SAME_MEMBER(AetherLibClosure, Closure_1, captures);
    SAME_MEMBER(AetherLibClosure, Closure_1, source_file);
    SAME_MEMBER(AetherLibClosure, Closure_1, source_line);
    ASSERT_EQ(sizeof(Constant_1), sizeof(AetherLibConstant));
    SAME_MEMBER(AetherLibConstant, Constant_1, name);
    SAME_MEMBER(AetherLibConstant, Constant_1, type);
    SAME_MEMBER(AetherLibConstant, Constant_1, value);
    ASSERT_EQ(sizeof(Field_1), sizeof(AetherLibField));
    SAME_MEMBER(AetherLibField, Field_1, name);
    SAME_MEMBER(AetherLibField, Field_1, type);
    SAME_MEMBER(AetherLibField, Field_1, flags);
    ASSERT_EQ(sizeof(Struct_1), sizeof(AetherLibStruct));
    SAME_MEMBER(AetherLibStruct, Struct_1, name);
    SAME_MEMBER(AetherLibStruct, Struct_1, kind);
    SAME_MEMBER(AetherLibStruct, Struct_1, field_count);
    SAME_MEMBER(AetherLibStruct, Struct_1, fields);
    SAME_MEMBER(AetherLibStruct, Struct_1, source_file);
    SAME_MEMBER(AetherLibStruct, Struct_1, source_line);

    /* The catalog itself grows at its end only: 1.11's fields stay where
     * they are, whatever a later minor appends after them. */
    ASSERT_TRUE(sizeof(AetherLibMeta) >= sizeof(Meta_1_11));
    SAME_MEMBER(AetherLibMeta, Meta_1_11, schema_version);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, aether_version);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, primary_source);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, function_count);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, functions);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, closure_count);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, closures);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, constant_count);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, constants);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, struct_count);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, structs);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, source_signatures);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, function_modules);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, constant_modules);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, runtime);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, closure_modules);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, actors);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, closure_config_unused);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, string_release);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, outcome);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, host_abi);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, runtime_abi);
    SAME_MEMBER(AetherLibMeta, Meta_1_11, requires);
}

/* ---- The rendezvous the runtime copies of one process share ---- */

/* Sandbox levels, version 2: version and size first, as every version
 * after it keeps them. */
typedef struct {
    uint32_t version;
    uint32_t size;
    void* threads;
    int (*levels)(int delta);
} SandboxRv_2;

/* Execution accounts, version 1 as it first ships: its version, its size
 * and the sizes of what the copies pass each other, then eighteen
 * operations (version 1 grew hold, unregister_copy, exec_begin and
 * exec_end before any release: #3407, #3411, #3409). */
typedef struct {
    uint32_t version;
    uint32_t size;
    uint32_t account_size;
    uint32_t limits_size;
    void (*ops[18])(void);
} AccountsRv_1;

extern const void* aether_sandbox_rendezvous(void);
extern const void* aether_accounts_rendezvous(void);

TEST_CATEGORY(abi_rendezvous_layouts, TEST_CATEGORY_RUNTIME) {
    const uint32_t* sb = (const uint32_t*)aether_sandbox_rendezvous();
    ASSERT_NOT_NULL(sb);
    ASSERT_EQ(2, sb[0]);
    ASSERT_EQ(sizeof(SandboxRv_2), sb[1]);

    const uint32_t* ac = (const uint32_t*)aether_accounts_rendezvous();
    ASSERT_NOT_NULL(ac);
    ASSERT_EQ(1, ac[0]);
    ASSERT_EQ(sizeof(AccountsRv_1), ac[1]);
    ASSERT_EQ(sizeof(Limits_L1), ac[3]);
}

/* The record each copy carries in its image (#3445), an ELF note on every
 * format: how copies of different releases find each other, so it never
 * changes (another layout takes another name). Its desc is the function's
 * offset from the desc on ELF (then 4 zero bytes), the function's address
 * elsewhere. */
typedef struct {
    uint32_t namesz;
    uint32_t descsz;
    uint32_t kind;
    char name[12];
} RvMarkHead_1;

TEST_CATEGORY(abi_rv_mark_layout, TEST_CATEGORY_RUNTIME) {
    ASSERT_EQ(24, sizeof(RvMarkHead_1));
#if AETHER_RV_ELF
    ASSERT_EQ(24, offsetof(AetherRvMark, fn_offset));
    ASSERT_EQ(32, sizeof(AetherRvMark));   /* an 8-byte desc: 4 of offset, 4 of zeros */
#else
    ASSERT_EQ(24, offsetof(AetherRvMark, fn));
    ASSERT_EQ(24 + sizeof(void (*)(void)), sizeof(AetherRvMark));
#endif
    ASSERT_EQ(9, sizeof AETHER_RV_NAME);
    ASSERT_STREQ("AetherRV", AETHER_RV_NAME);
    ASSERT_EQ(1, AETHER_RV_KIND_SANDBOX);
    ASSERT_EQ(2, AETHER_RV_KIND_ACCOUNTS);
}

/* This runner links its runtime, and on Windows its executable exports
 * nothing: its copy is still found, by its record, the first in load order
 * (#3445: a lookup by export found no copy here, or a library's). */
TEST_CATEGORY(abi_rv_mark_finds_the_program, TEST_CATEGORY_RUNTIME) {
#if AETHER_RV_MARKS
    const void* (*sb)(void) = aether_sandbox_rendezvous;
    const void* (*ac)(void) = aether_accounts_rendezvous;
    void* want_sb;
    void* want_ac;
    memcpy(&want_sb, &sb, sizeof(want_sb));
    memcpy(&want_ac, &ac, sizeof(want_ac));
    ASSERT_TRUE(aether_rendezvous_find("aether_sandbox_rendezvous", AETHER_RV_KIND_SANDBOX, NULL) ==
                want_sb);
    ASSERT_TRUE(aether_rendezvous_find("aether_accounts_rendezvous", AETHER_RV_KIND_ACCOUNTS, NULL) ==
                want_ac);
#endif
}

/* ---- The runtime ABI ---- */

TEST_CATEGORY(abi_runtime_identity, TEST_CATEGORY_RUNTIME) {
    ASSERT_EQ(3, AETHER_RUNTIME_ABI_REV);
    /* The runtime reports what it was built with, and what generated C
     * compiled with these headers compares it with. */
    ASSERT_STREQ(AETHER_RUNTIME_ABI_ID, aether_runtime_abi());
    char want[128];
    snprintf(want, sizeof(want), "%s/abi%d%s", AETHER_VERSION, AETHER_RUNTIME_ABI_REV, AETHER_ABI_FLAGS);
    ASSERT_STREQ(want, aether_runtime_abi());
}

/* ---- Counted slices (runtime ABI revision 3, #3568) ----
 *
 * AetherSlice keeps the size and the offsets of revision 2: `off` sits in
 * the padding after `flags`, and flags' bits 8..31 carry the offset's bits
 * 32..55. A slice marked AETHER_SLICE_MADE views a counted buffer whose
 * header is AETHER_SLICE_HEAD bytes before the buffer's first element; C
 * never frees such a slice's `ptr`. */

typedef struct { void* ptr; uint64_t len; uint32_t flags; uint32_t off; } Slice_R3;
typedef struct {
    int refs;
    uint32_t reserved;
    uint64_t len;
    uint64_t elem_size;
    const void* ops;
} SliceBuf_R3;

TEST_CATEGORY(abi_slice_layouts, TEST_CATEGORY_RUNTIME) {
    ASSERT_EQ(3, AETHER_RUNTIME_ABI_REV);
    ASSERT_EQ(sizeof(Slice_R3), sizeof(AetherSlice));
    SAME_MEMBER(AetherSlice, Slice_R3, ptr);
    SAME_MEMBER(AetherSlice, Slice_R3, len);
    SAME_MEMBER(AetherSlice, Slice_R3, flags);
    SAME_MEMBER(AetherSlice, Slice_R3, off);
    ASSERT_EQ(32, AETHER_SLICE_HEAD);
    ASSERT_TRUE(sizeof(AetherSliceBuf) <= AETHER_SLICE_HEAD);
    SAME_MEMBER(AetherSliceBuf, SliceBuf_R3, refs);
    SAME_MEMBER(AetherSliceBuf, SliceBuf_R3, len);
    SAME_MEMBER(AetherSliceBuf, SliceBuf_R3, elem_size);
    SAME_MEMBER(AetherSliceBuf, SliceBuf_R3, ops);
    ASSERT_EQ(1u, AETHER_SLICE_READONLY);
    ASSERT_EQ(2u, AETHER_SLICE_BORROWED);
    ASSERT_EQ(4u, AETHER_SLICE_MADE);
    ASSERT_EQ(8u, AETHER_SLICE_GROWN);
    ASSERT_EQ(0xFFu, AETHER_SLICE_MARKS);
}

TEST_CATEGORY(abi_slice_view_finds_its_buffer, TEST_CATEGORY_RUNTIME) {
    AetherSlice s = aether_slice_alloc(10, sizeof(int), NULL);
    ASSERT_TRUE((s.flags & AETHER_SLICE_MADE) != 0);
    AetherSlice v = aether_slice_derive(s, (int*)s.ptr + 3, 4);
    ASSERT_TRUE(aether_slice_buf(v) == aether_slice_buf(s));
    aether_slice_retain(v);
    ASSERT_EQ(2, aether_slice_buf(s)->refs);
    aether_slice_release(&s);
    ASSERT_TRUE(s.ptr == NULL);
    ASSERT_EQ(1, aether_slice_buf(v)->refs);
    aether_slice_release(&v);
    /* An uncounted view takes and gives back nothing. */
    int arr[3] = {1, 2, 3};
    AetherSlice a = aether_slice_make(arr, 3);
    aether_slice_retain(a);
    aether_slice_release(&a);
    ASSERT_EQ(2, arr[1]);
}
