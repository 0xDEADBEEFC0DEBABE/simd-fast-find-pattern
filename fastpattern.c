#include "fastpattern.h"
#include <immintrin.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

typedef enum {
    FP_CPU_SSE2 = 1 << 0,
    FP_CPU_SSE42 = 1 << 1,
    FP_CPU_AVX2 = 1 << 2,
    FP_CPU_AVX512F = 1 << 3,
    FP_CPU_AVX512BW = 1 << 4
} fp_cpu_features_t;

#if defined(__clang__) && defined(_WIN32)
static inline void fp_cpuid(int level, unsigned int *a, unsigned int *b, 
                            unsigned int *c, unsigned int *d) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(level)
        : "memory"
    );
}

static inline void fp_get_cpuid_count(int level, int count, unsigned int *a, 
                                      unsigned int *b, unsigned int *c, unsigned int *d) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(level), "c"(count)
        : "memory"
    );
}
#elif defined(_WIN32)
#include <intrin.h>
static inline void fp_cpuid(int level, unsigned int *a, unsigned int *b, 
                            unsigned int *c, unsigned int *d) {
    int cpuinfo[4];
    __cpuid(cpuinfo, level);
    *a = cpuinfo[0]; *b = cpuinfo[1]; *c = cpuinfo[2]; *d = cpuinfo[3];
}

static inline void fp_get_cpuid_count(int level, int count, unsigned int *a, 
                                      unsigned int *b, unsigned int *c, unsigned int *d) {
    int cpuinfo[4];
    __cpuidex(cpuinfo, level, count);
    *a = cpuinfo[0]; *b = cpuinfo[1]; *c = cpuinfo[2]; *d = cpuinfo[3];
}
#else
#include <cpuid.h>
static inline void fp_cpuid(int level, unsigned int *a, unsigned int *b, 
                            unsigned int *c, unsigned int *d) {
    __cpuid(level, *a, *b, *c, *d);
}

static inline void fp_get_cpuid_count(int level, int count, unsigned int *a, 
                                      unsigned int *b, unsigned int *c, unsigned int *d) {
    __cpuid_count(level, count, *a, *b, *c, *d);
}
#endif

static fp_cpu_features_t fp_detect_cpu_features(void) {
    fp_cpu_features_t features = 0;
    unsigned int eax, ebx, ecx, edx;
    
    fp_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (edx & (1 << 26)) features |= FP_CPU_SSE2;
    if (ecx & (1 << 20)) features |= FP_CPU_SSE42;
    
    fp_get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
    if (ebx & (1 << 5)) features |= FP_CPU_AVX2;
    if (ebx & (1 << 16)) features |= FP_CPU_AVX512F;
    if (ebx & (1 << 30)) features |= FP_CPU_AVX512BW;
    
    return features;
}

static inline size_t search_avx512(const uint8_t* haystack, size_t haystack_len,
                                   const uint8_t* needle, size_t needle_len,
                                   const uint8_t* mask);
static inline size_t search_avx2(const uint8_t* haystack, size_t haystack_len,
                                 const uint8_t* needle, size_t needle_len,
                                 const uint8_t* mask);
static inline size_t search_sse42(const uint8_t* haystack, size_t haystack_len,
                                  const uint8_t* needle, size_t needle_len,
                                  const uint8_t* mask);
static inline size_t search_generic(const uint8_t* haystack, size_t haystack_len,
                                    const uint8_t* needle, size_t needle_len,
                                    const uint8_t* mask);

static size_t fp_search_single(const uint8_t* haystack, size_t haystack_len,
                              const uint8_t* needle, size_t needle_len,
                              const uint8_t* mask, fp_cpu_features_t features) {
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return SIZE_MAX;
    }
    
    if (features & FP_CPU_AVX512BW) {
        return search_avx512(haystack, haystack_len, needle, needle_len, mask);
    } else if (features & FP_CPU_AVX2) {
        return search_avx2(haystack, haystack_len, needle, needle_len, mask);
    } else if (features & FP_CPU_SSE42) {
        return search_sse42(haystack, haystack_len, needle, needle_len, mask);
    } else {
        return search_generic(haystack, haystack_len, needle, needle_len, mask);
    }
}

