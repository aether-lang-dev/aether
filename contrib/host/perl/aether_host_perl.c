// aether_host_perl.c — Embedded Perl Language Host Module
//
// Embeds Perl in the Aether process. Perl's open(), $ENV{},
// system() etc. go through libc and are intercepted by the
// LD_PRELOAD sandbox layer.
//
// LOADING MODEL: dlopen, not -lperl. Mirrors contrib/host/python,
// contrib/host/ruby, contrib/host/lua — every Perl C-API symbol
// resolved via dlsym at first call. The bridge .a has NO unresolved
// Perl symbols at link time, so end-user binaries have no
// DT_NEEDED libperl and are ABI-portable across deploy-host Perl
// versions (5.34 / 5.36 / 5.38 / …).
//
// The Perl bridge is the most macro-heavy of the four:
//
//   eval_pv(s, croak)       → Perl_eval_pv(my_perl, s, croak)
//   ERRSV                   → Perl_get_sv(my_perl, "@", 0)
//   SvTRUE(sv)              → Perl_sv_true(my_perl, sv)
//   SvPV_nolen(sv)          → Perl_sv_2pv_flags(my_perl, sv, NULL, SV_GMAGIC)
//
// All the macros expand to context-passing (pTHX_) wrappers around
// real Perl_* exported functions. We dlsym those Perl_* functions
// directly and pass `my_perl` (the per-interpreter context) as the
// first argument explicitly. SV_GMAGIC's value (= 2) is a stable
// public constant baked into the bridge — same shape as Ruby's
// Qnil-as-literal.
//
// What we DON'T do: dlsym SV-struct-field accessors (SvFLAGS,
// SvPVX, …). Those would bake the SV layout into the bridge .a,
// which differs across Perl versions. Calling Perl_* functions
// instead is the layout-agnostic path.

#include "aether_host_perl.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_PERL
// Perl headers pull in EXTERN.h's pTHX_ machinery; we just need the
// PerlInterpreter / SV typedefs and the FALSE/TRUE / SV_GMAGIC
// constants. Including EXTERN.h + perl.h gets us those without
// causing Perl_* macros to bind to inline definitions (we'll use
// the dlsym'd versions exclusively).
#include <EXTERN.h>
#include <perl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

// --- libperl dlopen table ---------------------------------------------------

static void* libperl_handle = NULL;

// SV_GMAGIC flag value (2 — defined in perl/sv.h). The bridge bakes
// it directly to avoid pulling in more of the SV macro machinery.
// This is part of Perl's public ABI (5.x has been stable for years
// on this point).
#define BRIDGE_SV_GMAGIC 2

static struct {
    // Lifecycle (un-prefixed, real exports).
    PerlInterpreter* (*perl_alloc)(void);
    void  (*perl_construct)(PerlInterpreter*);
    int   (*perl_parse)(PerlInterpreter*, XSINIT_t xsinit, int argc, char** argv, char** env);
    int   (*perl_run)(PerlInterpreter*);
    int   (*perl_destruct)(PerlInterpreter*);
    void  (*perl_free)(PerlInterpreter*);
    // Eval + error introspection (Perl_*-prefixed; we pass my_perl
    // as first arg explicitly instead of via pTHX_ macro).
    SV*   (*Perl_eval_pv)(PerlInterpreter*, const char*, I32 croak_on_error);
    SV*   (*Perl_get_sv)(PerlInterpreter*, const char* name, I32 flags);
    I32   (*Perl_sv_true)(PerlInterpreter*, SV*);
    char* (*Perl_sv_2pv_flags)(PerlInterpreter*, SV*, STRLEN* lp, U32 flags);
} g_pl;

