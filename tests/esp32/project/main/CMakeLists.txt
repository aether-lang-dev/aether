# The generated program plus the runtime the WASM leg links (Makefile
# ci-wasm RUNTIME_FILES; keep the two lists in step), with the cooperative
# scheduler. The generated C supplies app_main() under ESP_PLATFORM.
set(AE /aether)
set(AE_RUNTIME
    runtime/scheduler/aether_scheduler_coop.c runtime/scheduler/scheduler_optimizations.c
    runtime/config/aether_optimization_config.c runtime/memory/aether_arena.c
    runtime/memory/aether_pool.c runtime/memory/aether_memory_stats.c
    runtime/utils/aether_bounds_check.c runtime/utils/aether_test.c runtime/memory/aether_arena_optimized.c
    runtime/aether_runtime_types.c runtime/aether_locale_num.c runtime/utils/aether_cpu_detect.c
    runtime/utils/aether_simd_vectorized.c runtime/aether_runtime.c runtime/aether_numa.c
    runtime/aether_host.c
    runtime/actors/aether_send_buffer.c runtime/actors/aether_send_message.c
    runtime/actors/aether_unwind.c runtime/aether_callbacks.c
    runtime/actors/aether_actor_thread.c
    std/string/aether_string.c std/math/aether_math.c std/net/aether_http.c
    std/net/aether_http_server.c std/net/aether_http_pool.c std/net/aether_http_park.c
    std/net/aether_net.c std/net/aether_actor_bridge.c
    std/udp/aether_udp.c
    std/collections/aether_collections.c
    std/json/aether_json.c std/fs/aether_fs.c std/log/aether_log.c std/io/aether_io.c
    std/os/aether_os.c
    std/collections/aether_set.c std/collections/aether_pqueue.c std/collections/aether_intarr.c
    std/collections/aether_longarr.c std/collections/aether_bits.c
    runtime/sandbox/capsicum_autosandbox.c)
list(TRANSFORM AE_RUNTIME PREPEND "${AE}/")
idf_component_register(
    SRCS "/gen/${AE_PROG}.c" ${AE_RUNTIME}
    INCLUDE_DIRS "${AE}/runtime" "${AE}/runtime/actors" "${AE}/runtime/scheduler"
                 "${AE}/runtime/utils" "${AE}/runtime/memory" "${AE}/runtime/config"
                 "${AE}/std" "${AE}/std/string" "${AE}/std/io" "${AE}/std/math"
                 "${AE}/std/net" "${AE}/std/collections" "${AE}/std/json")
# No threads, files, sockets or environment yet (FreeRTOS pthreads and lwIP
# are a later step). The scheduler is sized for ~320 KB of DRAM: at the
# defaults its static tables alone are about 10 MB.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AETHER_NO_THREADING AETHER_NO_FILESYSTEM AETHER_NO_NETWORKING AETHER_NO_GETENV
    AETHER_NO_SIMD AETHER_NO_AFFINITY AETHER_NO_NUMA
    MAX_CORES=1 QUEUE_SIZE=64 SCHEDULER_DRAIN_BATCH=32 MAX_ACTORS_PER_CORE=256)
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
