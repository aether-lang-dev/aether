/*
 * aether_lib.i — Per-project SWIG interface.
 *
 * The user's .ae file exports aether_<name>(...) entry points. For SWIG to
 * wrap them, a small header describing those signatures must exist. In a
 * real project you'd maintain this header; for the test we inline the
 * declarations directly in the .i file.
 *
 * A `-> string` export returns a string the caller owns (#2931): `shout`
 * copies its bytes out, gives it back through the library's release, and
 * SWIG frees the copy once Python has its own (%newobject). The helper has
 * the library function's own name, `shout`: the library's call to its `shout`
 * stays inside the library (#2937), where on ELF it bound to this helper.
 */

%module aether_lib

%{
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern int32_t aether_add(int32_t a, int32_t b);
extern const void* aether_shout(const char* s);
extern const char* aether_string_data(const void* s);
extern size_t aether_string_length(const void* s);
extern void aether_config_string_release(const void* s);
%}

%include <stdint.i>

int32_t aether_add(int32_t a, int32_t b);

%newobject shout;
%inline %{
char* shout(const char* s) {
    const void* h = aether_shout(s);
    size_t n = aether_string_length(h);
    char* out = (char*)malloc(n + 1);
    if (out) {
        memcpy(out, aether_string_data(h), n);
        out[n] = '\0';
    }
    aether_config_string_release(h);
    return out;
}
%}