#ifdef __AVX512BW__
static inline size_t search_avx512(const uint8_t* haystack, size_t haystack_len,
                                   const uint8_t* needle, size_t needle_len,
                                   const uint8_t* mask) {
    if (needle_len >= 2) {
        uint8_t first_byte = needle[0];
        uint8_t first_mask = mask ? mask[0] : 0xFF;
        uint8_t last_byte = needle[needle_len - 1];
        uint8_t last_mask = mask ? mask[needle_len - 1] : 0xFF;
        
        size_t first_pos = 0;
        while (first_pos < needle_len && mask && mask[first_pos] == 0x00) {
            first_pos++;
        }
        if (first_pos < needle_len) {
            first_byte = needle[first_pos];
            first_mask = mask ? mask[first_pos] : 0xFF;
        }
        
        size_t last_pos = needle_len - 1;
        while (last_pos > first_pos && mask && mask[last_pos] == 0x00) {
            last_pos--;
        }
        if (last_pos >= first_pos) {
            last_byte = needle[last_pos];
            last_mask = mask ? mask[last_pos] : 0xFF;
        }
        
        const __m512i first_vec = _mm512_set1_epi8(first_byte);
        const __m512i last_vec = _mm512_set1_epi8(last_byte);
        const __m512i first_mask_vec = _mm512_set1_epi8(first_mask);
        const __m512i last_mask_vec = _mm512_set1_epi8(last_mask);
        const __m512i masked_first = _mm512_and_si512(first_vec, first_mask_vec);
        const __m512i masked_last = _mm512_and_si512(last_vec, last_mask_vec);
        
        size_t offset_diff = last_pos - first_pos;
        
        for (size_t i = 0; i <= haystack_len - needle_len; i += 64) {
            if (i + 64 + needle_len <= haystack_len) {
                _mm_prefetch((const char*)(haystack + i + 64), _MM_HINT_T0);
                
                __m512i block_first = _mm512_loadu_si512((__m512i*)(haystack + i + first_pos));
                __m512i block_last = _mm512_loadu_si512((__m512i*)(haystack + i + first_pos + offset_diff));
                
                __m512i masked_block_first = _mm512_and_si512(block_first, first_mask_vec);
                __m512i masked_block_last = _mm512_and_si512(block_last, last_mask_vec);
                
                __mmask64 eq_first = _mm512_cmpeq_epi8_mask(masked_first, masked_block_first);
                __mmask64 eq_last = _mm512_cmpeq_epi8_mask(masked_last, masked_block_last);
                
                __mmask64 matches = eq_first & eq_last;
                
                while (__builtin_expect(!!matches, 0)) {
                    int bit_pos = __builtin_ctzll(matches);
                    size_t candidate = i + bit_pos;
                    
                    if (__builtin_expect(candidate <= haystack_len - needle_len, 1)) {
                        bool is_match = true;
                        for (size_t j = 0; j < needle_len; j++) {
                            if (j == first_pos || j == last_pos) continue;
                            uint8_t hay_byte = mask ? (haystack[candidate + j] & mask[j]) : haystack[candidate + j];
                            uint8_t needle_byte = mask ? (needle[j] & mask[j]) : needle[j];
                            if (__builtin_expect(hay_byte != needle_byte, 0)) {
                                is_match = false;
                                break;
                            }
                        }
                        
                        if (__builtin_expect(is_match, 0)) {
                            return candidate;
                        }
                    }
                    
                    matches &= matches - 1;
                }
            } else {
                for (size_t j = i; j <= haystack_len - needle_len && j < i + 64; j++) {
                    bool found = true;
                    for (size_t k = 0; k < needle_len; k++) {
                        uint8_t hay_byte = mask ? (haystack[j + k] & mask[k]) : haystack[j + k];
                        uint8_t needle_byte = mask ? (needle[k] & mask[k]) : needle[k];
                        if (__builtin_expect(hay_byte != needle_byte, 1)) {
                            found = false;
                            break;
                        }
                    }
                    if (__builtin_expect(found, 0)) return j;
                }
            }
        }
        
        return SIZE_MAX;
    }
    
    if (needle_len == 1) {
        const __m512i first = _mm512_set1_epi8(needle[0]);
        const __m512i mask_vec = mask ? _mm512_set1_epi8(mask[0]) : _mm512_set1_epi8(0xFF);
        const __m512i masked_needle = _mm512_and_si512(first, mask_vec);
        
        size_t i = 0;
        for (; i + 256 <= haystack_len; i += 256) {
            _mm_prefetch((const char*)(haystack + i + 256), _MM_HINT_T0);
            
            __m512i data0 = _mm512_loadu_si512((__m512i*)(haystack + i));
            __m512i data1 = _mm512_loadu_si512((__m512i*)(haystack + i + 64));
            __m512i data2 = _mm512_loadu_si512((__m512i*)(haystack + i + 128));
            __m512i data3 = _mm512_loadu_si512((__m512i*)(haystack + i + 192));
            
            __m512i masked_data0 = _mm512_and_si512(data0, mask_vec);
            __m512i masked_data1 = _mm512_and_si512(data1, mask_vec);
            __m512i masked_data2 = _mm512_and_si512(data2, mask_vec);
            __m512i masked_data3 = _mm512_and_si512(data3, mask_vec);
            
            __mmask64 matches0 = _mm512_cmpeq_epi8_mask(masked_data0, masked_needle);
            __mmask64 matches1 = _mm512_cmpeq_epi8_mask(masked_data1, masked_needle);
            __mmask64 matches2 = _mm512_cmpeq_epi8_mask(masked_data2, masked_needle);
            __mmask64 matches3 = _mm512_cmpeq_epi8_mask(masked_data3, masked_needle);
            
            if (__builtin_expect(!!(matches0 | matches1 | matches2 | matches3), 0)) {
                if (matches0) return i + __builtin_ctzll(matches0);
                if (matches1) return i + 64 + __builtin_ctzll(matches1);
                if (matches2) return i + 128 + __builtin_ctzll(matches2);
                if (matches3) return i + 192 + __builtin_ctzll(matches3);
            }
        }
        
        for (; i + 64 <= haystack_len; i += 64) {
            __m512i data = _mm512_loadu_si512((__m512i*)(haystack + i));
            __m512i masked_data = _mm512_and_si512(data, mask_vec);
            __mmask64 matches = _mm512_cmpeq_epi8_mask(masked_data, masked_needle);
            if (__builtin_expect(!!matches, 0)) {
                return i + __builtin_ctzll(matches);
            }
        }
        
        for (; i < haystack_len; i++) {
            uint8_t hay_byte = mask ? (haystack[i] & mask[0]) : haystack[i];
            uint8_t needle_byte = mask ? (needle[0] & mask[0]) : needle[0];
            if (hay_byte == needle_byte) {
                return i;
            }
        }
        return SIZE_MAX;
    }
    
    return search_generic(haystack, haystack_len, needle, needle_len, mask);
}
#else
static inline size_t search_avx512(const uint8_t* haystack, size_t haystack_len,
                                   const uint8_t* needle, size_t needle_len,
                                   const uint8_t* mask) {
    return search_avx2(haystack, haystack_len, needle, needle_len, mask);
}
#endif

