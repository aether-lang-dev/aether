#include "profiler_server.h"
#include "../../runtime/memory/aether_memory_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <arpa/inet.h>
#endif

// Global profiler state
static struct {
    ProfilerConfig config;
    int initialized;
    int server_running;
    int server_socket;
    pthread_t server_thread;
    
    // Ring buffer for events
    ProfilerEvent* events;
    int event_count;
    int event_index;
    pthread_mutex_t event_mutex;
    
    // Metrics accumulation
    size_t total_messages_sent;
    size_t total_messages_processed;
    double total_message_latency_ms;
    int active_actors_count;
    
    double start_time_ms;
} g_profiler = {0};

// Helper functions
double profiler_get_time_ms(void) {
    struct timespec ts;
    #ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
    #else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    #endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static double get_current_time_ms(void) {
    return profiler_get_time_ms();
}

static void init_winsock(void) {
    #ifdef _WIN32
    static int winsock_initialized = 0;
    if (!winsock_initialized) {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
        winsock_initialized = 1;
    }
    #endif
}

// HTML Dashboard (embedded as string)
static const char* dashboard_html = 
"<!DOCTYPE html>\n"
"<html><head><meta charset=\"utf-8\"><title>Aether Profiler</title>\n"
"<style>\n"
"body { font-family: 'Segoe UI', Arial, sans-serif; margin: 0; padding: 20px; background: #1e1e1e; color: #d4d4d4; }\n"
"h1 { color: #61dafb; margin-bottom: 10px; }\n"
".container { max-width: 1400px; margin: 0 auto; }\n"
".header { background: #2d2d30; padding: 20px; border-radius: 8px; margin-bottom: 20px; }\n"
".metrics { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 15px; margin-bottom: 20px; }\n"
".metric-card { background: #2d2d30; padding: 20px; border-radius: 8px; border-left: 4px solid #61dafb; }\n"
".metric-title { font-size: 12px; color: #858585; text-transform: uppercase; margin-bottom: 5px; }\n"
".metric-value { font-size: 28px; font-weight: bold; color: #61dafb; }\n"
".metric-sub { font-size: 13px; color: #858585; margin-top: 5px; }\n"
".section { background: #2d2d30; padding: 20px; border-radius: 8px; margin-bottom: 20px; }\n"
".section h2 { margin-top: 0; color: #61dafb; font-size: 18px; }\n"
".event-log { max-height: 400px; overflow-y: auto; font-family: 'Consolas', monospace; font-size: 13px; }\n"
".event { padding: 8px; border-bottom: 1px solid #3e3e42; }\n"
".event-time { color: #4ec9b0; }\n"
".event-type { color: #dcdcaa; font-weight: bold; }\n"
".event-details { color: #ce9178; }\n"
".status { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }\n"
".status-active { background: #4ec9b0; }\n"
".status-inactive { background: #f48771; }\n"
"button { background: #0e639c; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; font-size: 14px; }\n"
"button:hover { background: #1177bb; }\n"
"</style></head><body>\n"
"<div class=\"container\">\n"
"<div class=\"header\"><h1>⚡ Aether Runtime Profiler</h1>\n"
"<p><span class=\"status status-active\"></span> Server running on <strong>http://localhost:8080</strong> | Auto-refresh: <span id=\"refresh-time\">2s</span></p>\n"
"<button onclick=\"exportJSON()\">📥 Export JSON</button> <button onclick=\"clearEvents()\">🗑️ Clear Events</button></div>\n"
"<div class=\"metrics\" id=\"metrics\"></div>\n"
"<div class=\"section\"><h2>📊 Recent Events</h2><div class=\"event-log\" id=\"events\"></div></div>\n"
"</div>\n"
"<script>\n"
"function updateMetrics() {\n"
"  fetch('/api/metrics').then(r => r.json()).then(data => {\n"
"    document.getElementById('metrics').innerHTML = `\n"
"      <div class=\"metric-card\"><div class=\"metric-title\">Memory Usage</div>\n"
"      <div class=\"metric-value\">${(data.current_bytes/(1024*1024)).toFixed(2)} MB</div>\n"
"      <div class=\"metric-sub\">Peak: ${(data.peak_bytes/(1024*1024)).toFixed(2)} MB</div></div>\n"
"      <div class=\"metric-card\"><div class=\"metric-title\">Active Allocations</div>\n"
"      <div class=\"metric-value\">${data.current_allocations}</div>\n"
"      <div class=\"metric-sub\">Total: ${data.total_allocations}</div></div>\n"
"      <div class=\"metric-card\"><div class=\"metric-title\">Active Actors</div>\n"
"      <div class=\"metric-value\">${data.active_actors}</div>\n"
"      <div class=\"metric-sub\">Messages: ${data.total_messages_sent}</div></div>\n"
"      <div class=\"metric-card\"><div class=\"metric-title\">Message Throughput</div>\n"
"      <div class=\"metric-value\">${data.total_messages_processed}</div>\n"
"      <div class=\"metric-sub\">Avg latency: ${data.avg_message_latency_ms.toFixed(2)}ms</div></div>\n"
"    `;\n"
"  });\n"
"}\n"
"function updateEvents() {\n"
"  fetch('/api/events?count=50').then(r => r.json()).then(data => {\n"
"    let html = '';\n"
"    data.events.forEach(e => {\n"
"      html += `<div class=\"event\"><span class=\"event-time\">[${e.timestamp.toFixed(1)}ms]</span> `;\n"
"      html += `<span class=\"event-type\">${e.type}</span> `;\n"
"      html += `<span class=\"event-details\">${e.details}</span></div>`;\n"
"    });\n"
"    document.getElementById('events').innerHTML = html || '<p>No events recorded yet</p>';\n"
"  });\n"
"}\n"
"function exportJSON() { window.open('/api/export', '_blank'); }\n"
"function clearEvents() { fetch('/api/clear', {method: 'POST'}).then(() => updateEvents()); }\n"
"setInterval(() => { updateMetrics(); updateEvents(); }, 2000);\n"
"updateMetrics(); updateEvents();\n"
"</script></body></html>";

