// aether_host_ruby.c — Embedded Ruby Language Host Module
//
// Embeds Ruby (CRuby/MRI) in the Aether process. Ruby's File.open,
// ENV[], Kernel#system etc. go through libc and are intercepted by
// the LD_PRELOAD sandbox layer.
//
// LOADING MODEL: dlopen, not -lruby. Mirrors contrib/host/python's
// v0.209.0 design — every Ruby C-API symbol resolved via dlsym at
// first call. The bridge .a has NO unresolved Ruby symbols at link
// time, so end-user binaries have no DT_NEEDED libruby and are
// ABI-portable across deploy-host Ruby minor versions.
//
// RUBY_INIT_STACK macro — Ruby's docs require capturing the
// calling thread's stack bottom for GC. The macro expands to
// `ruby_init_stack(&local)` where `local` is a stack-allocated
// VALUE. Both the function AND the &local capture must happen on
// the MAIN thread (the thread that calls ruby_init). We handle
// this by:
//   - declaring a `volatile VALUE stack_bottom` local in init,
//   - dlsym'ing `ruby_init_stack` as a regular function pointer,
//   - calling it with `&stack_bottom` before the first ruby_init().
// This is the same effect as `RUBY_INIT_STACK; ruby_init();` from
// the original code, just expressed without the macro.
//
// Qnil — Ruby's nil singleton VALUE is a compile-time enum constant, not an
// exported symbol, so there is nothing to dlsym. It was baked in as 0x08 with
// a note that a change would be "extremely unlikely"; Ruby 3.4 changed it to
// 0x04. The bridge then compared errinfo against the wrong value and handed
// 0x08 to rb_set_errinfo, which is no longer a special constant there, so
// Ruby dereferenced it: "[BUG] Segmentation fault at 0x10" (#1618).
//
// The loaded interpreter is asked instead: evaluating `nil` once at init
// yields whatever nil is in THAT libruby, for any version and any
// USE_FLONUM setting.

#include "aether_host_ruby.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_RUBY
#include <ruby.h>
// Ruby's subst.h `#define snprintf ruby_snprintf` substitutes its
// own snprintf so Ruby's printf %-extensions work in user format
// strings. Our scrub-script builder uses snprintf to assemble plain
// C strings (no Ruby format extensions), so we want libc snprintf
// — undef the substitution.
#undef snprintf
#include "../aep_dl.h"
#include <stdio.h>
#include <string.h>

// --- libruby dlopen table ---------------------------------------------------

static void* libruby_handle = NULL;

// nil, as the LOADED libruby spells it. Resolved in ruby_init_host by
// evaluating `nil`; see the header comment. The 0x08 fallback only applies if
// that evaluation somehow fails, and matches Ruby 3.0 through 3.3.
static VALUE bridge_qnil = (VALUE)0x08;

static struct {
    // Lifecycle.
    void (*ruby_init)(void);
    void (*ruby_init_loadpath)(void);
    void (*ruby_init_stack)(volatile VALUE* addr);
    // Windows-only: ruby_sysinit(&argc,&argv) MUST run before ruby_init on
    // Windows (sets up the ucrt/argv layer ruby_init assumes). Absent/unneeded
    // on POSIX. Soft-resolved; only called under _WIN32.
    void (*ruby_sysinit)(int* argc, char*** argv);
    void (*ruby_finalize)(void);
    // Eval + error introspection.
    VALUE (*rb_eval_string_protect)(const char*, int*);
    VALUE (*rb_errinfo)(void);
    void  (*rb_set_errinfo)(VALUE);
    // String + symbol helpers.
    //
    // NOTE: `rb_intern` and `rb_funcall` are #define'd as macros in
    // ruby/ruby.h (rb_intern → rb_intern2-ish dispatcher,
    // rb_funcall → __extension__({...}) GCC statement-expr block).
    // A bare struct field named `rb_intern` or `rb_funcall` gets
    // textually rewritten by the preprocessor and breaks the build.
    // Same trap as `Py_None` in the python bridge — field names are
    // prefixed `f_` to avoid the macro collision.
    ID    (*f_rb_intern)(const char*);
    char* (*rb_string_value_cstr)(volatile VALUE*);
    // Method dispatch — varargs; dlsym handles it.
    VALUE (*f_rb_funcall)(VALUE, ID, int, ...);
} g_rb;

