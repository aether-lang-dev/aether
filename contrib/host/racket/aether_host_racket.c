// aether_host_racket.c — Embedded Racket / Rhombus Language Host Module
//
// Embeds the Racket CS runtime in the Aether process. Racket and Rhombus are
// the SAME VM — Rhombus is a #lang built on the Racket runtime — so one
// persistent VM backs both contrib.host.racket and contrib.host.rhombus. The
// bridge boots once, then evaluates source text and returns the captured
// stdout INTO Aether. The VM survives across calls, so set / evaluate / get
// share state (a live k-v map handed in and read back, same process).
//
// LINKING MODEL: STATIC. Unlike the lightweight hosts (which dlopen their
// runtime), Racket CS has no shared libracketcs to dlopen — its embedding API
// (racket_boot / racket_eval / ...) ships only in the static `libracketcs.a`
// from a built Racket CS tree (the upstream `cs/c/configure` explicitly
// refuses `--enable-shared`). So the bridge calls those entry points directly
// and the importer links `libracketcs.a` + the runtime's system deps with
// `-rdynamic`. Because that archive is only present on a built-Racket host,
// this bridge is experimental, NOT in the default CONTRIB_HOST_LANGS set, and
// the integration test gates on $AETHER_RACKET_LIB / $AETHER_RACKET_BOOT_DIR.
//
// VALUE MODEL: Racket CS values are Chez Scheme `ptr`s, and two facts shape
// this bridge (both verified against a built v9.2 CS):
//   1. `racket_apply` / `racket_eval` return a *list of result values* — take
//      Scar for a single-value call.
//   2. To run user source we must NOT call `racket_primitive("read")` (that
//      resolves Chez's reader, which rejects Racket ports). Instead we build
//      the datum `(string->bytes/utf-8 (with-output-to-string (lambda () (eval
//      (read (open-input-string CODE)))))) ` as a cons-tree in C, so every
//      symbol resolves to RACKET's binding inside the eval namespace. The
//      result is a UTF-8 bytevector we copy out with Sbytevector_length/data.
//
// SANDBOX CAVEAT (like host/factor): the Racket VM runs its own GC, JIT,
// signal handling and threads, which the LD_PRELOAD libc gate does not cleanly
// contain. *_run_sandboxed accepts `perms` for signature parity only; rely on
// the process-level sandbox (spawn_sandboxed) for isolation. See README.md.

#include "aether_host_racket.h"
#include "../../../runtime/aether_shared_map.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_RACKET

#include "chezscheme.h"
#include "racketcs.h"  // installed name of the embedding API header (the
                       // in-source-tree name is api.h). Declares racket_boot,
                       // racket_eval, racket_apply, racket_primitive,
                       // racket_namespace_require, racket_dynamic_require, and
                       // pulls in racketcsboot.h for racket_boot_arguments_t.
                       // $AETHER_RACKET_INCLUDE should be the built Racket's
                       // include/ dir (chezscheme.h + racketcs.h + racketcsboot.h).

// --- bridge state -----------------------------------------------------------

static int booted = 0;        // racket_boot done + namespace required
static int rhombus_ready = 0; // Rhombus #lang loaded (lazy, first use)

// A persistent Racket hash table (for the string-only k-v map) and the live
// shared-map token, kept as Racket top-level bindings so the GC keeps them.
static int kv_installed = 0;

static ptr SYM(const char* s) { return Sstring_to_symbol(s); }

// Embedded apply/eval return a LIST of result values; first value for the
// single-value calls we make. NULL-safe-ish: returns the input on a non-pair.
static ptr first_value(ptr vlist) {
    return Spairp(vlist) ? Scar(vlist) : vlist;
}

// Convenience: call a Racket procedure (looked up via racket_primitive or a
// datum) on one / two args, returning the single result value.
static ptr call1(ptr proc, ptr a) {
    return first_value(racket_apply(proc, Scons(a, Snil)));
}
static ptr call2(ptr proc, ptr a, ptr b) {
    return first_value(racket_apply(proc, Scons(a, Scons(b, Snil))));
}

