/* `ae checksec <binary>` — what a linked artifact's hardening actually is.
 *
 * The toolchain can say what it asked for; only the artifact says what it got.
 * This reads the file's own headers rather than shelling out to checksec(1),
 * readelf or otool, so the answer is the same on a developer's laptop and on a
 * minimal CI image, for all three formats the toolchain emits.
 *
 * What is read, per format:
 *
 *   ELF    e_type for PIE (ET_DYN with DF_1_PIE, or ET_DYN without an
 *          INTERP for a shared object), PT_GNU_STACK's flags for NX,
 *          PT_GNU_RELRO plus DT_BIND_NOW / DF_BIND_NOW / DF_1_NOW for full
 *          RELRO, and the dynamic and static symbol tables for the canary
 *          (__stack_chk_fail) and _FORTIFY_SOURCE (*_chk).
 *   Mach-O MH_PIE for PIE, MH_ALLOW_STACK_EXECUTION for NX, and the symbol
 *          table for the canary and fortified calls. RELRO has no Mach-O
 *          equivalent, so it reports n/a rather than a failure.
 *   PE     DllCharacteristics DYNAMIC_BASE for ASLR, NX_COMPAT for NX. A
 *          /GS canary is compiler-internal on MSVC and a __stack_chk_fail
 *          import under MinGW, so the symbol scan applies there too.
 *
 * `--require` turns the report into a gate, which is what stops a mitigation
 * from being lost in a flag change nobody notices.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define CHECKSEC_MAX_FILE (256u * 1024u * 1024u)

typedef enum { PROP_NO = 0, PROP_YES, PROP_PARTIAL, PROP_NA } PropState;

typedef struct {
    const char* format;      /* "ELF", "Mach-O", "PE" */
    PropState pie;
    PropState nx;
    PropState relro;
    PropState canary;
    PropState fortify;
    PropState stripped;
} Checksec;

static const char* prop_word(PropState s) {
    switch (s) {
        case PROP_YES:     return "yes";
        case PROP_PARTIAL: return "partial";
        case PROP_NA:      return "n/a";
        default:           return "no";
    }
}

/* Read the whole file. Binaries are read once and inspected in memory; the
 * cap keeps a mistaken argument (a core dump, a disk image) from being pulled
 * in wholesale. */
static unsigned char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || (unsigned long)size > CHECKSEC_MAX_FILE) { fclose(f); return NULL; }
    rewind(f);
    unsigned char* buf = (unsigned char*)malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    *out_len = got;
    return buf;
}

/* Little/big-endian readers. A cross-compiled artifact may not share the
 * host's byte order, and reporting the wrong answer confidently would be
 * worse than refusing. */
static uint16_t rd16(const unsigned char* p, int be) {
    return be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)((p[1] << 8) | p[0]);
}
static uint32_t rd32(const unsigned char* p, int be) {
    return be ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
              : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
}
static uint64_t rd64(const unsigned char* p, int be) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[be ? i : 7 - i] << (8 * (7 - i));
    return v;
}

/* Does the file contain this symbol name? Both the canary and the fortified
 * wrappers are undefined symbols naming libc entry points, so a substring
 * search over the string tables answers it without walking every symbol
 * table variant each format has. The names are distinctive enough that a
 * false positive would need a program that mentions them deliberately. */
