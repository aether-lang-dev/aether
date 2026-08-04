#include "api.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

static blob g_blob = { 7, 3 };

uint8_t api_scale(uint8_t v) { return (uint8_t)(v * 3); }

size_t api_span(size_t a, size_t b) { return b > a ? b - a : a - b; }

blob* api_blob(void) { return &g_blob; }

size_t api_blob_len(const blob* b) { return b ? b->len : 0; }

char* api_fmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;

    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) return NULL;

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}