// Build a UTF-8 bytevector ptr from a C string.
static ptr cstr_to_bv(const char* s) {
    size_t n = strlen(s);
    ptr bv = Smake_bytevector((iptr)n, 0);
    if (n) memcpy(Sbytevector_data(bv), s, n);
    return bv;
}

// Copy a Racket UTF-8 bytevector out to a malloc'd, NUL-terminated C string.
static char* bv_to_cstr(ptr bv) {
    if (!Sbytevectorp(bv)) { char* e = malloc(1); if (e) e[0] = '\0'; return e; }
    iptr n = Sbytevector_length(bv);
    char* out = malloc((size_t)n + 1);
    if (!out) return NULL;
    if (n) memcpy(out, Sbytevector_data(bv), (size_t)n);
    out[n] = '\0';
    return out;
}

// Build the eval datum for a snippet, capturing stdout and returning a UTF-8
// bytevector. `lang` 0 = Racket (read + eval each form), 1 = Rhombus (read the
// source as a `#lang rhombus` module via the shrubbery reader and eval it).
// Errors are caught and their message returned as the output (prefixed
// "error: ") so C always gets a bytevector, never an uncaught escape.
//
// Racket form (lang 0):
//   (string->bytes/utf-8
//    (with-output-to-string
//     (lambda ()
//      (with-handlers ([(lambda (e) #t)
//                       (lambda (e) (display (string-append "error: "
//                                    (if (exn? e) (exn-message e)
//                                        (format "~a" e)))))])
//        (define ip (open-input-string CODE))
//        (let loop () (define f (read ip))
//          (unless (eof-object? f) (eval f) (loop)))))))
//
// Rhombus form (lang 1) replaces the inner body with reading a
//   "#lang rhombus\n" ++ CODE  module via read-syntax and evaluating it.
static ptr build_eval_datum(int lang, const char* code) {
    ptr code_str = first_value(racket_apply(racket_primitive("bytes->string/utf-8"),
                                             Scons(cstr_to_bv(code), Snil)));

    // handler: (lambda (e) (display (string-append "error: " <msg>)))
    ptr msg = Scons(SYM("if"),
                Scons(Scons(SYM("exn?"), Scons(SYM("e"), Snil)),
                  Scons(Scons(SYM("exn-message"), Scons(SYM("e"), Snil)),
                    Scons(Scons(SYM("format"), Scons(Sstring_utf8("~a", -1),
                                                 Scons(SYM("e"), Snil))),
                      Snil))));
    ptr disp_err = Scons(SYM("display"),
                     Scons(Scons(SYM("string-append"),
                             Scons(Sstring_utf8("error: ", -1), Scons(msg, Snil))),
                       Snil));
    ptr handler_lam = Scons(SYM("lambda"), Scons(Scons(SYM("e"), Snil),
                          Scons(disp_err, Snil)));
    ptr always = Scons(SYM("lambda"), Scons(Scons(SYM("e"), Snil),
                     Scons(Strue, Snil)));
    ptr handler_clause = Scons(always, Scons(handler_lam, Snil));

    ptr body;
    if (lang == 1) {
        // Rhombus: build "#lang rhombus\n" ++ CODE in memory, read it as a
        // module syntax object, DECLARE it under a fresh resolved-module-path
        // (so its body runs on require — and a unique name per call avoids the
        // module cache returning a stale, already-run instance), then require
        // that name to instantiate it. No temp file. The reader/lang accept
        // parameters must be enabled (off by default for interactive eval, the
        // "#lang not enabled" error otherwise). Datum:
        //   (let ([name (make-resolved-module-path 'aether-rhombus-<seq>)])
        //     (parameterize ([read-accept-reader #t] [read-accept-lang #t])
        //       (define stx (read-syntax 'aether (open-input-string SRC)))
        //       (parameterize ([current-module-declare-name name]) (eval stx))
        //       (dynamic-require name #f)))
        static unsigned long seq = 0;
        char namebuf[64];
        snprintf(namebuf, sizeof(namebuf), "aether-rhombus-%lu", ++seq);

        ptr src = Scons(SYM("string-append"),
                    Scons(Sstring_utf8("#lang rhombus\n", -1),
                      Scons(code_str, Snil)));
        ptr ois = Scons(SYM("open-input-string"), Scons(src, Snil));
        ptr rds = Scons(SYM("read-syntax"),
                    Scons(Scons(SYM("quote"), Scons(SYM("aether"), Snil)),
                      Scons(ois, Snil)));
        ptr def_stx = Scons(SYM("define"), Scons(SYM("stx"), Scons(rds, Snil)));

        ptr mkname = Scons(SYM("make-resolved-module-path"),
                       Scons(Scons(SYM("quote"), Scons(SYM(namebuf), Snil)), Snil));
        ptr def_name = Scons(SYM("define"), Scons(SYM("name"), Scons(mkname, Snil)));

        ptr cmdn = Scons(Scons(SYM("current-module-declare-name"),
                           Scons(SYM("name"), Snil)), Snil);
        ptr ev_stx = Scons(SYM("eval"), Scons(SYM("stx"), Snil));
        ptr decl = Scons(SYM("parameterize"), Scons(cmdn, Scons(ev_stx, Snil)));

        ptr dynreq = Scons(SYM("dynamic-require"),
                       Scons(SYM("name"), Scons(Sfalse, Snil)));

        ptr rp = Scons(Scons(SYM("read-accept-reader"), Scons(Strue, Snil)),
                   Scons(Scons(SYM("read-accept-lang"), Scons(Strue, Snil)), Snil));
        ptr inner = Scons(SYM("parameterize"),
                      Scons(rp, Scons(def_stx, Scons(decl, Scons(dynreq, Snil)))));
        body = Scons(SYM("let"), Scons(Snil, Scons(def_name, Scons(inner, Snil))));
    } else {
        // Read ALL top-level forms from the source and eval them as one
        // `begin`, so multi-form snippets and definitions both work without an
        // internal define-in-expression-context problem:
        //   (eval (cons 'begin
        //               (port->list read (open-input-string CODE))))
        // port->list (racket/port, required at boot) applies `read` until EOF.
        ptr ois2 = Scons(SYM("open-input-string"), Scons(code_str, Snil));
        ptr p2l  = Scons(SYM("port->list"), Scons(SYM("read"), Scons(ois2, Snil)));
        ptr qbeg = Scons(SYM("quote"), Scons(SYM("begin"), Snil));
        ptr forms = Scons(SYM("cons"), Scons(qbeg, Scons(p2l, Snil)));
        body = Scons(SYM("eval"), Scons(forms, Snil));
    }

    ptr guarded = Scons(SYM("with-handlers"),
                    Scons(Scons(handler_clause, Snil), Scons(body, Snil)));
    ptr thunk = Scons(SYM("lambda"), Scons(Snil, Scons(guarded, Snil)));
    ptr wts = Scons(SYM("with-output-to-string"), Scons(thunk, Snil));
    return Scons(SYM("string->bytes/utf-8"), Scons(wts, Snil));
}