// HTTP response helper
static void send_http_response(int client_socket, const char* status, const char* content_type, const char* body) {
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, content_type, strlen(body));
    
    send(client_socket, header, strlen(header), 0);
    send(client_socket, body, strlen(body), 0);
}

// Handle HTTP requests
static void handle_http_request(int client_socket, const char* request) {
    if (strstr(request, "GET / ") || strstr(request, "GET /index.html ")) {
        send_http_response(client_socket, "200 OK", "text/html", dashboard_html);
    }
    else if (strstr(request, "GET /api/metrics")) {
        MetricsSnapshot metrics = profiler_get_current_metrics();
        const char* json = profiler_metrics_to_json(&metrics);
        send_http_response(client_socket, "200 OK", "application/json", json);
    }
    else if (strstr(request, "GET /api/events")) {
        const char* json = profiler_events_to_json(50, 0);
        send_http_response(client_socket, "200 OK", "application/json", json);
    }
    else if (strstr(request, "GET /api/export")) {
        // Full data export
        char buffer[8192];
        MetricsSnapshot metrics = profiler_get_current_metrics();
        snprintf(buffer, sizeof(buffer),
            "{\"metrics\":%s,\"events\":%s}",
            profiler_metrics_to_json(&metrics),
            profiler_events_to_json(1000, 0));
        send_http_response(client_socket, "200 OK", "application/json", buffer);
    }
    else if (strstr(request, "POST /api/clear")) {
        pthread_mutex_lock(&g_profiler.event_mutex);
        g_profiler.event_count = 0;
        g_profiler.event_index = 0;
        pthread_mutex_unlock(&g_profiler.event_mutex);
        send_http_response(client_socket, "200 OK", "application/json", "{\"status\":\"cleared\"}");
    }
    else {
        send_http_response(client_socket, "404 Not Found", "text/plain", "Not Found");
    }
}