static int has_name(const unsigned char* buf, size_t len, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return 0;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (buf[i] == (unsigned char)needle[0] && memcmp(buf + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

/* A fortified call is any of the __*_chk family. Rather than list them, look
 * for the suffix on a symbol-looking name, which is what _FORTIFY_SOURCE
 * emits and what nothing else does.
 *
 * __chk_fail is mingw-w64's: it fortifies in its own headers, so the bound
 * check is inlined and the only name left behind is the handler the failing
 * branch jumps to. */
static int has_fortified_call(const unsigned char* buf, size_t len) {
    static const char* known[] = {
        "__memcpy_chk", "__memset_chk", "__strcpy_chk", "__strncpy_chk",
        "__sprintf_chk", "__snprintf_chk", "__printf_chk", "__fprintf_chk",
        "__memmove_chk", "__strcat_chk", "__vsnprintf_chk", "__read_chk",
        "__chk_fail",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (has_name(buf, len, known[i])) return 1;
    }
    return 0;
}

static int inspect_elf(const unsigned char* b, size_t len, Checksec* out) {
    if (len < 64) return -1;
    int is64 = b[4] == 2;
    int be   = b[5] == 2;
    out->format = "ELF";

    uint16_t e_type = rd16(b + 16, be);
    uint64_t phoff  = is64 ? rd64(b + 32, be) : rd32(b + 28, be);
    uint16_t phentsize = rd16(b + (is64 ? 54 : 42), be);
    uint16_t phnum     = rd16(b + (is64 ? 56 : 44), be);

    int saw_relro = 0, saw_interp = 0, bind_now = 0;
    out->nx = PROP_NA;   /* no PT_GNU_STACK at all: the kernel default decides */

    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t off = phoff + (uint64_t)i * phentsize;
        if (off + phentsize > len) break;
        const unsigned char* ph = b + off;
        uint32_t p_type  = rd32(ph, be);
        uint32_t p_flags = is64 ? rd32(ph + 4, be) : rd32(ph + 24, be);

        if (p_type == 0x6474e551) {                     /* PT_GNU_STACK */
            out->nx = (p_flags & 0x1) ? PROP_NO : PROP_YES;   /* PF_X means executable */
        } else if (p_type == 0x6474e552) {              /* PT_GNU_RELRO */
            saw_relro = 1;
        } else if (p_type == 3) {                       /* PT_INTERP */
            saw_interp = 1;
        } else if (p_type == 2) {                       /* PT_DYNAMIC */
            uint64_t d_off = is64 ? rd64(ph + 8, be) : rd32(ph + 4, be);
            uint64_t d_sz  = is64 ? rd64(ph + 32, be) : rd32(ph + 16, be);
            size_t   ent   = is64 ? 16u : 8u;
            for (uint64_t d = 0; d + ent <= d_sz && d_off + d + ent <= len; d += ent) {
                const unsigned char* dyn = b + d_off + d;
                uint64_t tag = is64 ? rd64(dyn, be) : rd32(dyn, be);
                uint64_t val = is64 ? rd64(dyn + 8, be) : rd32(dyn + 4, be);
                if (tag == 0) break;                       /* DT_NULL */
                if (tag == 24) bind_now = 1;               /* DT_BIND_NOW */
                if (tag == 30 && (val & 0x08)) bind_now = 1;   /* DT_FLAGS DF_BIND_NOW */
                if (tag == 0x6ffffffb) {                   /* DT_FLAGS_1 */
                    if (val & 0x00000001) bind_now = 1;    /* DF_1_NOW */
                    if (val & 0x08000000) out->pie = PROP_YES;  /* DF_1_PIE */
                }
            }
        }
    }

    /* ET_DYN with an interpreter is a PIE executable; without one it is a
     * shared library, which is position-independent by construction. */
    if (out->pie != PROP_YES) {
        out->pie = (e_type == 3 /* ET_DYN */) ? PROP_YES : PROP_NO;
        if (e_type == 3 && !saw_interp) out->pie = PROP_YES;
    }
    out->relro = saw_relro ? (bind_now ? PROP_YES : PROP_PARTIAL) : PROP_NO;
    out->canary  = has_name(b, len, "__stack_chk_fail") ? PROP_YES : PROP_NO;
    out->fortify = has_fortified_call(b, len) ? PROP_YES : PROP_NO;
    /* .symtab is dropped by strip; .dynsym survives, so the presence of the
     * section-header string ".symtab" is the readable signal. */
    out->stripped = has_name(b, len, ".symtab") ? PROP_NO : PROP_YES;
    return 0;
}

static int inspect_macho(const unsigned char* b, size_t len, Checksec* out) {
    if (len < 32) return -1;
    uint32_t magic = rd32(b, 0);
    int be = 0;
    int is64 = (magic == 0xfeedfacf || magic == 0xcffaedfe);
    if (magic == 0xcefaedfe || magic == 0xcffaedfe) be = 1;
    out->format = "Mach-O";

    uint32_t flags = rd32(b + 24, be);
    out->pie = (flags & 0x00200000) ? PROP_YES : PROP_NO;          /* MH_PIE */
    out->nx  = (flags & 0x00020000) ? PROP_NO : PROP_YES;          /* MH_ALLOW_STACK_EXECUTION */
    out->relro = PROP_NA;   /* no Mach-O equivalent; the dyld info is read-only already */
    out->canary  = has_name(b, len, "___stack_chk_fail") ||
                   has_name(b, len, "__stack_chk_fail") ? PROP_YES : PROP_NO;
    out->fortify = has_fortified_call(b, len) ||
                   has_name(b, len, "_memcpy_chk") ? PROP_YES : PROP_NO;
    out->stripped = has_name(b, len, "_main") ? PROP_NO : PROP_YES;
    (void)is64;
    return 0;
}

static int inspect_pe(const unsigned char* b, size_t len, Checksec* out) {
    if (len < 0x40) return -1;
    uint32_t pe_off = rd32(b + 0x3c, 0);
    if ((size_t)pe_off + 0x18 > len) return -1;
    if (memcmp(b + pe_off, "PE\0\0", 4) != 0) return -1;
    out->format = "PE";

    /* DllCharacteristics sits at optional-header offset 0x46 in both PE32 and
     * PE32+: the two layouts diverge only around ImageBase (4 bytes vs 8, with
     * PE32's BaseOfData making up the difference) and realign from
     * SectionAlignment onward. The magic is still read, to reject an optional
     * header that is neither. */
    uint16_t opt_magic = rd16(b + pe_off + 0x18, 0);
    if (opt_magic != 0x10b && opt_magic != 0x20b) return -1;
    size_t dllchar_off = pe_off + 0x18 + 0x46;
    if (dllchar_off + 2 > len) return -1;
    uint16_t dllchar = rd16(b + dllchar_off, 0);

    out->pie   = (dllchar & 0x0040) ? PROP_YES : PROP_NO;   /* DYNAMIC_BASE (ASLR) */
    out->nx    = (dllchar & 0x0100) ? PROP_YES : PROP_NO;   /* NX_COMPAT */
    out->relro = PROP_NA;                                   /* no PE equivalent */
    out->canary  = has_name(b, len, "__stack_chk_fail") ||
                   has_name(b, len, "__security_cookie") ? PROP_YES : PROP_NO;

    /* Both of the above are read from names, and a PE need not carry any: the
     * COFF symbol table is optional and some linkers emit none at all. With no
     * names to read, "no" would be a guess dressed as a finding, so say so
     * instead. NumberOfSymbols lives at offset 12 of the 20-byte COFF header,
     * which starts right after the 4-byte PE signature. */
    uint32_t num_symbols = (pe_off + 4 + 16 <= len) ? rd32(b + pe_off + 4 + 12, 0) : 0;

    out->fortify = has_fortified_call(b, len) ? PROP_YES
                 : num_symbols == 0           ? PROP_NA
                                              : PROP_NO;
    if (out->canary == PROP_NO && num_symbols == 0) out->canary = PROP_NA;
    out->stripped = has_name(b, len, ".debug_info") ? PROP_NO : PROP_YES;
    return 0;
}

/* Distinguishes "could not read that file" from "read it, and it is not a
 * binary this understands". Reporting both as the second sent a Windows CI run
 * hunting a format bug when the path simply had no .exe on the end. */
#define CHECKSEC_UNREADABLE (-2)

static int inspect(const char* path, Checksec* out) {
    size_t len = 0;
    unsigned char* b = read_file(path, &len);
    if (!b) return CHECKSEC_UNREADABLE;
    memset(out, 0, sizeof(*out));

    int rc = -1;
    if (len >= 4 && memcmp(b, "\x7f" "ELF", 4) == 0) {
        rc = inspect_elf(b, len, out);
    } else if (len >= 4 && (memcmp(b, "\xcf\xfa\xed\xfe", 4) == 0 ||
                            memcmp(b, "\xce\xfa\xed\xfe", 4) == 0 ||
                            memcmp(b, "\xfe\xed\xfa\xcf", 4) == 0 ||
                            memcmp(b, "\xfe\xed\xfa\xce", 4) == 0)) {
        rc = inspect_macho(b, len, out);
    } else if (len >= 2 && b[0] == 'M' && b[1] == 'Z') {
        rc = inspect_pe(b, len, out);
    }
    free(b);
    return rc;
}

/* What to say when a property is missing, so the report is actionable rather
 * than a scoreboard. */
static const char* remedy(const char* prop, PropState s) {
    if (s == PROP_YES || s == PROP_NA) return "";
    if (strcmp(prop, "relro") == 0)   return "full needs -Wl,-z,relro,-z,now";
    if (strcmp(prop, "canary") == 0)  return "needs -fstack-protector-strong";
    if (strcmp(prop, "fortify") == 0) return "needs -D_FORTIFY_SOURCE=2 with -O1 or higher";
    if (strcmp(prop, "pie") == 0)     return "needs -fPIE -pie";
    if (strcmp(prop, "nx") == 0)      return "needs a non-executable stack (-Wl,-z,noexecstack)";
    return "";
}

static PropState prop_by_name(const Checksec* c, const char* name) {
    if (strcmp(name, "pie") == 0)     return c->pie;
    if (strcmp(name, "nx") == 0)      return c->nx;
    if (strcmp(name, "relro") == 0)   return c->relro;
    if (strcmp(name, "canary") == 0)  return c->canary;
    if (strcmp(name, "fortify") == 0) return c->fortify;
    if (strcmp(name, "stripped") == 0) return c->stripped;
    return PROP_NA;
}

static void print_row(const char* label, const char* key, PropState s) {
    const char* fix = remedy(key, s);
    if (fix[0]) printf("  %-12s %-8s (%s)\n", label, prop_word(s), fix);
    else        printf("  %-12s %s\n", label, prop_word(s));
}

int cmd_checksec(int argc, char** argv) {
    const char* path = NULL;
    const char* require = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ae checksec <binary> [--require pie,nx,relro-full,canary,fortify]\n"
                   "\n"
                   "Reports the hardening a linked artifact actually carries, read from its\n"
                   "own headers: ELF, Mach-O and PE. With --require, exits non-zero when a\n"
                   "listed property is missing, which is what keeps a mitigation from being\n"
                   "lost silently.\n"
                   "\n"
                   "Properties: pie, nx, relro (relro-full requires BIND_NOW), canary,\n"
                   "fortify, stripped. A property the artifact's format cannot express\n"
                   "reports n/a and satisfies --require, so one gate works everywhere.\n");
            return 0;
        }
        if (strcmp(argv[i], "--require") == 0 && i + 1 < argc) {
            require = argv[++i];
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "Usage: ae checksec <binary> [--require ...]\n");
        return 2;
    }

    Checksec c;
    int rc = inspect(path, &c);
    if (rc == CHECKSEC_UNREADABLE) {
        fprintf(stderr, "ae checksec: cannot read '%s': %s\n", path, strerror(errno));
        return 2;
    }
    if (rc != 0) {
        fprintf(stderr, "ae checksec: '%s' is not an ELF, Mach-O or PE binary\n", path);
        return 2;
    }

    printf("%s (%s)\n", path, c.format);
    print_row("PIE", "pie", c.pie);
    print_row("NX", "nx", c.nx);
    print_row("RELRO", "relro", c.relro);
    print_row("canary", "canary", c.canary);
    print_row("FORTIFY", "fortify", c.fortify);
    print_row("stripped", "stripped", c.stripped);

    if (!require) return 0;

    int failed = 0;
    char list[512];
    snprintf(list, sizeof(list), "%s", require);
    for (char* tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        int want_full = 0;
        char name[64];
        snprintf(name, sizeof(name), "%s", tok);
        char* dash = strstr(name, "-full");
        if (dash) { *dash = '\0'; want_full = 1; }

        PropState s = prop_by_name(&c, name);
        /* n/a passes: the format cannot express the property, so demanding it
         * would make one gate unwritable across formats, and the report says
         * n/a in plain sight either way. `-full` is about the distinction the
         * property itself has (RELRO with BIND_NOW versus without), so partial
         * satisfies the plain form and only the full one. */
        int ok = (s == PROP_NA) ? 1
               : want_full     ? (s == PROP_YES)
                               : (s == PROP_YES || s == PROP_PARTIAL);
        if (!ok) {
            printf("  %-12s %-8s FAIL (required%s)\n", name, prop_word(s),
                   want_full ? ": full" : "");
            failed = 1;
        }
    }
    if (failed) {
        printf("\nae checksec: required hardening is missing from %s\n", path);
        return 1;
    }
    return 0;
}