// Boot the VM. Idempotent. Reads boot images from $AETHER_RACKET_BOOT_DIR.
int racket_host_init(void) {
    if (booted) return 0;

    const char* boot_dir = getenv("AETHER_RACKET_BOOT_DIR");
    if (!boot_dir || !*boot_dir) {
        fprintf(stderr,
            "aether host_racket: AETHER_RACKET_BOOT_DIR unset; need the dir "
            "holding petite.boot / scheme.boot / racket.boot from a built "
            "Racket CS.\n");
        return -1;
    }
    // Join boot_dir + each image into static buffers (live only across boot).
    static char b1[1024], b2[1024], b3[1024];
    snprintf(b1, sizeof(b1), "%s/petite.boot", boot_dir);
    snprintf(b2, sizeof(b2), "%s/scheme.boot", boot_dir);
    snprintf(b3, sizeof(b3), "%s/racket.boot", boot_dir);

    racket_boot_arguments_t ba;
    memset(&ba, 0, sizeof(ba));
    ba.boot1_path   = b1;
    ba.boot2_path   = b2;
    ba.boot3_path   = b3;
    ba.exec_file    = "aether";
    ba.collects_dir = getenv("AETHER_RACKET_COLLECTS"); // NULL/"" disables
    ba.config_dir   = getenv("AETHER_RACKET_CONFIG");   // NULL => "etc"
    ba.exit_after   = 0;   // embedded mode: do NOT exit after booting
    racket_boot(&ba);

    racket_namespace_require(SYM("racket"));
    booted = 1;
    return 0;
}