static int resolve_perl_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_pl.field) = dlsym(h, sym);                        \
        if (!g_pl.field) {                                             \
            fprintf(stderr,                                            \
                "aether host_perl: libperl missing symbol %s\n", sym); \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(perl_alloc,        "perl_alloc");
    RESOLVE(perl_construct,    "perl_construct");
    RESOLVE(perl_parse,        "perl_parse");
    RESOLVE(perl_run,          "perl_run");
    RESOLVE(perl_destruct,     "perl_destruct");
    RESOLVE(perl_free,         "perl_free");
    RESOLVE(Perl_eval_pv,      "Perl_eval_pv");
    RESOLVE(Perl_get_sv,       "Perl_get_sv");
    RESOLVE(Perl_sv_true,      "Perl_sv_true");
    RESOLVE(Perl_sv_2pv_flags, "Perl_sv_2pv_flags");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libperl on first use. Strict two-step contract:
//   1. ${AETHER_PERL_SONAME} env var (orchestrator-supplied exact,
//      e.g. "libperl.so.5.36"). The orchestrator MUST probe with
//      `perl -MConfig -e 'print $Config{libperl}'` and pass that.
//   2. libperl.so (Debian-style flat symlink, when present).
// No hardcoded version-list fallback. Orchestrator owns the probe.
static int load_libperl(void) {
    if (libperl_handle) return 0;

    const char* env_soname = getenv("AETHER_PERL_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libperl.so");
    if (!h) {
        fprintf(stderr,
            "aether host_perl: cannot dlopen libperl "
            "(tried $AETHER_PERL_SONAME=%s, libperl.so).\n"
            "  Install a perl 5.x runtime on the host, or set "
            "AETHER_PERL_SONAME to the exact soname.\n"
            "  Hint: $(perl -MConfig -e 'print $Config{libperl}')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_perl_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libperl_handle = h;
    return 0;
}

// --- bridge state (unchanged from pre-dlopen) ------------------------------

static PerlInterpreter* my_perl = NULL;

static void* perl_perms_stack[64];
static int   perl_perms_depth = 0;

extern int list_size(void*);
extern void* list_get_raw(void*, int);
/* A grant list std.sandbox built holds plain C strings; one a program
 * built can hold Aether strings (a helper that keeps its `string`
 * parameter stores its own copy). This reads the bytes of either. */
extern const char* aether_string_data(const void*);

static int pattern_match(const char* pat, const char* resource) {
    if (pat && strncmp(pat, "::ffff:", 7) == 0) pat += 7;
    if (resource && strncmp(resource, "::ffff:", 7) == 0) resource += 7;
    int plen = strlen(pat);
    int rlen = strlen(resource);
    if (plen == 1 && pat[0] == '*') return 1;
    if (plen > 1 && pat[plen-1] == '*') {
        if (strncmp(pat, resource, plen-1) == 0) return 1;
    }
    if (plen > 1 && pat[0] == '*') {
        int slen = plen - 1;
        if (rlen >= slen && strcmp(resource + rlen - slen, pat + 1) == 0) return 1;
    }
    return strcmp(pat, resource) == 0;
}

static int perms_allow(void* perms, const char* category, const char* resource) {
    if (!perms) return 1;
    int n = list_size(perms);
    if (n == 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(perms, i));
        const char* pat = aether_string_data(list_get_raw(perms, i + 1));
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && pattern_match(pat, resource)) return 1;
    }
    return 0;
}

static int host_perl_checker(const char* category, const char* resource) {
    if (perl_perms_depth <= 0) return 1;
    for (int level = 0; level < perl_perms_depth; level++) {
        if (!perms_allow(perl_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int aether_perl_init(void) {
    if (my_perl) return 0;
    if (load_libperl() != 0) return -1;
    my_perl = g_pl.perl_alloc();
    if (!my_perl) return -1;
    g_pl.perl_construct(my_perl);

    char* embedding[] = { "", "-e", "0" };
    g_pl.perl_parse(my_perl, NULL, 3, embedding, NULL);
    g_pl.perl_run(my_perl);
    return 0;
}

void aether_perl_finalize(void) {
    if (my_perl) {
        g_pl.perl_destruct(my_perl);
        g_pl.perl_free(my_perl);
        my_perl = NULL;
    }
}

// Safe eval — catches exceptions and prints them.
//
// Replaces the original `eval_pv` + `SvTRUE(ERRSV)` + `SvPV_nolen(ERRSV)`
// macros with explicit Perl_* calls passing my_perl.
//   ERRSV          → Perl_get_sv(my_perl, "@", 0)  (returns $@ SV)
//   SvTRUE(sv)     → Perl_sv_true(my_perl, sv)
//   SvPV_nolen(sv) → Perl_sv_2pv_flags(my_perl, sv, NULL, SV_GMAGIC)
static int run_perl_code(const char* code) {
    (void)g_pl.Perl_eval_pv(my_perl, code, 0 /* croak_on_error=FALSE */);
    SV* errsv = g_pl.Perl_get_sv(my_perl, "@", 0);
    if (errsv && g_pl.Perl_sv_true(my_perl, errsv)) {
        char* msg = g_pl.Perl_sv_2pv_flags(my_perl, errsv, NULL, BRIDGE_SV_GMAGIC);
        fprintf(stderr, "[perl] %s\n", msg ? msg : "(no message)");
        return -1;
    }
    return 0;
}

int aether_perl_run(const char* code) {
    if (!code) return -1;
    if (aether_perl_init() != 0) return -1;
    return run_perl_code(code);
}

int aether_perl_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (aether_perl_init() != 0) return -1;

    if (perl_perms_depth >= 64) return -1;
    perl_perms_stack[perl_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_perl_checker;

    // Scrub Perl's cached %ENV — delete vars the sandbox doesn't grant.
    // Perl populates %ENV at startup before the sandbox is active.
    {
        int n = list_size(perms);
        char scrub[4096];
        int pos = snprintf(scrub, sizeof(scrub),
            "my %%keep; ");
        for (int i = 0; i < n && pos < 3900; i += 2) {
            const char* cat = aether_string_data(list_get_raw(perms, i));
            const char* pat = aether_string_data(list_get_raw(perms, i + 1));
            if (cat && strcmp(cat, "env") == 0 && pat) {
                if (strcmp(pat, "*") == 0) {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "$keep{$_}=1 for keys %%ENV; ");
                } else {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "$keep{'%s'}=1; ", pat);
                }
            }
        }
        snprintf(scrub + pos, sizeof(scrub) - pos,
            "delete $ENV{$_} for grep { !$keep{$_} } keys %%ENV;");
        run_perl_code(scrub);
    }

    int result = run_perl_code(code);

    _aether_sandbox_checker = prev;
    perl_perms_depth--;

    return result;
}

// --- Shared map for Perl ---------------------------------------------------

int aether_perl_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (aether_perl_init() != 0) return -1;

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    // Inject frozen inputs as %_aether_input.
    {
        int n = aether_shared_map_count_by_token(map_token);
        char inject[8192];
        int pos = snprintf(inject, sizeof(inject),
            "our %%_aether_input = (); our %%_aether_output = ();\n");
        for (int i = 0; i < n && pos < 7900; i++) {
            const char* k = aether_shared_map_key_at_by_token(map_token, i);
            const char* v = aether_shared_map_value_at_by_token(map_token, i);
            if (k && v) {
                pos += snprintf(inject + pos, sizeof(inject) - pos,
                    "$_aether_input{'%s'} = '%s';\n", k, v);
            }
        }
        pos += snprintf(inject + pos, sizeof(inject) - pos,
            "sub aether_map_get { return $_aether_input{$_[0]}; }\n"
            "sub aether_map_put { $_aether_output{$_[0]} = $_[1]; }\n");
        run_perl_code(inject);
    }

    if (perl_perms_depth >= 64) return -1;
    perl_perms_stack[perl_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_perl_checker;

    int result = run_perl_code(code);

    _aether_sandbox_checker = prev;
    perl_perms_depth--;

    // Note: outputs written via aether_map_put stay in Perl's
    // %_aether_output hash. Surfacing them back to the C-side shared
    // map requires XS (`AetherMap.xs`) or direct SvPV extraction —
    // tracked in docs/next-steps.md under "Shared-map native bindings
    // for Perl and Ruby".

    return result;
}

#else
#include <stdio.h>
int aether_perl_init(void) {
    fprintf(stderr, "error: contrib.host.perl not available (compile with AETHER_HAS_PERL)\n");
    return -1;
}
void aether_perl_finalize(void) {}
int aether_perl_run(const char* code) { (void)code; return aether_perl_init(); }
int aether_perl_run_sandboxed(void* perms,
    const char* code) {
  (void)perms; (void)code;
  return aether_perl_init();
}
int aether_perl_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return aether_perl_init();
}
#endif
