#include "vt.h"

unsigned long vt_call_hash(const dictType* t, const void* key) {
    return t->hashFunction(key);
}

int vt_call_cmp(const dictType* t, const void* a, const void* b) {
    return t->keyCompare(a, b);
}