void racket_host_finalize(void) {
    // The embedding API has no clean in-process teardown that allows a
    // re-boot; leave the VM to process exit (matches host/factor, host/tcl).
}

// Lazily load the Rhombus #lang on first rhombus use (large require).
static int ensure_rhombus(void) {
    if (rhombus_ready) return 0;
    // (dynamic-require 'rhombus #f) — load the language, import no names.
    racket_dynamic_require(SYM("rhombus"), Sfalse);
    rhombus_ready = 1;
    return 0;
}

static void ensure_kv(void);  // defined below; bound before every eval

// Core eval: returns captured stdout as malloc'd C string, NULL on hard fail.
static char* eval_raw(int lang, const char* code) {
    if (!code) return NULL;
    if (racket_host_init() != 0) return NULL;
    // Bind the shared k-v hash before any eval so guest code can read/write it
    // (aether-host-kv) — e.g. a guest loop that populates keys the host reads
    // back, the way the integration tests exercise it. ensure_kv is idempotent.
    ensure_kv();
    if (lang == 1 && ensure_rhombus() != 0) return NULL;
    ptr datum = build_eval_datum(lang, code);
    ptr bv = first_value(racket_eval(datum));
    return bv_to_cstr(bv);
}

AetherString* racket_host_eval(int lang, const char* code) {
    char* r = eval_raw(lang, code);
    if (!r) return string_new("");
    AetherString* out = string_new(r);
    free(r);
    return out;
}

int racket_host_run(int lang, const char* code) {
    char* r = eval_raw(lang, code);
    if (!r) return -1;
    free(r);
    return 0;
}

int racket_host_run_sandboxed(int lang, void* perms, const char* code) {
    (void)perms;  // see SANDBOX CAVEAT; process-level isolation only
    return racket_host_run(lang, code);
}

// --- string-only k-v map over a persistent Racket hash ----------------------
//
// A single Racket hash table bound at the top level (aether-host-kv). set/get
// poke it via datums. Numbers/strings round-trip as their text, matching the
// other hosts (string-only channel).

// Define the persistent kv hash as a top-level binding once, via eval of
// `(define aether-host-kv (make-hash))`. Reaching it later is then just a
// reference to `aether-host-kv` inside an eval'd datum, which resolves in the
// same namespace.
static void ensure_kv(void) {
    if (kv_installed) return;
    ptr def = Scons(SYM("define"), Scons(SYM("aether-host-kv"),
                  Scons(Scons(SYM("make-hash"), Snil), Snil)));
    racket_eval(def);
    kv_installed = 1;
}

// A Racket string literal from a C string, decoded as UTF-8 (so non-ASCII
// keys/values survive): (bytes->string/utf-8 #"...").
static ptr cstr_to_racket_string(const char* s) {
    return call1(racket_primitive("bytes->string/utf-8"), cstr_to_bv(s));
}

