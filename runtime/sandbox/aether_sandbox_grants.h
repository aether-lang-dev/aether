#ifndef AETHER_SANDBOX_GRANTS_H
#define AETHER_SANDBOX_GRANTS_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define AETHER_SANDBOX_GRANTS_SIZE 8192

extern int list_size(void*);
extern void* list_get_raw(void*, int);
extern const char* aether_string_data(const void*);

static int aether_sandbox_serialize_grants(void* grant_list, char* buf,
                                           size_t buf_size, size_t* data_size) {
    if (!grant_list || !buf || !data_size || buf_size == 0) {
        errno = EINVAL;
        return -1;
    }

    int n = list_size(grant_list);
    if (n < 0 || n % 2 != 0) {
        errno = EINVAL;
        return -1;
    }

    size_t pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i += 2) {
        void* category_item = list_get_raw(grant_list, i);
        void* pattern_item = list_get_raw(grant_list, i + 1);
        if (!category_item || !pattern_item) {
            errno = EINVAL;
            return -1;
        }
        const char* category = aether_string_data(category_item);
        const char* pattern = aether_string_data(pattern_item);
        if (strchr(category, ':') ||
            strpbrk(category, "\r\n") || strpbrk(pattern, "\r\n")) {
            errno = EINVAL;
            return -1;
        }

        int written = snprintf(buf + pos, buf_size - pos, "%s:%s\n",
                               category, pattern);
        if (written < 0 || (size_t)written >= buf_size - pos) {
            errno = E2BIG;
            return -1;
        }
        pos += (size_t)written;
    }

    *data_size = pos + 1;
    return 0;
}

#endif
