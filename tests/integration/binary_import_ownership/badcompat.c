struct _AeLibInfoMeta {
    const char* schema_version;
    const char* aether_version;
    const char* primary_source;
    int function_count;
    const void* functions;
    int closure_count;
    const void* closures;
    int constant_count;
    const void* constants;
    int struct_count;
    const void* structs;
    const char* const* source_signatures;
    const char* const* function_modules;
    const char* const* constant_modules;
    const char* runtime;
    const char* const* closure_modules;
    int actors;
    const unsigned char* closure_config_unused;
    const char* string_release;
    const char* outcome;
    int host_abi;
    const char* runtime_abi;
    const char* requires;
};
static const struct _AeLibInfoMeta meta = {
    .schema_version = "99.0",
    .aether_version = "99.0.0",
};
#if defined(_WIN32)
__declspec(dllexport)
#endif
const struct _AeLibInfoMeta* aether_lib_meta(void) {
    return &meta;
}
