#include "aether_defines.h"

#include <stdlib.h>
#include <string.h>

/* Every symbol and every name asked, as long as it is (#3574): 128 names of
 * 127 bytes refused a longer -D name as "too many -D symbols", and a module
 * asking about more than 128 names got no artifact. Names are copies the
 * tables own; aether_defines_clear and a new recording give them back. */
static char** g_defines = NULL;
static int  g_define_count = 0;
static int  g_define_cap = 0;

/* A copy of `name` at the end of `*names`, grown as it must; 0 out of
 * memory. */
static int names_push(char*** names, int* count, int* cap, const char* name) {
    if (*count == *cap) {
        int ncap = *cap ? *cap * 2 : 16;
        char** grown = (char**)realloc(*names, sizeof(char*) * (size_t)ncap);
        if (!grown) return 0;
        *names = grown;
        *cap = ncap;
    }
    char* copy = (char*)malloc(strlen(name) + 1);
    if (!copy) return 0;
    strcpy(copy, name);
    (*names)[(*count)++] = copy;
    return 1;
}

/* Build symbols are matched by `defined(NAME)`, where NAME is an identifier,
 * so anything that could not be written there is rejected rather than stored.
 * A name arriving with a space (from a `-D FOO` that reached the process as
 * one argument, which is what a quoted command string produces) would
 * otherwise be accepted and then never match anything. */
int aether_define_is_valid_name(const char* name) {
    if (!name || !*name) return 0;
    if (!((name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return 0;
    for (const char* p = name; *p; p++) {
        int ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                 (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok) return 0;
    }
    return 1;
}

int aether_define_add(const char* name) {
    if (!aether_define_is_valid_name(name)) return 0;
    if (aether_define_is_set(name)) return 1;
    return names_push(&g_defines, &g_define_count, &g_define_cap, name);
}

static int  g_recording = 0;
static int  g_query_overflow = 0;   /* out of memory: the log is incomplete */
static int  g_query_count = 0;
static int  g_query_cap = 0;
static char** g_query_names = NULL;
static int* g_query_values = NULL;

static void record_query(const char* name, int value) {
    for (int i = 0; i < g_query_count; i++) {
        if (strcmp(g_query_names[i], name) == 0) return;
    }
    int cap = g_query_cap;
    if (!names_push(&g_query_names, &g_query_count, &g_query_cap, name)) {
        g_query_overflow = 1;
        return;
    }
    if (g_query_cap != cap) {
        int* grown = (int*)realloc(g_query_values, sizeof(int) * (size_t)g_query_cap);
        if (!grown) {
            free(g_query_names[--g_query_count]);
            g_query_overflow = 1;
            return;
        }
        g_query_values = grown;
    }
    g_query_values[g_query_count - 1] = value;
}

int aether_define_is_set(const char* name) {
    if (!name) return 0;
    int value = 0;
    for (int i = 0; i < g_define_count; i++) {
        if (strcmp(g_defines[i], name) == 0) { value = 1; break; }
    }
    if (g_recording) record_query(name, value);
    return value;
}

void aether_define_record_queries(int on) {
    g_recording = on ? 1 : 0;
    if (on) {
        for (int i = 0; i < g_query_count; i++) free(g_query_names[i]);
        g_query_count = 0;
        g_query_overflow = 0;
    }
}

int aether_define_query_count(void) { return g_query_count; }

int aether_define_query_overflowed(void) { return g_query_overflow; }

const char* aether_define_query_name(int index) {
    if (index < 0 || index >= g_query_count) return NULL;
    return g_query_names[index];
}

int aether_define_query_value(int index) {
    if (index < 0 || index >= g_query_count) return 0;
    return g_query_values[index];
}

int aether_define_count(void) { return g_define_count; }

const char* aether_define_at(int index) {
    if (index < 0 || index >= g_define_count) return NULL;
    return g_defines[index];
}

void aether_defines_clear(void) {
    for (int i = 0; i < g_define_count; i++) free(g_defines[i]);
    g_define_count = 0;
}