static int resolve_ruby_symbols(void* h) {
#define RESOLVE(field, sym) do {                                       \
        *(void**)(&g_rb.field) = dlsym(h, sym);                        \
        if (!g_rb.field) {                                             \
            fprintf(stderr,                                            \
                "aether host_ruby: libruby missing symbol %s\n", sym); \
            return -1;                                                 \
        }                                                              \
    } while (0)

    RESOLVE(ruby_init,              "ruby_init");
    RESOLVE(ruby_init_loadpath,     "ruby_init_loadpath");
    RESOLVE(ruby_init_stack,        "ruby_init_stack");
    // soft-resolve (not RESOLVE): POSIX libruby need not export it.
    *(void**)(&g_rb.ruby_sysinit) = dlsym(h, "ruby_sysinit");
    RESOLVE(ruby_finalize,          "ruby_finalize");
    RESOLVE(rb_eval_string_protect, "rb_eval_string_protect");
    RESOLVE(rb_errinfo,             "rb_errinfo");
    RESOLVE(rb_set_errinfo,         "rb_set_errinfo");
    RESOLVE(f_rb_intern,            "rb_intern");
    RESOLVE(rb_string_value_cstr,   "rb_string_value_cstr");
    RESOLVE(f_rb_funcall,           "rb_funcall");
    return 0;
#undef RESOLVE
}

static void* try_dlopen(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

// Load libruby on first use. Strict two-step contract:
//   1. ${AETHER_RUBY_SONAME} env var (orchestrator-supplied exact).
//      Bazzite/Fedora hosts ship NO `libruby.so` symlink — the
//      orchestrator MUST probe with `ruby -rrbconfig
//      -e 'print RbConfig::CONFIG["LIBRUBY_SO"]'` (e.g.
//      "libruby.so.3.4") and pass that env var.
//   2. libruby.so (Debian-style unversioned symlink, fallback only).
// No versioned hardcoded fallback list — would be a maintenance
// treadmill across Ruby minors. Orchestrator owns the probe.
// Returns 0 on success, -1 on any failure.
static int load_libruby(void) {
    if (libruby_handle) return 0;

    const char* env_soname = getenv("AETHER_RUBY_SONAME");
    void* h = try_dlopen(env_soname);
    if (!h) h = try_dlopen("libruby.so");
    if (!h) {
        fprintf(stderr,
            "aether host_ruby: cannot dlopen libruby "
            "(tried $AETHER_RUBY_SONAME=%s, libruby.so).\n"
            "  Install a ruby3 runtime on the host, or set "
            "AETHER_RUBY_SONAME to the exact soname.\n"
            "  Hint: $(ruby -rrbconfig -e "
            "'print RbConfig::CONFIG[\"LIBRUBY_SO\"]')\n"
            "  dlerror: %s\n",
            env_soname ? env_soname : "(unset)",
            dlerror() ? dlerror() : "(none)");
        return -1;
    }

    if (resolve_ruby_symbols(h) != 0) {
        dlclose(h);
        return -1;
    }
    libruby_handle = h;
    return 0;
}

// --- sandbox / permission stack (unchanged from pre-dlopen) ----------------

static int ruby_initialized = 0;

static void* ruby_perms_stack[64];
static int   ruby_perms_depth = 0;

extern int list_size(void*);
extern void* list_get_raw(void*, int);

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
        const char* cat = (const char*)list_get_raw(perms, i);
        const char* pat = (const char*)list_get_raw(perms, i + 1);
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && pattern_match(pat, resource)) return 1;
    }
    return 0;
}