#ifdef __AVX2__
static inline size_t search_avx2(const uint8_t* haystack, size_t haystack_len,
                                 const uint8_t* needle, size_t needle_len,
                                 const uint8_t* mask) {
    if (needle_len == 1) {
        const __m256i first = _mm256_set1_epi8(needle[0]);
        const __m256i mask_vec = mask ? _mm256_set1_epi8(mask[0]) : _mm256_set1_epi8(0xFF);
        
        for (size_t i = 0; i <= haystack_len - 32; i += 32) {
            __m256i data = _mm256_loadu_si256((__m256i*)(haystack + i));
            __m256i masked_data = _mm256_and_si256(data, mask_vec);
            __m256i masked_needle = _mm256_and_si256(first, mask_vec);
            
            __m256i cmp = _mm256_cmpeq_epi8(masked_data, masked_needle);
            int matches = _mm256_movemask_epi8(cmp);
            if (matches) {
                int pos = __builtin_ctz(matches);
                return i + pos;
            }
        }
        
        for (size_t i = (haystack_len / 32) * 32; i < haystack_len; i++) {
            uint8_t hay_byte = mask ? (haystack[i] & mask[0]) : haystack[i];
            uint8_t needle_byte = mask ? (needle[0] & mask[0]) : needle[0];
            if (hay_byte == needle_byte) {
                return i;
            }
        }
        return SIZE_MAX;
    }
    
    return search_generic(haystack, haystack_len, needle, needle_len, mask);
}
#else
static inline size_t search_avx2(const uint8_t* haystack, size_t haystack_len,
                                 const uint8_t* needle, size_t needle_len,
                                 const uint8_t* mask) {
    return search_sse42(haystack, haystack_len, needle, needle_len, mask);
}
#endif

#ifdef __SSE4_2__
static inline size_t search_sse42(const uint8_t* haystack, size_t haystack_len,
                                  const uint8_t* needle, size_t needle_len,
                                  const uint8_t* mask) {
    if (needle_len == 1) {
        const __m128i first = _mm_set1_epi8(needle[0]);
        const __m128i mask_vec = mask ? _mm_set1_epi8(mask[0]) : _mm_set1_epi8(0xFF);
        
        for (size_t i = 0; i <= haystack_len - 16; i += 16) {
            __m128i data = _mm_loadu_si128((__m128i*)(haystack + i));
            __m128i masked_data = _mm_and_si128(data, mask_vec);
            __m128i masked_needle = _mm_and_si128(first, mask_vec);
            
            __m128i cmp = _mm_cmpeq_epi8(masked_data, masked_needle);
            int matches = _mm_movemask_epi8(cmp);
            if (matches) {
                int pos = __builtin_ctz(matches);
                return i + pos;
            }
        }
        
        for (size_t i = (haystack_len / 16) * 16; i < haystack_len; i++) {
            uint8_t hay_byte = mask ? (haystack[i] & mask[0]) : haystack[i];
            uint8_t needle_byte = mask ? (needle[0] & mask[0]) : needle[0];
            if (hay_byte == needle_byte) {
                return i;
            }
        }
        return SIZE_MAX;
    }
    
    return search_generic(haystack, haystack_len, needle, needle_len, mask);
}
#else
static inline size_t search_sse42(const uint8_t* haystack, size_t haystack_len,
                                  const uint8_t* needle, size_t needle_len,
                                  const uint8_t* mask) {
    return search_generic(haystack, haystack_len, needle, needle_len, mask);
}
#endif

