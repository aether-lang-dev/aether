/* A Redis-shaped callback table: dictType, rio method tables, connection
 * vtables all have this form. The C side calls through the fields itself, so
 * whatever Aether stores in them has to be a real function address. */
#ifndef AETHER_TEST_VT_H
#define AETHER_TEST_VT_H

typedef struct dictType {
    unsigned long (*hashFunction)(const void* key);
    int           (*keyCompare)(const void* a, const void* b);
} dictType;

unsigned long vt_call_hash(const dictType* t, const void* key);
int           vt_call_cmp(const dictType* t, const void* a, const void* b);
#endif
