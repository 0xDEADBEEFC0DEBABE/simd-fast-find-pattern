#include "fastpattern.h"

#include <stdlib.h>
#include <string.h>

#ifndef FP_MAX_SIMD_BITS
#define FP_MAX_SIMD_BITS 512
#endif

#if !defined(FP_DISABLE_SIMD) && (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
#define FP_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(__GNUC__) || defined(__clang__)
#define FP_TARGET(isa) __attribute__((target(isa), noinline))
#define FP_WIDE_X86 1
#else
#define FP_TARGET(isa) __declspec(noinline)
#endif
#elif !defined(FP_DISABLE_SIMD) && defined(__aarch64__) && !defined(_MSC_VER)
#define FP_NEON 1
#include <arm_neon.h>
#endif

/* Only this integer is shared mutable state; no dependent data is published. */
enum fp_backend { FP_SCALAR = 1, FP_SSE2, FP_AVX2, FP_AVX512, FP_ARM_NEON };

#if defined(FP_X86)
static void fp_cpuid(unsigned int leaf, unsigned int subleaf, unsigned int out[4]) {
#if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, (int)leaf, (int)subleaf);
    out[0] = (unsigned int)regs[0]; out[1] = (unsigned int)regs[1];
    out[2] = (unsigned int)regs[2]; out[3] = (unsigned int)regs[3];
#else
    __cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
#endif
}

#if defined(FP_WIDE_X86)
FP_TARGET("xsave") static uint64_t fp_xgetbv(void) {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned int low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return ((uint64_t)high << 32) | low;
#endif
}
#endif

static int fp_detect_backend(void) {
    unsigned int regs[4], max_leaf;
    int backend = FP_SCALAR;
    fp_cpuid(0, 0, regs);
    max_leaf = regs[0];
    if (max_leaf < 1) return backend;
    fp_cpuid(1, 0, regs);
    if ((regs[3] & (1u << 26)) && FP_MAX_SIMD_BITS >= 128) backend = FP_SSE2;
#if defined(FP_WIDE_X86)
    /* XGETBV itself requires OSXSAVE; AVX additionally needs XMM/YMM state. */
    if (max_leaf >= 7 && FP_MAX_SIMD_BITS >= 256 &&
        (regs[2] & (7u << 26)) == (7u << 26)) {
        const uint64_t xcr0 = fp_xgetbv();
        if ((xcr0 & 6u) == 6u) {
            fp_cpuid(7, 0, regs);
            if (regs[1] & (1u << 5)) {
                backend = FP_AVX2;
                if (FP_MAX_SIMD_BITS >= 512 && (xcr0 & 0xe6u) == 0xe6u &&
                    (regs[1] & ((1u << 16) | (1u << 30))) == ((1u << 16) | (1u << 30))) {
                    backend = FP_AVX512;
                }
            }
        }
    }
#endif
    return backend;
}
#endif

static int fp_get_backend(void) {
#if defined(FP_X86)
#if defined(_MSC_VER)
    static volatile long cached = 0;
    int backend = (int)_InterlockedCompareExchange(&cached, 0, 0);
    if (!backend) {
        backend = fp_detect_backend();
        _InterlockedExchange(&cached, (long)backend);
    }
#else
    static int cached = 0;
    int backend = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (!backend) {
        backend = fp_detect_backend();
        __atomic_store_n(&cached, backend, __ATOMIC_RELAXED);
    }
#endif
    return backend;
#elif defined(FP_NEON)
    return FP_MAX_SIMD_BITS >= 128 ? FP_ARM_NEON : FP_SCALAR;
#else
    return FP_SCALAR;
#endif
}

typedef struct {
    const uint8_t* bytes;
    const uint8_t* mask;
    size_t length;
    size_t first;
    size_t last;
    int wildcard_only;
} fp_plan;

struct fp_pattern { fp_plan plan; };