int racket_host_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    if (racket_host_init() != 0) return -1;
    ensure_kv();
    // (hash-set! aether-host-kv KEY VALUE) — KEY/VALUE embedded as string vals
    ptr datum = Scons(SYM("hash-set!"),
                  Scons(SYM("aether-host-kv"),
                    Scons(cstr_to_racket_string(key),
                      Scons(cstr_to_racket_string(value), Snil))));
    racket_eval(datum);
    return 0;
}

AetherString* racket_host_get(const char* key) {
    if (!key) return string_new("");
    if (racket_host_init() != 0) return string_new("");
    ensure_kv();
    // (string->bytes/utf-8 (hash-ref aether-host-kv KEY ""))
    ptr href = Scons(SYM("hash-ref"),
                 Scons(SYM("aether-host-kv"),
                   Scons(cstr_to_racket_string(key),
                     Scons(Sstring_utf8("", -1), Snil))));
    ptr s2b = Scons(SYM("string->bytes/utf-8"), Scons(href, Snil));
    ptr bv = first_value(racket_eval(s2b));
    char* c = bv_to_cstr(bv);
    if (!c) return string_new("");
    AetherString* out = string_new(c);
    free(c);
    return out;
}

// --- First-class shared map (run_with_map), matching the other hosts -------
//
// The guest reads/writes the Aether-owned shared map live via two procedures
// the bridge binds into the namespace before the eval: aether-map-get and
// aether-map-put. We bind them as Racket closures wrapping the two C callbacks
// through ffi/unsafe, passing the C addresses as exact integers. Aether reads
// the whole map back afterwards (including runtime-discovered keys) via
// map.keys(). Single-threaded: one token live at a time.

static uint64_t g_current_map_token = 0;
static int map_procs_installed = 0;

static const char* host_map_get(const char* key) {
    if (!g_current_map_token || !key) return NULL;
    return aether_shared_map_get_by_token(g_current_map_token, key);
}
static void host_map_put(const char* key, const char* value) {
    if (!g_current_map_token || !key || !value) return;
    aether_shared_map_put_by_token(g_current_map_token, key, value);
}

// Bind aether-map-get / aether-map-put once, by evaluating a datum that
// requires ffi/unsafe and casts the two integer addresses to _fpointer then to
// the right _fun type. Built as a datum so symbols resolve in Racket.
static int install_map_procs(void) {
    if (map_procs_installed) return 0;
    if (racket_host_init() != 0) return -1;
    racket_namespace_require(SYM("ffi/unsafe"));

    intptr_t g = (intptr_t)&host_map_get;
    intptr_t p = (intptr_t)&host_map_put;

    // (define aether-map-get
    //   (cast (cast G _intptr _pointer) _fpointer (_fun _string -> _string)))
    ptr get_ptr = Scons(SYM("cast"),
        Scons(Scons(SYM("cast"), Scons(Sinteger((iptr)g),
                Scons(SYM("_intptr"), Scons(SYM("_pointer"), Snil)))),
          Scons(SYM("_fpointer"),
            Scons(Scons(SYM("_fun"), Scons(SYM("_string"),
                    Scons(SYM("->"), Scons(SYM("_string"), Snil)))),
              Snil))));
    ptr def_get = Scons(SYM("define"), Scons(SYM("aether-map-get"),
                     Scons(get_ptr, Snil)));

    ptr put_ptr = Scons(SYM("cast"),
        Scons(Scons(SYM("cast"), Scons(Sinteger((iptr)p),
                Scons(SYM("_intptr"), Scons(SYM("_pointer"), Snil)))),
          Scons(SYM("_fpointer"),
            Scons(Scons(SYM("_fun"), Scons(SYM("_string"),
                    Scons(SYM("_string"), Scons(SYM("->"),
                      Scons(SYM("_void"), Snil))))),
              Snil))));
    ptr def_put = Scons(SYM("define"), Scons(SYM("aether-map-put"),
                     Scons(put_ptr, Snil)));

    racket_eval(def_get);
    racket_eval(def_put);
    map_procs_installed = 1;
    return 0;
}

