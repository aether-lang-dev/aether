// SIMD Header - AVX2 Vectorized Operations
#ifndef AETHER_SIMD_VECTORIZED_H
#define AETHER_SIMD_VECTORIZED_H

#include <stdint.h>

// Initialize SIMD system (detects AVX2)
void aether_simd_init(void);

// Check if AVX2 is available
int aether_simd_is_available(void);

// Vectorized operations (8-wide AVX2 or scalar fallback)
void extract_message_ids_avx2(const void** msg_data, int32_t* msg_ids, int count);
int filter_messages_by_type_avx2(const int32_t* msg_ids, int count, int32_t target_type, int* indices);
void increment_counters_avx2(int32_t* counters, const int32_t* increments, int count);
int count_active_actors_avx2(const uint8_t* active_flags, int count);

#endif // AETHER_SIMD_VECTORIZED_H