static void fp_prepare(fp_plan* plan, const uint8_t* bytes, size_t length,
                       const uint8_t* mask) {
    size_t first = 0, last = length - 1;
    plan->bytes = bytes;
    plan->length = length;
    plan->wildcard_only = 0;
    if (mask) {
        int exact = 1;
        size_t j;
        for (j = 0; j < length; ++j) {
            if (mask[j] != 0xff) { exact = 0; break; }
        }
        if (exact) mask = NULL;
    }
    plan->mask = mask;
    if (mask) {
        while (first < length && mask[first] == 0) ++first;
        if (first == length) {
            plan->wildcard_only = 1;
            plan->first = plan->last = 0;
            return;
        }
        while (last > first && mask[last] == 0) --last;
    }
    plan->first = first;
    plan->last = last;
}

static int fp_matches(const uint8_t* data, const fp_plan* plan) {
    size_t j;
    if (!plan->mask) return memcmp(data, plan->bytes, plan->length) == 0;
    for (j = plan->first; j <= plan->last; ++j) {
        if (((data[j] ^ plan->bytes[j]) & plan->mask[j]) != 0) return 0;
    }
    return 1;
}

static size_t fp_scalar(const uint8_t* data, size_t count, const fp_plan* plan) {
    const size_t first = plan->first, last = plan->last;
    const uint8_t fm = plan->mask ? plan->mask[first] : 0xff;
    const uint8_t lm = plan->mask ? plan->mask[last] : 0xff;
    /* Prefer a constrained suffix: common prefixes otherwise cause one memchr
     * call per candidate on repetitive input. Keep the scanned length bounded
     * by valid starts, rather than by the remainder of the haystack. */
    const size_t anchor = lm == 0xff ? last : first;
    const uint8_t anchor_mask = lm == 0xff ? lm : fm;
    size_t i = 0;
    while (i < count) {
        if (anchor_mask == 0xff) {
            const uint8_t* found = (const uint8_t*)memchr(data + i + anchor, plan->bytes[anchor], count - i);
            if (!found) return SIZE_MAX;
            i = (size_t)(found - (data + anchor));
        }
        if (((data[i + first] ^ plan->bytes[first]) & fm) == 0 &&
            ((data[i + last] ^ plan->bytes[last]) & lm) == 0 &&
            fp_matches(data + i, plan)) return i;
        ++i;
    }
    return SIZE_MAX;
}

#if defined(FP_X86)
static unsigned int fp_first_bit(uint64_t bits) {
#if defined(_MSC_VER)
    unsigned long index;
    /* SSE2 is the only native MSVC backend; clang-cl can use 64-bit masks. */
#if defined(_M_X64)
    _BitScanForward64(&index, bits);
#else
    if ((uint32_t)bits) _BitScanForward(&index, (unsigned long)bits);
    else { _BitScanForward(&index, (unsigned long)(bits >> 32)); index += 32; }
#endif
    return (unsigned int)index;
#else
    return (unsigned int)__builtin_ctzll(bits);
#endif
}

/* Each SIMD lane represents a valid pattern start. Bounding by candidate
 * count guarantees BOTH anchor loads fit, even for buffers shorter than a
 * vector, leading/trailing wildcards, and the final possible match. */
#define FP_DEFINE_X86_SEARCH(NAME, ATTR, VECTOR, WIDTH, SET, LOAD, AND, EQMASK) \
FP_TARGET(ATTR) static size_t NAME(const uint8_t* data, size_t count, const fp_plan* plan) { \
    const size_t first = plan->first, last = plan->last; \
    const uint8_t fm = plan->mask ? plan->mask[first] : 0xff; \
    const uint8_t lm = plan->mask ? plan->mask[last] : 0xff; \
    const VECTOR fmask = SET((char)fm), lmask = SET((char)lm); \
    const VECTOR fbyte = SET((char)(plan->bytes[first] & fm)); \
    const VECTOR lbyte = SET((char)(plan->bytes[last] & lm)); \
    size_t i = 0; \
    for (; count - i >= WIDTH; i += WIDTH) { \
        const VECTOR f = AND(LOAD((const VECTOR*)(const void*)(data + i + first)), fmask); \
        const VECTOR l = AND(LOAD((const VECTOR*)(const void*)(data + i + last)), lmask); \
        uint64_t matches = (uint64_t)EQMASK(f, fbyte) & (uint64_t)EQMASK(l, lbyte); \
        while (matches) { \
            const size_t pos = i + fp_first_bit(matches); \
            if (fp_matches(data + pos, plan)) return pos; \
            matches &= matches - 1; \
        } \
    } \
    { \
        const size_t tail = fp_scalar(data + i, count - i, plan); \
        return tail == SIZE_MAX ? SIZE_MAX : i + tail; \
    } \
}
#define FP_SSE_MASK(a, b) ((unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8((a), (b))))
FP_DEFINE_X86_SEARCH(fp_sse2, "sse2", __m128i, 16, _mm_set1_epi8,
                     _mm_loadu_si128, _mm_and_si128, FP_SSE_MASK)