// Server thread
static void* server_thread_func(void* arg) {
    (void)arg;
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (g_profiler.server_running) {
        int client_socket = accept(g_profiler.server_socket, 
                                   (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) continue;
        
        // Read HTTP request
        char buffer[4096] = {0};
        recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        // Handle request
        handle_http_request(client_socket, buffer);
        
        close(client_socket);
    }
    
    return NULL;
}

// Public API Implementation

void profiler_init(ProfilerConfig* config) {
    if (g_profiler.initialized) return;
    
    memcpy(&g_profiler.config, config, sizeof(ProfilerConfig));
    g_profiler.initialized = 1;
    g_profiler.start_time_ms = get_current_time_ms();
    
    // Allocate event ring buffer
    g_profiler.events = (ProfilerEvent*)calloc(config->max_events, sizeof(ProfilerEvent));
    g_profiler.event_count = 0;
    g_profiler.event_index = 0;
    pthread_mutex_init(&g_profiler.event_mutex, NULL);
    
    printf("[Profiler] Initialized with max %d events\n", config->max_events);
}

void profiler_shutdown(void) {
    if (!g_profiler.initialized) return;
    
    profiler_stop_server();
    
    free(g_profiler.events);
    pthread_mutex_destroy(&g_profiler.event_mutex);
    
    memset(&g_profiler, 0, sizeof(g_profiler));
    printf("[Profiler] Shutdown complete\n");
}

void profiler_start_server(void) {
    if (!g_profiler.initialized || g_profiler.server_running) return;
    
    init_winsock();
    
    g_profiler.server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_profiler.server_socket < 0) {
        fprintf(stderr, "[Profiler] Failed to create socket\n");
        return;
    }
    
    // Allow port reuse
    int opt = 1;
    setsockopt(g_profiler.server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(g_profiler.config.port);
    
    if (bind(g_profiler.server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "[Profiler] Failed to bind to port %d\n", g_profiler.config.port);
        close(g_profiler.server_socket);
        return;
    }
    
    if (listen(g_profiler.server_socket, 5) < 0) {
        fprintf(stderr, "[Profiler] Failed to listen\n");
        close(g_profiler.server_socket);
        return;
    }
    
    g_profiler.server_running = 1;
    pthread_create(&g_profiler.server_thread, NULL, server_thread_func, NULL);
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║   Aether Profiler Dashboard Running               ║\n");
    printf("║   Open: http://localhost:%d                     ║\n", g_profiler.config.port);
    printf("╚═══════════════════════════════════════════════════╝\n");
    printf("\n");
}

void profiler_stop_server(void) {
    if (!g_profiler.server_running) return;
    
    g_profiler.server_running = 0;
    close(g_profiler.server_socket);
    pthread_join(g_profiler.server_thread, NULL);
    
    printf("[Profiler] Server stopped\n");
}

void profiler_record_event(ProfilerEvent* event) {
    if (!g_profiler.initialized || !g_profiler.config.enabled) return;
    
    pthread_mutex_lock(&g_profiler.event_mutex);
    
    // Add to ring buffer
    int index = g_profiler.event_index;
    memcpy(&g_profiler.events[index], event, sizeof(ProfilerEvent));
    
    g_profiler.event_index = (g_profiler.event_index + 1) % g_profiler.config.max_events;
    if (g_profiler.event_count < g_profiler.config.max_events) {
        g_profiler.event_count++;
    }
    
    // Update accumulator metrics
    if (event->type == PROF_EVENT_ACTOR_MESSAGE_SENT) {
        g_profiler.total_messages_sent++;
    } else if (event->type == PROF_EVENT_ACTOR_MESSAGE_PROCESSED) {
        g_profiler.total_messages_processed++;
    }
    
    pthread_mutex_unlock(&g_profiler.event_mutex);
}

int profiler_is_enabled(void) {
    return g_profiler.initialized && g_profiler.config.enabled;
}

MetricsSnapshot profiler_get_current_metrics(void) {
    MetricsSnapshot metrics = {0};
    
    // Get memory stats
    MemoryStats mem_stats = memory_stats_get();
    metrics.total_allocations = mem_stats.total_allocations;
    metrics.total_frees = mem_stats.total_frees;
    metrics.current_allocations = mem_stats.current_allocations;
    metrics.peak_allocations = mem_stats.peak_allocations;
    metrics.bytes_allocated = mem_stats.bytes_allocated;
    metrics.bytes_freed = mem_stats.bytes_freed;
    metrics.current_bytes = mem_stats.current_bytes;
    metrics.peak_bytes = mem_stats.peak_bytes;
    
    // Get profiler-tracked metrics
    pthread_mutex_lock(&g_profiler.event_mutex);
    metrics.total_messages_sent = g_profiler.total_messages_sent;
    metrics.total_messages_processed = g_profiler.total_messages_processed;
    metrics.active_actors = g_profiler.active_actors_count;

    /* Not instrumented. Events carry no message id, so a SENT event cannot
     * be matched to the PROCESSED event it caused; any pairing would be an
     * invented approximation, and scanning the ring on every poll would be
     * quadratic while holding this mutex. Reported as zero so the dashboard
     * cannot present a guess as a measurement. It previously returned a
     * hardcoded 0.5, which read as the user's real latency. Instrumenting
     * this needs a correlation id on the event, not a cleverer reader. */
    metrics.avg_message_latency_ms = 0.0;
    pthread_mutex_unlock(&g_profiler.event_mutex);

    metrics.timestamp_ms = get_current_time_ms() - g_profiler.start_time_ms;
    /* Not instrumented yet. Reported as zero rather than an invented
     * constant so the dashboard cannot present them as measurements. */
    metrics.active_threads = 0;
    metrics.tasks_completed = 0;
    metrics.cpu_utilization = 0.0;
    
    return metrics;
}

const char* profiler_metrics_to_json(MetricsSnapshot* metrics) {
    static char buffer[2048];
    snprintf(buffer, sizeof(buffer),
        "{"
        "\"total_allocations\":%zu,"
        "\"total_frees\":%zu,"
        "\"current_allocations\":%zu,"
        "\"peak_allocations\":%zu,"
        "\"bytes_allocated\":%zu,"
        "\"bytes_freed\":%zu,"
        "\"current_bytes\":%zu,"
        "\"peak_bytes\":%zu,"
        "\"active_actors\":%d,"
        "\"total_messages_sent\":%zu,"
        "\"total_messages_processed\":%zu,"
        "\"avg_message_latency_ms\":%.2f,"
        "\"timestamp_ms\":%.2f"
        "}",
        (size_t)metrics->total_allocations,
        (size_t)metrics->total_frees,
        (size_t)metrics->current_allocations,
        (size_t)metrics->peak_allocations,
        (size_t)metrics->bytes_allocated,
        (size_t)metrics->bytes_freed,
        (size_t)metrics->current_bytes,
        (size_t)metrics->peak_bytes,
        metrics->active_actors,
        (size_t)metrics->total_messages_sent,
        (size_t)metrics->total_messages_processed,
        metrics->avg_message_latency_ms,
        metrics->timestamp_ms);
    return buffer;
}

const char* profiler_events_to_json(int count, int offset) {
    static char buffer[16384];
    char* ptr = buffer;
    char* end = buffer + sizeof(buffer);
    ptr += snprintf(ptr, end - ptr, "{\"events\":[");
    
    pthread_mutex_lock(&g_profiler.event_mutex);
    
    int start = (g_profiler.event_index - g_profiler.event_count + g_profiler.config.max_events) 
                % g_profiler.config.max_events;
    /* `offset` pages back from the newest event: offset 0 returns the
     * latest `count` events, offset N skips the N newest. It was accepted
     * and ignored before, so every page repeated the same events. */
    if (offset < 0) offset = 0;
    int available = g_profiler.event_count > offset ? g_profiler.event_count - offset : 0;
    int num_events = available < count ? available : count;
    
    for (int i = 0; i < num_events && ptr < buffer + sizeof(buffer) - 256; i++) {
        int idx = (start + g_profiler.event_count - offset - num_events + i + g_profiler.config.max_events) 
                  % g_profiler.config.max_events;
        ProfilerEvent* e = &g_profiler.events[idx];
        
        const char* type_str = "UNKNOWN";
        char details[128] = {0};
        
        switch (e->type) {
            case PROF_EVENT_ACTOR_SPAWN:
                type_str = "ACTOR_SPAWN";
                snprintf(details, sizeof(details), "actor_%d spawned", e->actor_id);
                break;
            case PROF_EVENT_ACTOR_MESSAGE_SENT:
                type_str = "MSG_SENT";
                snprintf(details, sizeof(details), "actor_%d → actor_%d (type=%d)", 
                        e->actor_id, e->target_actor_id, e->message_type);
                break;
            case PROF_EVENT_ACTOR_MESSAGE_RECEIVED:
                type_str = "MSG_RECEIVED";
                snprintf(details, sizeof(details), "actor_%d received msg (type=%d)", 
                        e->actor_id, e->message_type);
                break;
            case PROF_EVENT_MEMORY_ALLOC:
                type_str = "MEM_ALLOC";
                snprintf(details, sizeof(details), "%zu bytes", e->memory_bytes);
                break;
            default:
                snprintf(details, sizeof(details), "%s", e->custom_data);
                break;
        }
        
        ptr += snprintf(ptr, end - ptr, "%s{\"type\":\"%s\",\"timestamp\":%.2f,\"details\":\"%s\"}",
                      i > 0 ? "," : "", type_str, e->timestamp_ms, details);
    }
    
    pthread_mutex_unlock(&g_profiler.event_mutex);
    
    snprintf(ptr, end - ptr, "]}");
    return buffer;
}