int racket_host_run_sandboxed_with_map(int lang, void* perms, const char* code,
                                       uint64_t map_token) {
    (void)perms;  // see SANDBOX CAVEAT; process-level isolation only
    if (!code) return -1;
    if (racket_host_init() != 0) return -1;

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    if (install_map_procs() != 0) return -1;
    g_current_map_token = map_token;
    int rc = racket_host_run(lang, code);
    g_current_map_token = 0;
    return rc;
}

#else /* !AETHER_HAS_RACKET */

int racket_host_init(void) {
    fprintf(stderr, "aether host_racket: built without Racket support "
                    "(needs a built Racket CS libracketcs.a; compile with "
                    "AETHER_HAS_RACKET)\n");
    return -1;
}
void racket_host_finalize(void) {}
AetherString* racket_host_eval(int lang, const char* code) {
    (void)lang; (void)code; (void)racket_host_init(); return string_new("");
}
int racket_host_run(int lang, const char* code) {
    (void)lang; (void)code; return racket_host_init();
}
int racket_host_run_sandboxed(int lang, void* perms, const char* code) {
    (void)lang; (void)perms; (void)code; return racket_host_init();
}
int racket_host_run_sandboxed_with_map(int lang, void* perms, const char* code,
                                       uint64_t map_token) {
    (void)lang; (void)perms; (void)code; (void)map_token;
    return racket_host_init();
}
int racket_host_set(const char* key, const char* value) {
    (void)key; (void)value; return racket_host_init();
}
AetherString* racket_host_get(const char* key) {
    (void)key; (void)racket_host_init(); return string_new("");
}

#endif /* AETHER_HAS_RACKET */

// --- per-language thin wrappers (defined in BOTH build modes) ---------------
// Fixed-arity symbols the Aether module surfaces `extern`. The result-returning
// call is `evaluate` (NOT `eval`): libracketcs.a exports its own `racket_eval`,
// so a C symbol named `racket_eval` here would be a duplicate at static-link.
// `racket_evaluate` sidesteps that. lang 0 = Racket s-exprs, lang 1 = Rhombus.

AetherString* racket_evaluate(const char* code) { return racket_host_eval(0, code); }
int racket_run(const char* code)                 { return racket_host_run(0, code); }
int racket_run_sandboxed(void* p, const char* c) {
    return racket_host_run_sandboxed(0, p, c);
}
int racket_run_sandboxed_with_map(void* p, const char* c, uint64_t t) {
    return racket_host_run_sandboxed_with_map(0, p, c, t);
}

AetherString* rhombus_evaluate(const char* code) { return racket_host_eval(1, code); }
int rhombus_run(const char* code)                 { return racket_host_run(1, code); }
int rhombus_run_sandboxed(void* p, const char* c) {
    return racket_host_run_sandboxed(1, p, c);
}
int rhombus_run_sandboxed_with_map(void* p, const char* c, uint64_t t) {
    return racket_host_run_sandboxed_with_map(1, p, c, t);
}

int           racket_set(const char* k, const char* v) { return racket_host_set(k, v); }
AetherString* racket_get(const char* k)                { return racket_host_get(k); }
int           racket_init(void)                        { return racket_host_init(); }
void          racket_finalize(void)                    { racket_host_finalize(); }

int           rhombus_set(const char* k, const char* v) { return racket_host_set(k, v); }
AetherString* rhombus_get(const char* k)                { return racket_host_get(k); }
int           rhombus_init(void)                        { return racket_host_init(); }
void          rhombus_finalize(void)                    { racket_host_finalize(); }
