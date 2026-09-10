// Aether Profiler Demo
// Demonstrates the web-based profiler dashboard

#include <stdio.h>
#include <stdlib.h>
#include "profiler_server.h"
#include "../../runtime/memory/aether_memory_stats.h"
#include "../../runtime/memory/aether_arena.h"

#ifdef _WIN32
    #include <windows.h>
    #define sleep(n) Sleep((n) * 1000)
    #define usleep(n) Sleep((n) / 1000)
#else
    #include <unistd.h>
#endif

// Simulate actor activity for profiling
void simulate_actor_activity(void) {
    printf("Simulating actor activity for profiling...\n");
    
    Arena* arena = arena_create(1024 * 1024); // 1MB arena
    
    for (int i = 0; i < 100; i++) {
        double current_time = profiler_get_time_ms();
        
        // Simulate actor spawns
        ProfilerEvent spawn_event = {
            .type = PROF_EVENT_ACTOR_SPAWN,
            .timestamp_ms = current_time,
            .actor_id = i + 1,
            .target_actor_id = 0,
            .message_type = 0,
            .message_payload = 0,
            .memory_bytes = 0,
            .thread_id = 0
        };
        snprintf(spawn_event.custom_data, sizeof(spawn_event.custom_data), 
                "Spawned actor %d", i + 1);
        profiler_record_event(&spawn_event);
        
        // Simulate message passing
        if (i > 0) {
            ProfilerEvent msg_event = {
                .type = PROF_EVENT_ACTOR_MESSAGE_SENT,
                .timestamp_ms = current_time,
                .actor_id = i,
                .target_actor_id = i + 1,
                .message_type = 100,
                .message_payload = i * 10,
                .memory_bytes = 0,
                .thread_id = 0
            };
            profiler_record_event(&msg_event);
        }
        
        // Simulate memory allocations
        arena_alloc(arena, 128 + (i % 512));
        memory_stats_record_alloc(128 + (i % 512));
        
        ProfilerEvent mem_event = {
            .type = PROF_EVENT_MEMORY_ALLOC,
            .timestamp_ms = current_time,
            .actor_id = i + 1,
            .target_actor_id = 0,
            .message_type = 0,
            .message_payload = 0,
            .memory_bytes = 128 + (i % 512),
            .thread_id = 0
        };
        profiler_record_event(&mem_event);
        
        // Small delay to spread events over time
        usleep(10000); // 10ms
    }
    
    arena_destroy(arena);
    
    printf("Activity simulation complete. Events recorded: 300\n");
}

int main(void) {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║     Aether Profiler Demo                       ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    // Initialize memory tracking
    memory_stats_init();
    
    // Initialize profiler
    ProfilerConfig config = {
        .enabled = 1,
        .port = 8081,
        .bind_address = "0.0.0.0",
        .max_events = 10000,
        .collection_interval_ms = 100
    };
    
    profiler_init(&config);
    profiler_start_server();
    
    printf("\n[Demo] Generating sample profiling data...\n\n");
    
    // Simulate some activity
    simulate_actor_activity();
    
    printf("\n[Demo] Profiler is running. Press Ctrl+C to exit.\n");
    printf("[Demo] Keep the program running to interact with the dashboard.\n\n");
    
    // Keep server running
    while (1) {
        sleep(1);
        
        // Periodically generate more events
        static int counter = 0;
        if (++counter % 5 == 0) {
            ProfilerEvent event = {
                .type = PROF_EVENT_ACTOR_MESSAGE_PROCESSED,
                .timestamp_ms = profiler_get_time_ms(),
                .actor_id = counter % 10 + 1,
                .message_type = 200,
                .memory_bytes = 0
            };
            snprintf(event.custom_data, sizeof(event.custom_data), 
                    "Heartbeat event %d", counter);
            profiler_record_event(&event);
        }
    }
    
    profiler_shutdown();
    memory_stats_print();
    
    return 0;
}

