#ifndef AETHER_STRMAP_H
#define AETHER_STRMAP_H

#include <stddef.h>

/* A string-keyed hash map for the compiler's own bookkeeping (#2007).
 *
 * Several passes answer "which top-level definition has this name?" or
 * "have I seen this name?" once per call site, per identifier, or per
 * function. Each of them did it with a walk of the whole list, which is
 * fine for a hundred functions and quadratic for a thousand; the
 * measurements in #2007 put three such walks at three quarters of a
 * 6400-function compile. This is the one structure they share.
 *
 * Open addressing with linear probing; keys are copied; values are
 * `void*` the caller owns. Iteration order is insertion order, exposed
 * through strmap_count / strmap_key_at / strmap_value_at so a pass that
 * needs "every definition, in program order" can have it. Never shrinks.
 */
typedef struct {
    char* key;
    void* value;
} StrMapEntry;

typedef struct {
    StrMapEntry* entries;      /* insertion order */
    int count;
    int capacity;
    int* slots;                /* entry index + 1; 0 = empty */
    int slot_count;            /* power of two, >= 2 * count */
} StrMap;

/* Zero-initialise a StrMap (`StrMap m = {0};` is equivalent). */
void strmap_init(StrMap* m);
/* Release the map's own storage. Values are not touched. */
void strmap_free(StrMap* m);

/* The value stored under `key`, or NULL when absent. NULL is also a legal
 * stored value; use strmap_has to tell the two apart. */
void* strmap_get(const StrMap* m, const char* key);
int strmap_has(const StrMap* m, const char* key);

/* Store `value` under `key`, replacing an earlier value. Returns the previous
 * value (NULL when the key was new). */
void* strmap_put(StrMap* m, const char* key, void* value);

/* The number of keys, and the i-th key / value in insertion order. */
int strmap_count(const StrMap* m);
const char* strmap_key_at(const StrMap* m, int i);
void* strmap_value_at(const StrMap* m, int i);

/* The hash the map uses, exposed for callers that keep a parallel table. */
unsigned strmap_hash(const char* s);

#endif