#if defined(FP_WIDE_X86)
#define FP_AVX_MASK(a, b) ((unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8((a), (b))))
FP_DEFINE_X86_SEARCH(fp_avx2, "avx2", __m256i, 32, _mm256_set1_epi8,
                     _mm256_loadu_si256, _mm256_and_si256, FP_AVX_MASK)
FP_DEFINE_X86_SEARCH(fp_avx512, "avx512f,avx512bw", __m512i, 64, _mm512_set1_epi8,
                     _mm512_loadu_si512, _mm512_and_si512, _mm512_cmpeq_epi8_mask)
#endif
#undef FP_DEFINE_X86_SEARCH
#endif

#if defined(FP_NEON)
static size_t fp_neon(const uint8_t* data, size_t count, const fp_plan* plan) {
    const size_t first = plan->first, last = plan->last;
    const uint8_t fm = plan->mask ? plan->mask[first] : 0xff;
    const uint8_t lm = plan->mask ? plan->mask[last] : 0xff;
    const uint8x16_t fmask = vdupq_n_u8(fm), lmask = vdupq_n_u8(lm);
    const uint8x16_t fbyte = vdupq_n_u8(plan->bytes[first] & fm);
    const uint8x16_t lbyte = vdupq_n_u8(plan->bytes[last] & lm);
    size_t i = 0;
    for (; count - i >= 16; i += 16) {
        const uint8x16_t f = vandq_u8(vld1q_u8(data + i + first), fmask);
        const uint8x16_t l = vandq_u8(vld1q_u8(data + i + last), lmask);
        const uint8x16_t matches = vandq_u8(vceqq_u8(f, fbyte), vceqq_u8(l, lbyte));
        if (vmaxvq_u8(matches)) {
            uint8_t lanes[16];
            size_t j;
            vst1q_u8(lanes, matches);
            for (j = 0; j < 16; ++j) {
                if (lanes[j] && fp_matches(data + i + j, plan)) return i + j;
            }
        }
    }
    {
        const size_t tail = fp_scalar(data + i, count - i, plan);
        return tail == SIZE_MAX ? SIZE_MAX : i + tail;
    }
}
#endif

static size_t fp_search(const uint8_t* data, size_t size, const fp_plan* plan) {
    const size_t count = size - plan->length + 1;
    if (plan->wildcard_only) return 0;
    if (count == 1) return fp_matches(data, plan) ? 0 : SIZE_MAX;
    if (!plan->mask && plan->length == 1) {
        const uint8_t* found = (const uint8_t*)memchr(data, plan->bytes[0], size);
        return found ? (size_t)(found - data) : SIZE_MAX;
    }
#if defined(FP_X86)
    if (count >= 16) {
        const int backend = fp_get_backend();
#if defined(FP_WIDE_X86)
        if (backend >= FP_AVX512 && count >= 64) return fp_avx512(data, count, plan);
        if (backend >= FP_AVX2 && count >= 32) return fp_avx2(data, count, plan);
#endif
        if (backend >= FP_SSE2) return fp_sse2(data, count, plan);
    }
#elif defined(FP_NEON)
    if (FP_MAX_SIMD_BITS >= 128 && count >= 16) return fp_neon(data, count, plan);
#endif
    return fp_scalar(data, count, plan);
}