static int host_ruby_checker(const char* category, const char* resource) {
    if (ruby_perms_depth <= 0) return 1;
    for (int level = 0; level < ruby_perms_depth; level++) {
        if (!perms_allow(ruby_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int ruby_init_host(void) {
    if (ruby_initialized) return 0;
    if (load_libruby() != 0) return -1;

    // Equivalent of the RUBY_INIT_STACK macro: capture this thread's
    // stack bottom for GC. Must happen on the calling thread, before
    // ruby_init().
    volatile VALUE stack_bottom = 0;
#if defined(_WIN32)
    // Windows requires ruby_sysinit() before any other ruby_* call.
    if (g_rb.ruby_sysinit) {
        static char* aep_argv0 = "aether";
        char* aep_argv_storage[2] = { aep_argv0, (char*)0 };
        char** aep_argv = aep_argv_storage;
        int aep_argc = 1;
        g_rb.ruby_sysinit(&aep_argc, &aep_argv);
    }
#endif
    g_rb.ruby_init_stack(&stack_bottom);

    g_rb.ruby_init();
    g_rb.ruby_init_loadpath();
    ruby_initialized = 1;

    /* Ask the interpreter what nil is, now that it can answer. Must precede
     * any eval that inspects errinfo, which is why it sits here and not in
     * eval_ruby: the very first eval would otherwise compare against, and
     * assign, the wrong constant. */
    {
        int st = 0;
        VALUE v = g_rb.rb_eval_string_protect("nil", &st);
        if (!st) {
            bridge_qnil = v;
        } else {
            g_rb.rb_set_errinfo(bridge_qnil);
        }
    }
    return 0;
}

void ruby_finalize_host(void) {
    if (ruby_initialized) {
        g_rb.ruby_finalize();
        ruby_initialized = 0;
    }
}

// Safe eval — catches exceptions and prints them.
static int eval_ruby(const char* code) {
    int state = 0;
    g_rb.rb_eval_string_protect(code, &state);
    if (state) {
        VALUE err = g_rb.rb_errinfo();
        if (err != bridge_qnil) {
            VALUE msg = g_rb.f_rb_funcall(err, g_rb.f_rb_intern("message"), 0);
            // StringValueCStr(msg) macro expands to
            // rb_string_value_cstr(&msg); call the function directly.
            fprintf(stderr, "[ruby] %s\n", g_rb.rb_string_value_cstr(&msg));
        }
        g_rb.rb_set_errinfo(bridge_qnil);
        return -1;
    }
    return 0;
}

int ruby_run(const char* code) {
    if (!code) return -1;
    if (ruby_init_host() != 0) return -1;
    return eval_ruby(code);
}

int ruby_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (ruby_init_host() != 0) return -1;

    if (ruby_perms_depth >= 64) return -1;
    ruby_perms_stack[ruby_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_ruby_checker;

    // Scrub Ruby's ENV hash — delete vars the sandbox doesn't grant.
    // Ruby caches ENV at startup like Perl.
    {
        int n = list_size(perms);
        char scrub[4096];
        int pos = snprintf(scrub, sizeof(scrub),
            "_keep = {}; ");
        for (int i = 0; i < n && pos < 3900; i += 2) {
            const char* cat = (const char*)list_get_raw(perms, i);
            const char* pat = (const char*)list_get_raw(perms, i + 1);
            if (cat && strcmp(cat, "env") == 0 && pat) {
                if (strcmp(pat, "*") == 0) {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "ENV.each_key { |k| _keep[k] = true }; ");
                } else {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "_keep['%s'] = true; ", pat);
                }
            }
        }
        snprintf(scrub + pos, sizeof(scrub) - pos,
            "ENV.keys.each { |k| ENV.delete(k) unless _keep[k] }");
        eval_ruby(scrub);
    }

    int result = eval_ruby(code);

    _aether_sandbox_checker = prev;
    ruby_perms_depth--;

    return result;
}

// --- Shared map for Ruby ---------------------------------------------------

int ruby_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    if (ruby_init_host() != 0) return -1;

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    // Inject inputs as $_aether_input hash.
    {
        int n = aether_shared_map_count_by_token(map_token);
        char inject[8192];
        int pos = snprintf(inject, sizeof(inject),
            "$_aether_input = {}; $_aether_output = {};\n");
        for (int i = 0; i < n && pos < 7900; i++) {
            const char* k = aether_shared_map_key_at_by_token(map_token, i);
            const char* v = aether_shared_map_value_at_by_token(map_token, i);
            if (k && v) {
                pos += snprintf(inject + pos, sizeof(inject) - pos,
                    "$_aether_input['%s'] = '%s'\n", k, v);
            }
        }
        pos += snprintf(inject + pos, sizeof(inject) - pos,
            "def aether_map_get(key); $_aether_input[key]; end\n"
            "def aether_map_put(key, val); $_aether_output[key] = val; end\n");
        eval_ruby(inject);
    }

    // Env scrub.
    {
        int n = list_size(perms);
        char scrub[4096];
        int pos = snprintf(scrub, sizeof(scrub), "_keep = {}; ");
        for (int i = 0; i < n && pos < 3900; i += 2) {
            const char* cat = (const char*)list_get_raw(perms, i);
            const char* pat = (const char*)list_get_raw(perms, i + 1);
            if (cat && strcmp(cat, "env") == 0 && pat) {
                if (strcmp(pat, "*") == 0) {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "ENV.each_key { |k| _keep[k] = true }; ");
                } else {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "_keep['%s'] = true; ", pat);
                }
            }
        }
        snprintf(scrub + pos, sizeof(scrub) - pos,
            "ENV.keys.each { |k| ENV.delete(k) unless _keep[k] }");
        eval_ruby(scrub);
    }

    if (ruby_perms_depth >= 64) return -1;
    ruby_perms_stack[ruby_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_ruby_checker;

    int result = eval_ruby(code);

    _aether_sandbox_checker = prev;
    ruby_perms_depth--;

    return result;
}

#else
#include <stdio.h>
int ruby_init_host(void) {
    fprintf(stderr, "error: contrib.host.ruby not available (compile with AETHER_HAS_RUBY)\n");
    return -1;
}
void ruby_finalize_host(void) {}
int ruby_run(const char* code) { (void)code; return ruby_init_host(); }
int ruby_run_sandboxed(void* perms,
    const char* code) {
  (void)perms; (void)code;
  return ruby_init_host();
}
int ruby_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return ruby_init_host();
}
#endif
