# The generated library plus the cooperative runtime (the one list every
# cooperative build reads) and the C host.
set(AE /aether)
file(STRINGS "${AE}/runtime/coop_runtime_sources.txt" AE_RUNTIME REGEX "^[^#]")
list(TRANSFORM AE_RUNTIME PREPEND "${AE}/")
idf_component_register(
    SRCS "/gen/ctxlib.c" "host_esp.c" ${AE_RUNTIME}
    INCLUDE_DIRS "${AE}/include" "${AE}/runtime" "${AE}/runtime/actors" "${AE}/runtime/scheduler"
                 "${AE}/runtime/utils" "${AE}/runtime/memory" "${AE}/runtime/config"
                 "${AE}/std" "${AE}/std/string" "${AE}/std/io" "${AE}/std/math"
                 "${AE}/std/net" "${AE}/std/collections" "${AE}/std/json")
# As the esp32 lane, and as the smart-plug firmware builds Aether.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AETHER_NO_THREADING AETHER_NO_FILESYSTEM AETHER_NO_NETWORKING AETHER_NO_GETENV
    AETHER_NO_SIMD AETHER_NO_AFFINITY AETHER_NO_NUMA
    MAX_CORES=1 QUEUE_SIZE=64 SCHEDULER_DRAIN_BATCH=32 MAX_ACTORS_PER_CORE=256
    AETHER_ACCT_FILTER_LOG2=14)
set_source_files_properties("/gen/ctxlib.c" ${AE_RUNTIME} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("host_esp.c" PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
