/* stunt.h: the embedding API of stunt, a scriptless engine that a host
 * bridge loads the way it loads a real interpreter (contrib/host/stunt).
 *
 * stunt has no language. Its "script" is one line, "<check> <expectation>":
 * the name of a check from its catalogue and what the host expects of it
 * ("succeed" or "refused"; "host" for a check only the host can judge).
 * Evaluating it runs that one check, compares the outcome with the
 * expectation and leaves one result line, e.g.
 *
 *   open-etc-passwd: refused (Permission denied) - expected refused: PASS
 *
 * The shape follows Lua's C API, as the contrib bridges use it: a handle,
 * host functions registered by name that read their arguments and push one
 * typed result, an instruction limit, a cancel, and an interrupt handler.
 * Names are st_* so they cannot collide with a bridge's own stunt_* names.
 */
#ifndef STUNT_H
#define STUNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct st_vm st_vm;

/* A host function: reads its arguments with st_arg_*, pushes at most one
 * result with st_push_*, and returns the number pushed (0 or 1), or the
 * value of st_raise_error. `vm` is the st_vm calling it. */
typedef int (*st_host_fn)(void* vm);

/* An interrupt handler, called every ST_INTERRUPT_EVERY instructions of a
 * long-running check (spin); non-zero stops the check, as a cancel does.
 * ST_INTERRUPT_HUNG_UP says the host has gone: that stops silent too, which
 * answers nothing else. */
typedef int (*st_interrupt_fn)(void* ud);
#define ST_INTERRUPT_EVERY 10000
#define ST_INTERRUPT_HUNG_UP 2

/* The instruction limit the bridge's tier 1 (stunt.run) and the stand-alone
 * stunt program set, so spin with nothing else to stop it still ends. */
#define ST_DEFAULT_BUDGET 50000000
/* How long silent waits for the host to kill it before it gives up. */
#define ST_SILENT_CAP_MS 30000
/* The stunt program run by a host ends by SIGALRM after this many seconds,
 * whatever it is doing, so a host that never reaps it cannot leave it. */
#define ST_CHILD_CAP_S 60

/* Typed values (arguments and results). */
#define ST_TNIL 0
#define ST_TINT 1
#define ST_TSTR 2

/* st_eval's results, which are also the stunt program's exit codes. */
#define ST_PASS    0   /* the outcome matched the expectation */
#define ST_FAIL    1   /* it did not, or the check could not be judged */
#define ST_USAGE   2   /* unknown check, or an expectation it cannot take */
#define ST_STOPPED 3   /* a host-checked check was stopped (limit or cancel) */

st_vm* st_new(void);
void   st_free(st_vm* vm);

/* Register `fn` under `name`. The name "*" registers a fallback, called for
 * any name with nothing registered; st_call_name says which it was. */
int    st_register(st_vm* vm, const char* name, st_host_fn fn);

/* Run one "<check> <expectation>". Returns ST_PASS..ST_STOPPED; the line is
 * st_result's until the next eval. */
int         st_eval(st_vm* vm, const char* source);
const char* st_result(st_vm* vm);

/* Stop a long-running check after `budget` instructions (<= 0 disarms). */
void   st_set_instruction_limit(st_vm* vm, int64_t budget);
/* Stop it from anywhere, another thread included. */
void   st_cancel(st_vm* vm);
void   st_set_interrupt_handler(st_vm* vm, st_interrupt_fn fn, void* ud);

/* The engine copies the environment when it is made, as Python's os.environ
 * and Perl's %ENV do; env-cached reads that copy. A host entering a sandbox
 * scrubs it: entries `keep` returns 0 for are dropped, and stay dropped;
 * env-cached counts a dropped name as refused, and one that was never there
 * as an error. A host starting the engine in a child with less than its own
 * environment names what it kept back in STUNT_WITHHELD=NAME,... */
void   st_env_scrub(st_vm* vm, int (*keep)(const char* name));

/* The catalogue. `modes` is "in-process,child", "child" or "in-process";
 * host_checked is 1 when only the host can judge the outcome. */
int         st_catalogue_size(void);
const char* st_catalogue_name(int i);
const char* st_catalogue_modes(int i);
const char* st_catalogue_doc(int i);
int         st_catalogue_host_checked(int i);

/* Inside a host function. Arguments are 1-based. */
const char* st_call_name(void* vm);
int         st_arg_count(void* vm);
int         st_arg_type(void* vm, int i);
const char* st_arg_str(void* vm, int i);
int64_t     st_arg_int(void* vm, int i);
void        st_push_str(void* vm, const char* v);
void        st_push_int(void* vm, int64_t v);
void        st_push_nil(void* vm);
int         st_raise_error(void* vm, const char* msg);

#ifdef __cplusplus
}
#endif

#endif /* STUNT_H */
