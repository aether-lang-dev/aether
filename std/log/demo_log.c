// Aether Logging System Demo

#include <stdio.h>
#include "aether_log.h"

void perform_task(int task_id) {
    LOG_DEBUG("Starting task %d", task_id);
    
    if (task_id % 3 == 0) {
        LOG_WARN("Task %d requires special handling", task_id);
    }
    
    if (task_id == 7) {
        LOG_ERROR_LOC("Task %d failed with error code 42", task_id);
        return;
    }
    
    LOG_INFO("Task %d completed successfully", task_id);
}

int main() {
    printf("Aether Logging System Demo\n");
    printf("===========================\n\n");
    
    // Initialize with DEBUG level
    log_init_raw("demo.log", LOG_LEVEL_DEBUG);
    log_set_colors(1);
    log_set_timestamps(1);
    
    LOG_INFO("Application started");
    
    // Perform some tasks
    for (int i = 1; i <= 10; i++) {
        perform_task(i);
    }
    
    // Print statistics
    log_print_stats();
    
    LOG_INFO("Application shutting down");
    log_shutdown();
    
    printf("\n✅ Log file written to: demo.log\n");
    
    return 0;
}

