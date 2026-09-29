#include "aether_defines.h"

#include <stdlib.h>
#include <string.h>

#define AETHER_MAX_DEFINES 128
#define AETHER_MAX_DEFINE_LEN 128

static char g_defines[AETHER_MAX_DEFINES][AETHER_MAX_DEFINE_LEN];
static int  g_define_count = 0;

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
    if (strlen(name) >= AETHER_MAX_DEFINE_LEN) return 0;
    if (aether_define_is_set(name)) return 1;
    if (g_define_count >= AETHER_MAX_DEFINES) return 0;
    /* strncpy with an explicit terminator: the length is checked above, so
     * this cannot truncate, and the terminator keeps it true if that check
     * ever moves. */
    strncpy(g_defines[g_define_count], name, AETHER_MAX_DEFINE_LEN - 1);
    g_defines[g_define_count][AETHER_MAX_DEFINE_LEN - 1] = '\0';
    g_define_count++;
    return 1;
}

static int  g_recording = 0;
static int  g_query_overflow = 0;
static int  g_query_count = 0;
static char g_query_names[AETHER_MAX_DEFINES][AETHER_MAX_DEFINE_LEN];
static int  g_query_values[AETHER_MAX_DEFINES];

static void record_query(const char* name, int value) {
    for (int i = 0; i < g_query_count; i++) {
        if (strcmp(g_query_names[i], name) == 0) return;
    }
    if (g_query_count >= AETHER_MAX_DEFINES || strlen(name) >= AETHER_MAX_DEFINE_LEN) {
        g_query_overflow = 1;
        return;
    }
    strncpy(g_query_names[g_query_count], name, AETHER_MAX_DEFINE_LEN - 1);
    g_query_names[g_query_count][AETHER_MAX_DEFINE_LEN - 1] = '\0';
    g_query_values[g_query_count] = value;
    g_query_count++;
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

void aether_defines_clear(void) { g_define_count = 0; }
