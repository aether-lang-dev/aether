#include "aether_memory_stats.h"
#include <stdio.h>
#include <string.h>

static MemoryStats g_stats = {0};

void memory_stats_init(void) {
    memset(&g_stats, 0, sizeof(MemoryStats));
}

void memory_stats_record_alloc(size_t bytes) {
    g_stats.total_allocations++;
    g_stats.current_allocations++;
    g_stats.bytes_allocated += bytes;
    g_stats.current_bytes += bytes;
    
    if (g_stats.current_allocations > g_stats.peak_allocations) {
        g_stats.peak_allocations = g_stats.current_allocations;
    }
    
    if (g_stats.current_bytes > g_stats.peak_bytes) {
        g_stats.peak_bytes = g_stats.current_bytes;
    }
}

void memory_stats_record_free(size_t bytes) {
    g_stats.total_frees++;
    if (g_stats.current_allocations > 0) {
        g_stats.current_allocations--;
    }
    g_stats.bytes_freed += bytes;
    if (g_stats.current_bytes >= bytes) {
        g_stats.current_bytes -= bytes;
    }
}

void memory_stats_record_failure(void) {
    g_stats.allocation_failures++;
}

MemoryStats memory_stats_get(void) {
    return g_stats;
}

void memory_stats_print(void) {
    printf("\n========== Memory Statistics ==========\n");
    printf("Allocations:\n");
    printf("  Total:   %llu\n", (unsigned long long)g_stats.total_allocations);
    printf("  Frees:   %llu\n", (unsigned long long)g_stats.total_frees);
    printf("  Current: %llu\n", (unsigned long long)g_stats.current_allocations);
    printf("  Peak:    %llu\n", (unsigned long long)g_stats.peak_allocations);
    printf("  Failures: %llu\n", (unsigned long long)g_stats.allocation_failures);
    printf("\nBytes:\n");
    printf("  Allocated: %llu (%.2f MB)\n", 
           (unsigned long long)g_stats.bytes_allocated,
           g_stats.bytes_allocated / (1024.0 * 1024.0));
    printf("  Freed:     %llu (%.2f MB)\n", 
           (unsigned long long)g_stats.bytes_freed,
           g_stats.bytes_freed / (1024.0 * 1024.0));
    printf("  Current:   %llu (%.2f MB)\n", 
           (unsigned long long)g_stats.current_bytes,
           g_stats.current_bytes / (1024.0 * 1024.0));
    printf("  Peak:      %llu (%.2f MB)\n", 
           (unsigned long long)g_stats.peak_bytes,
           g_stats.peak_bytes / (1024.0 * 1024.0));
    printf("\nLeak Detection:\n");
    if (g_stats.current_allocations == 0 && g_stats.current_bytes == 0) {
        printf("  ✓ No memory leaks detected\n");
    } else {
        printf("  ⚠ Potential leak: %llu allocations, %llu bytes\n",
               (unsigned long long)g_stats.current_allocations,
               (unsigned long long)g_stats.current_bytes);
    }
    printf("=======================================\n\n");
}

void memory_stats_reset(void) {
    memset(&g_stats, 0, sizeof(MemoryStats));
}

