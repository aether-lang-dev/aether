/* #2586: the C side of fnptr_string_c_side. */
#include <string.h>

/* A C function returning a string C owns (a literal, as strerror returns
 * a static buffer). */
static const char* fixed_name(int n) {
    return n > 0 ? "positive" : "other";
}

typedef const char* (*name_fn)(int);

void* fixed_name_ptr(void) {
    return (void*)fixed_name;
}

/* An Aether-declared struct (Ops) laid over C memory, or filled by C. */
struct Ops {
    name_fn name;
    int k;
};

static struct Ops g_ops = { fixed_name, 7 };

void* c_ops(void) {
    return &g_ops;
}

void* c_ops_typed(void) {
    return &g_ops;
}

void c_fill_ops(void* out) {
    struct Ops* o = (struct Ops*)out;
    o->name = fixed_name;
    o->k = 9;
}

/* Calls an Aether callback with a fn pointer C made. */
typedef int (*user_cb)(name_fn);
int c_call_with_name(void* cb) {
    return ((user_cb)cb)(fixed_name);
}

/* A C function returning a string C owns, declared as an Aether extern. */
const char* c_label(int n) {
    return n > 0 ? "label-pos" : "label-neg";
}

/* Calls a function pointer it was handed and reads its result as C text. */
int c_len_of(void* fn) {
    const char* (*f)(int) = (const char* (*)(int))fn;
    return (int)strlen(f(0));
}

int c_len_typed(const char* (*f)(int)) {
    return (int)strlen(f(0));
}
