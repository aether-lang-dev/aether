#ifndef AETHER_PROFILER_SERVER_H
#define AETHER_PROFILER_SERVER_H

#include <stddef.h>
#include <stdint.h>

// Profiler configuration
typedef struct {
    int enabled;
    int port;
    const char* bind_address;
    int max_events;           // Maximum events to store in ring buffer
    int collection_interval_ms; // How often to collect metrics
} ProfilerConfig;

// Event types for profiling
typedef enum {
    PROF_EVENT_ACTOR_SPAWN,
    PROF_EVENT_ACTOR_MESSAGE_SENT,
    PROF_EVENT_ACTOR_MESSAGE_RECEIVED,
    PROF_EVENT_ACTOR_MESSAGE_PROCESSED,
    PROF_EVENT_MEMORY_ALLOC,
    PROF_EVENT_MEMORY_FREE,
    PROF_EVENT_SCHEDULER_TASK
} ProfilerEventType;

// Profiler event structure
typedef struct {
    ProfilerEventType type;
    double timestamp_ms;
    int actor_id;
    int target_actor_id;      // For messages
    int message_type;
    int message_payload;
    size_t memory_bytes;
    int thread_id;
    char custom_data[64];
} ProfilerEvent;

// Profiler API
void profiler_init(ProfilerConfig* config);
void profiler_shutdown(void);
void profiler_start_server(void);
void profiler_stop_server(void);
void profiler_record_event(ProfilerEvent* event);
int profiler_is_enabled(void);

// Metric snapshot for JSON export
typedef struct {
    // Memory metrics
    size_t total_allocations;
    size_t total_frees;
    size_t current_allocations;
    size_t peak_allocations;
    size_t bytes_allocated;
    size_t bytes_freed;
    size_t current_bytes;
    size_t peak_bytes;
    
    // Actor metrics
    int active_actors;
    size_t total_messages_sent;
    size_t total_messages_processed;
    double avg_message_latency_ms;
    
    // Scheduler metrics
    int active_threads;
    size_t tasks_completed;
    double cpu_utilization;
    
    double timestamp_ms;
} MetricsSnapshot;

MetricsSnapshot profiler_get_current_metrics(void);
const char* profiler_metrics_to_json(MetricsSnapshot* metrics);
const char* profiler_events_to_json(int count, int offset);

// Helper function
double profiler_get_time_ms(void);

#endif // AETHER_PROFILER_SERVER_H