static inline size_t search_generic(const uint8_t* haystack, size_t haystack_len,
                                    const uint8_t* needle, size_t needle_len,
                                    const uint8_t* mask) {
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        bool found = true;
        for (size_t j = 0; j < needle_len; j++) {
            uint8_t hay_byte = mask ? (haystack[i + j] & mask[j]) : haystack[i + j];
            uint8_t needle_byte = mask ? (needle[j] & mask[j]) : needle[j];
            if (hay_byte != needle_byte) {
                found = false;
                break;
            }
        }
        if (found) return i;
    }
    return SIZE_MAX;
}

static fp_cpu_features_t g_features = 0;
static bool g_features_detected = false;
static char g_cpu_info[256] = {0};

static void init_cpu_features(void) {
    if (!g_features_detected) {
        g_features = fp_detect_cpu_features();
        g_features_detected = true;
        
        strcpy(g_cpu_info, "CPU Features: ");
        if (g_features & FP_CPU_AVX512BW) strcat(g_cpu_info, "AVX-512BW ");
        if (g_features & FP_CPU_AVX512F) strcat(g_cpu_info, "AVX-512F ");
        if (g_features & FP_CPU_AVX2) strcat(g_cpu_info, "AVX2 ");
        if (g_features & FP_CPU_SSE42) strcat(g_cpu_info, "SSE4.2 ");
        if (g_features & FP_CPU_SSE2) strcat(g_cpu_info, "SSE2 ");
        
        if (strlen(g_cpu_info) == strlen("CPU Features: ")) {
            strcat(g_cpu_info, "Generic (no SIMD)");
        }
    }
}

static int parse_pattern(const char* pattern_str, uint8_t** pattern, uint8_t** mask, size_t* length) {
    size_t str_len = strlen(pattern_str);
    size_t max_bytes = (str_len + 1) / 3;
    
    *pattern = malloc(max_bytes);
    *mask = malloc(max_bytes);
    if (!*pattern || !*mask) {
        free(*pattern);
        free(*mask);
        return 0;
    }
    
    size_t byte_count = 0;
    const char* p = pattern_str;
    
    while (*p && byte_count < max_bytes) {
        while (*p == ' ') p++;
        if (!*p) break;
        
        if (*p == '?') {
            (*pattern)[byte_count] = 0x00;
            (*mask)[byte_count] = 0x00;
            p++;
            if (*p == '?') p++;
        } else {
            char hex_str[3] = {0};
            hex_str[0] = *p++;
            if (*p && *p != ' ') hex_str[1] = *p++;
            
            unsigned int byte_val;
            if (sscanf(hex_str, "%x", &byte_val) == 1) {
                (*pattern)[byte_count] = (uint8_t)byte_val;
                (*mask)[byte_count] = 0xFF;
            } else {
                free(*pattern);
                free(*mask);
                return 0;
            }
        }
        byte_count++;
    }
    
    *length = byte_count;
    return 1;
}

size_t fp_find(const uint8_t* data, size_t data_size,
               const uint8_t* pattern, size_t pattern_size,
               const uint8_t* mask) {
    if (!data || !pattern || pattern_size == 0 || data_size < pattern_size) {
        return SIZE_MAX;
    }
    
    if (__builtin_expect(!g_features_detected, 0)) {
        init_cpu_features();
    }
    
    return fp_search_single(data, data_size, pattern, pattern_size, mask, g_features);
}

size_t fp_find_pattern(const uint8_t* data, size_t data_size, const char* pattern_str) {
    uint8_t* pattern = NULL;
    uint8_t* mask = NULL;
    size_t pattern_len = 0;
    
    if (!parse_pattern(pattern_str, &pattern, &mask, &pattern_len)) {
        return SIZE_MAX;
    }
    
    size_t result = fp_find(data, data_size, pattern, pattern_len, mask);
    
    free(pattern);
    free(mask);
    return result;
}

const char* fp_ver(void) {
    return "FastPattern 1.0 - High-performance SIMD pattern matching";
}

const char* fp_cpuinfo(void) {
    init_cpu_features();
    return g_cpu_info;
}