size_t fp_find(const uint8_t* data, size_t data_size,
               const uint8_t* pattern, size_t pattern_size,
               const uint8_t* mask) {
    fp_plan plan;
    if (!data || !pattern || !pattern_size || data_size < pattern_size) return SIZE_MAX;
    /* Avoid preprocessing for anchored-size comparisons and exact bytes. */
    if (data_size == pattern_size) {
        size_t j;
        if (!mask) return memcmp(data, pattern, pattern_size) == 0 ? 0 : SIZE_MAX;
        for (j = 0; j < pattern_size; ++j) {
            if (((data[j] ^ pattern[j]) & mask[j]) != 0) return SIZE_MAX;
        }
        return 0;
    }
    fp_prepare(&plan, pattern, pattern_size, mask);
    return fp_search(data, data_size, &plan);
}

static int fp_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int fp_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Two passes: validate/count tokens, then fill exactly sized storage. */
static size_t fp_parse(const char* text, uint8_t* bytes, uint8_t* mask) {
    const unsigned char* p = (const unsigned char*)text;
    size_t count = 0;
    if (!p) return 0;
    while (*p) {
        int high, low;
        uint8_t byte = 0, bits = 0;
        while (fp_space(*p)) ++p;
        if (!*p) break;
        if (*p == '?' && (!p[1] || fp_space(p[1]))) {
            ++p;
        } else {
            high = *p == '?' ? 0 : fp_hex(*p);
            if (high < 0 || !p[1]) return 0;
            if (*p != '?') bits = 0xf0;
            ++p;
            low = *p == '?' ? 0 : fp_hex(*p);
            if (low < 0) return 0;
            if (*p != '?') bits |= 0x0f;
            byte = (uint8_t)((high << 4) | low);
            ++p;
        }
        if (*p && !fp_space(*p)) return 0;
        if (count == SIZE_MAX) return 0;
        if (bytes) { bytes[count] = byte; mask[count] = bits; }
        ++count;
    }
    return count;
}

fp_pattern* fp_compile(const char* pattern_str) {
    const size_t length = fp_parse(pattern_str, NULL, NULL);
    fp_pattern* pattern;
    uint8_t* bytes;
    if (!length || length > (SIZE_MAX - sizeof(fp_pattern)) / 2) return NULL;
    pattern = (fp_pattern*)malloc(sizeof(fp_pattern) + 2 * length);
    if (!pattern) return NULL;
    bytes = (uint8_t*)(pattern + 1);
    (void)fp_parse(pattern_str, bytes, bytes + length);
    fp_prepare(&pattern->plan, bytes, length, bytes + length);
    return pattern;
}

size_t fp_find_compiled(const uint8_t* data, size_t data_size, const fp_pattern* pattern) {
    if (!data || !pattern || data_size < pattern->plan.length) return SIZE_MAX;
    return fp_search(data, data_size, &pattern->plan);
}

void fp_free(fp_pattern* pattern) { free(pattern); }

size_t fp_find_pattern(const uint8_t* data, size_t data_size, const char* pattern_str) {
    uint8_t bytes[256], mask[256];
    size_t result;
    const size_t length = fp_parse(pattern_str, NULL, NULL);
    if (!data || !length || data_size < length) return SIZE_MAX;
    if (length <= sizeof(bytes)) {
        (void)fp_parse(pattern_str, bytes, mask);
        return fp_find(data, data_size, bytes, length, mask);
    } else {
        fp_pattern* pattern = fp_compile(pattern_str);
        if (!pattern) return SIZE_MAX;
        result = fp_find_compiled(data, data_size, pattern);
        fp_free(pattern);
        return result;
    }
}

const char* fp_ver(void) { return "FastPattern " FASTPATTERN_VERSION; }

const char* fp_cpuinfo(void) {
    switch (fp_get_backend()) {
        case FP_AVX512: return "CPU backend: AVX-512BW";
        case FP_AVX2: return "CPU backend: AVX2";
        case FP_SSE2: return "CPU backend: SSE2";
        case FP_ARM_NEON: return "CPU backend: NEON";
        default: return "CPU backend: scalar";
    }
}
