#include "fastpattern.h"

#include <stdlib.h>
#include <string.h>

#ifndef FP_MAX_SIMD_BITS
#define FP_MAX_SIMD_BITS 512
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FP_INLINE static inline __attribute__((always_inline))
#define FP_NOINLINE static __attribute__((noinline))
#elif defined(_MSC_VER)
#define FP_INLINE static __forceinline
#define FP_NOINLINE static __declspec(noinline)
#else
#define FP_INLINE static
#define FP_NOINLINE static
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
#define FP_TARGET_INLINE(isa) inline __attribute__((target(isa), always_inline))
#define FP_WIDE_X86 1
#else
#define FP_TARGET(isa) __declspec(noinline)
#define FP_TARGET_INLINE(isa) __forceinline
#endif
#elif !defined(FP_DISABLE_SIMD) && (defined(__aarch64__) || defined(_M_ARM64))
#define FP_NEON 1
#include <arm_neon.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#if defined(FP_X86) && FP_MAX_SIMD_BITS >= 128 && \
    (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
/* SSE2 is part of the target baseline, so SSE2 code can inline without dispatch. */
#define FP_SSE2_BASELINE 1
#endif

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

#if defined(FP_X86)
/* Only this integer is shared mutable state; no dependent data is published.
 * It is read on every search, so the read must not be a locked instruction. */
#if defined(__GNUC__) || defined(__clang__)
static int fp_cached_backend = 0;
static int fp_known_backend(void) { return __atomic_load_n(&fp_cached_backend, __ATOMIC_RELAXED); }
static void fp_store_backend(int backend) { __atomic_store_n(&fp_cached_backend, backend, __ATOMIC_RELAXED); }
#else
/* An aligned volatile long read is a plain atomic load on MSVC x86/x64. */
static volatile long fp_cached_backend = 0;
static int fp_known_backend(void) { return (int)fp_cached_backend; }
static void fp_store_backend(int backend) { _InterlockedExchange(&fp_cached_backend, (long)backend); }
#endif
#endif

static int fp_get_backend(void) {
#if defined(FP_X86)
    int backend = fp_known_backend();
    if (!backend) {
        backend = fp_detect_backend();
        fp_store_backend(backend);
    }
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
    size_t second;
    int wildcard_only;
} fp_plan;

struct fp_pattern { fp_plan plan; };

FP_INLINE void fp_prepare(fp_plan* plan, const uint8_t* bytes, size_t length,
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
            plan->first = plan->last = plan->second = 0;
            return;
        }
        while (last > first && mask[last] == 0) --last;
    }
    plan->first = first;
    plan->last = last;
    plan->second = last;
    /* Filters test the first constrained byte and a second anchor. Identical
     * exact anchors pass every offset in a run of that byte, such as zero or
     * 0xcc padding, so pair the first byte with the last exact byte that
     * differs from it, when there is one. */
    if (last > first && bytes[first] == bytes[last] &&
        (!mask || (mask[first] == 0xff && mask[last] == 0xff))) {
        size_t j;
        for (j = last - 1; j > first; --j) {
            if ((!mask || mask[j] == 0xff) && bytes[j] != bytes[first]) {
                plan->second = j;
                break;
            }
        }
    }
}

static uint64_t fp_load64(const uint8_t* p) { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }
static uint32_t fp_load32(const uint8_t* p) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }

/* Compares n bytes under mask m (NULL: exact) as overlapping words, which
 * keeps every read inside the n bytes. Inlined, a failing candidate costs a
 * few instructions rather than a memcmp call. */
FP_INLINE int fp_equal(const uint8_t* a, const uint8_t* b, const uint8_t* m, size_t n) {
    size_t j;
    if (n >= 8) {
        for (j = 0; n - j > 8; j += 8) {
            if ((fp_load64(a + j) ^ fp_load64(b + j)) & (m ? fp_load64(m + j) : ~UINT64_C(0))) return 0;
        }
        j = n - 8;
        return ((fp_load64(a + j) ^ fp_load64(b + j)) & (m ? fp_load64(m + j) : ~UINT64_C(0))) == 0;
    }
    if (n >= 4) {
        j = n - 4;
        return (((fp_load32(a) ^ fp_load32(b)) & (m ? fp_load32(m) : ~UINT32_C(0))) |
                ((fp_load32(a + j) ^ fp_load32(b + j)) & (m ? fp_load32(m + j) : ~UINT32_C(0)))) == 0;
    }
    for (j = 0; j < n; ++j) {
        if ((a[j] ^ b[j]) & (m ? m[j] : 0xff)) return 0;
    }
    return 1;
}

FP_INLINE int fp_matches(const uint8_t* data, const fp_plan* plan) {
    if (!plan->mask) return fp_equal(data, plan->bytes, NULL, plan->length);
    return fp_equal(data + plan->first, plan->bytes + plan->first, plan->mask + plan->first,
                    plan->last - plan->first + 1);
}

static size_t fp_scalar(const uint8_t* data, size_t count, const fp_plan* plan) {
    const size_t first = plan->first, second = plan->second;
    const uint8_t fm = plan->mask ? plan->mask[first] : 0xff;
    const uint8_t sm = plan->mask ? plan->mask[second] : 0xff;
    /* Prefer the second anchor: common prefixes otherwise cause one memchr
     * call per candidate on repetitive input. Keep the scanned length bounded
     * by valid starts, rather than by the remainder of the haystack, and test
     * the last few starts directly, where a call costs more than it saves. */
    const size_t anchor = sm == 0xff ? second : first;
    const uint8_t anchor_mask = sm == 0xff ? sm : fm;
    size_t i = 0;
    while (i < count) {
        if (anchor_mask == 0xff && count - i >= 16) {
            const uint8_t* found = (const uint8_t*)memchr(data + i + anchor, plan->bytes[anchor], count - i);
            if (!found) return SIZE_MAX;
            i = (size_t)(found - (data + anchor));
        }
        if (((data[i + first] ^ plan->bytes[first]) & fm) == 0 &&
            ((data[i + second] ^ plan->bytes[second]) & sm) == 0 &&
            fp_matches(data + i, plan)) return i;
        ++i;
    }
    return SIZE_MAX;
}

#if defined(FP_X86) || defined(FP_NEON)
static unsigned int fp_first_bit(uint64_t bits) {
#if defined(_MSC_VER)
    unsigned long index;
    /* 32-bit x86 has no _BitScanForward64; clang-cl can use 64-bit masks. */
#if defined(_M_X64) || defined(_M_ARM64)
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
#endif

#if defined(FP_X86)
/* Verifies candidate bits of the block at base, lowest offset first. */
FP_INLINE size_t fp_verify(const uint8_t* data, size_t base, uint64_t bits, const fp_plan* plan) {
    for (; bits; bits &= bits - 1) {
        const size_t pos = base + fp_first_bit(bits);
        if (fp_matches(data + pos, plan)) return pos;
    }
    return SIZE_MAX;
}

FP_INLINE size_t fp_verify_exact(const uint8_t* data, size_t base, uint64_t bits,
                                 const uint8_t* pattern, size_t length) {
    for (; bits; bits &= bits - 1) {
        const size_t pos = base + fp_first_bit(bits);
        if (fp_equal(data + pos, pattern, NULL, length)) return pos;
    }
    return SIZE_MAX;
}

/* fp_verify_exact for patterns of 8 to 16 bytes: the first and last 8 bytes
 * cover the pattern and compare as two words loaded once, with no branches
 * on the length. This keeps the verification small enough that the short
 * exact paths need few saved registers. */
FP_INLINE size_t fp_verify_exact8(const uint8_t* data, size_t base, uint64_t bits,
                                  const uint8_t* pattern, size_t length) {
    const uint64_t head = fp_load64(pattern), tail = fp_load64(pattern + length - 8);
    for (; bits; bits &= bits - 1) {
        const uint8_t* q = data + base + fp_first_bit(bits);
        if (fp_load64(q) == head && fp_load64(q + length - 8) == tail) return (size_t)(q - data);
    }
    return SIZE_MAX;
}

/* Exact search of a buffer shorter than any vector. */
FP_NOINLINE size_t fp_exact_small(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) {
    const size_t count = size - length + 1;
    size_t pos;
    for (pos = 0; pos < count; ++pos) {
        if (data[pos] == pattern[0] && fp_equal(data + pos, pattern, NULL, length)) return pos;
    }
    return SIZE_MAX;
}

/* Each SIMD lane represents a valid pattern start; MATCH sets the lanes that
 * pass both anchor tests. Bounding blocks by candidate count guarantees every
 * load fits, including leading/trailing wildcards and the final possible
 * match. NAME scans candidates [i, count) and requires count >= WIDTH.
 *
 * NAME_pair narrows the candidates of a block (lo) and of the block after it
 * (hi, or zero): while either holds two or more, the next pattern byte is
 * tested across all lanes of both at once, so dense input costs a few vector
 * operations per byte rather than one comparison per candidate. The
 * survivors are verified one by one. SSE2's 16 lanes do not repay the
 * broadcasts for 8- to 16-byte exact patterns, which fp_verify_exact8
 * checks directly instead. The unrolled scan loops break out to
 * verification and then resume, so they stay free of calls and spills:
 * Win64 does not preserve ymm/zmm upper halves, so a call would make the
 * compiler reload the broadcasts on every iteration. The final block ends
 * exactly at the last candidate and masks lanes already scanned, so no
 * scalar tail is needed. NAME_long plans an exact pattern and scans it. */
#define FP_DEFINE_X86_SEARCH(NAME, ATTR, VECTOR, LANES, WIDTH, SET, LOAD, AND, EQ, MATCH, ANY, BITS) \
FP_TARGET_INLINE(ATTR) static size_t NAME##_pair(const uint8_t* data, size_t base, uint64_t lo, uint64_t hi, \
                                                 const fp_plan* plan) { \
    size_t j = plan->first, pos; \
    if (WIDTH == 16 && !plan->mask && plan->length - 8 <= 8) { \
        pos = fp_verify_exact8(data, base, lo, plan->bytes, plan->length); \
        return pos != SIZE_MAX ? pos : fp_verify_exact8(data, base + WIDTH, hi, plan->bytes, plan->length); \
    } \
    if (!plan->mask) { \
        while (((lo & (lo - 1)) | (hi & (hi - 1))) && ++j <= plan->last) { \
            const VECTOR v = SET((char)plan->bytes[j]); \
            lo &= EQ(LOAD(data + base + j), v); \
            if (hi) hi &= EQ(LOAD(data + base + WIDTH + j), v); \
        } \
    } else { \
        while (((lo & (lo - 1)) | (hi & (hi - 1))) && ++j <= plan->last) { \
            const uint8_t m = plan->mask[j]; \
            if (m) { \
                const VECTOR k = SET((char)m), v = SET((char)(plan->bytes[j] & m)); \
                lo &= EQ(AND(LOAD(data + base + j), k), v); \
                if (hi) hi &= EQ(AND(LOAD(data + base + WIDTH + j), k), v); \
            } \
        } \
    } \
    pos = fp_verify(data, base, lo, plan); \
    return pos != SIZE_MAX ? pos : fp_verify(data, base + WIDTH, hi, plan); \
} \
FP_TARGET_INLINE(ATTR) static size_t NAME##_scan(const uint8_t* data, size_t count, size_t i, \
                                                 const fp_plan* plan) { \
    const size_t a = plan->first, b = plan->second; \
    const uint8_t am = plan->mask ? plan->mask[a] : 0xff; \
    const uint8_t bm = plan->mask ? plan->mask[b] : 0xff; \
    const VECTOR ak = SET((char)am), bk = SET((char)bm); \
    const VECTOR av = SET((char)(plan->bytes[a] & am)), bv = SET((char)(plan->bytes[b] & bm)); \
    size_t pos, base; \
    for (;;) { \
        uint64_t lo = 0, hi = 0; \
        if ((am & bm) == 0xff) { \
            for (; count - i >= 2 * WIDTH; i += 2 * WIDTH) { \
                const LANES x = MATCH(LOAD(data + i + a), av, LOAD(data + i + b), bv); \
                const LANES y = MATCH(LOAD(data + i + WIDTH + a), av, LOAD(data + i + WIDTH + b), bv); \
                if (ANY(x, y)) { lo = BITS(x); hi = BITS(y); break; } \
            } \
        } else { \
            for (; count - i >= 2 * WIDTH; i += 2 * WIDTH) { \
                const LANES x = MATCH(AND(LOAD(data + i + a), ak), av, AND(LOAD(data + i + b), bk), bv); \
                const LANES y = MATCH(AND(LOAD(data + i + WIDTH + a), ak), av, \
                                      AND(LOAD(data + i + WIDTH + b), bk), bv); \
                if (ANY(x, y)) { lo = BITS(x); hi = BITS(y); break; } \
            } \
        } \
        if (!(lo | hi)) break; \
        if ((pos = NAME##_pair(data, i, lo, hi, plan)) != SIZE_MAX) return pos; \
        i += 2 * WIDTH; \
    } \
    for (; i < count; i = base + WIDTH) { \
        base = count - i >= WIDTH ? i : count - WIDTH; \
        pos = NAME##_pair(data, base, BITS(MATCH(AND(LOAD(data + base + a), ak), av, \
                                                 AND(LOAD(data + base + b), bk), bv)) & \
                                      (~UINT64_C(0) << (i - base)), 0, plan); \
        if (pos != SIZE_MAX) return pos; \
    } \
    return SIZE_MAX; \
} \
FP_TARGET(ATTR) static size_t NAME(const uint8_t* data, size_t count, size_t i, const fp_plan* plan) { \
    return NAME##_scan(data, count, i, plan); \
} \
FP_TARGET(ATTR) static size_t NAME##_long(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                          size_t length) { \
    fp_plan plan; \
    fp_prepare(&plan, pattern, length, NULL); \
    return NAME##_scan(data, size - length + 1, 0, &plan); \
}
#define FP_SSE_LOAD(p) _mm_loadu_si128((const __m128i*)(const void*)(p))
#define FP_SSE_BITS(x) ((uint64_t)(unsigned int)_mm_movemask_epi8(x))
#define FP_SSE_EQ(x, y) FP_SSE_BITS(_mm_cmpeq_epi8((x), (y)))
#define FP_SSE_MATCH(x, xv, y, yv) _mm_and_si128(_mm_cmpeq_epi8((x), (xv)), _mm_cmpeq_epi8((y), (yv)))
#define FP_SSE_ANY(x, y) _mm_movemask_epi8(_mm_or_si128((x), (y)))
/* Exact patterns need no prior planning and are anchored on their first and
 * last bytes. FP_RETURN_EXACT dispatches them by tail call to small leaf
 * functions of two kinds: suffix _w for patterns of 8 to 16 bytes (see
 * fp_verify_exact8), _s for all others. NAME_one handles up to 64 starts,
 * NAME_two up to 128 (SSE2 uses NAME_small), NAME_small up to 1024, and
 * NAME_lead everything else. */
/* Up to 64 exact starts, gathered into one candidate mask before a single
 * verification loop. The last block ends at the last start and overlaps the
 * one before; ORing the same bits twice is harmless. With fewer than WIDTH
 * starts, the last-byte block ends at the buffer end; shifting it down to
 * line up with the first-byte block also clears the lanes past the last
 * start. A buffer shorter than a vector goes to the next narrower width,
 * NARROW. */
#define FP_DEFINE_X86_ONE(NAME, KIND, VERIFY, ATTR, VECTOR, WIDTH, SET, LOAD, EQ, MATCH, BITS, NARROW) \
FP_TARGET_INLINE(ATTR) static size_t NAME##_one_core##KIND(const uint8_t* data, size_t size, \
                                                           const uint8_t* pattern, size_t length) { \
    const size_t count = size - length + 1, last = length - 1; \
    uint64_t bits; \
    size_t j, base; \
    if (size < WIDTH) return NARROW(data, size, pattern, length); \
    if (count < WIDTH) { \
        bits = EQ(LOAD(data), SET((char)pattern[0])) & \
               (EQ(LOAD(data + size - WIDTH), SET((char)pattern[last])) >> (WIDTH - count)); \
    } else { \
        const VECTOR av = SET((char)pattern[0]), bv = SET((char)pattern[last]); \
        base = count - WIDTH; \
        bits = BITS(MATCH(LOAD(data + base), av, LOAD(data + base + last), bv)) << base; \
        for (j = 0; j < base; j += WIDTH) \
            bits |= BITS(MATCH(LOAD(data + j), av, LOAD(data + j + last), bv)) << j; \
    } \
    return VERIFY(data, 0, bits, pattern, length); \
} \
FP_TARGET(ATTR) static size_t NAME##_one##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                               size_t length) { \
    return NAME##_one_core##KIND(data, size, pattern, length); \
}
/* 65 to 128 exact starts in two candidate masks, lo for starts 0-63 from
 * whole blocks and hi for the rest; the last block ends at the last start
 * and may straddle both. */
#define FP_DEFINE_X86_TWO(NAME, KIND, VERIFY, ATTR, VECTOR, WIDTH, SET, LOAD, MATCH, BITS) \
FP_TARGET(ATTR) static size_t NAME##_two##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                               size_t length) { \
    const size_t count = size - length + 1, last = length - 1, base = count - WIDTH; \
    const VECTOR av = SET((char)pattern[0]), bv = SET((char)pattern[last]); \
    uint64_t lo = 0, hi = 0, bits; \
    size_t j, pos; \
    for (j = 0; j < 64; j += WIDTH) lo |= BITS(MATCH(LOAD(data + j), av, LOAD(data + j + last), bv)) << j; \
    for (; j < base; j += WIDTH) hi |= BITS(MATCH(LOAD(data + j), av, LOAD(data + j + last), bv)) << (j - 64); \
    bits = BITS(MATCH(LOAD(data + base), av, LOAD(data + base + last), bv)); \
    hi |= base >= 64 ? bits << (base - 64) : bits >> (64 - base); \
    pos = VERIFY(data, 0, lo, pattern, length); \
    return pos != SIZE_MAX ? pos : VERIFY(data, 64, hi, pattern, length); \
}
/* NAME_small: more than WIDTH exact starts, two blocks per test, the last
 * block overlapping the one before. NAME_lead checks the first WIDTH starts
 * (at least 32) with the one-block code before NAME_long plans the search,
 * so early matches in long buffers skip the planning. */
#define FP_DEFINE_X86_EXACT(NAME, KIND, VERIFY, ATTR, VECTOR, WIDTH, SET, LOAD, MATCH, BITS) \
FP_TARGET(ATTR) static size_t NAME##_small##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                                 size_t length) { \
    const size_t last = length - 1; \
    const uint8_t* const end = data + size - last - WIDTH; \
    const VECTOR av = SET((char)pattern[0]), bv = SET((char)pattern[last]); \
    const uint8_t* p; \
    uint64_t bits, next; \
    size_t pos; \
    for (p = data; p + WIDTH < end; p += 2 * WIDTH) { \
        bits = BITS(MATCH(LOAD(p), av, LOAD(p + last), bv)); \
        next = BITS(MATCH(LOAD(p + WIDTH), av, LOAD(p + WIDTH + last), bv)); \
        if (bits | next) { \
            if ((pos = VERIFY(data, (size_t)(p - data), bits, pattern, length)) != SIZE_MAX) return pos; \
            if ((pos = VERIFY(data, (size_t)(p - data) + WIDTH, next, pattern, length)) != SIZE_MAX) return pos; \
        } \
    } \
    if (p < end) { \
        bits = BITS(MATCH(LOAD(p), av, LOAD(p + last), bv)); \
        if (bits && (pos = VERIFY(data, (size_t)(p - data), bits, pattern, length)) != SIZE_MAX) return pos; \
        p += WIDTH; \
    } \
    bits = BITS(MATCH(LOAD(end), av, LOAD(end + last), bv)) & (~UINT64_C(0) << (p - end)); \
    return VERIFY(data, (size_t)(end - data), bits, pattern, length); \
} \
FP_TARGET(ATTR) static size_t NAME##_lead##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                                size_t length) { \
    const size_t lead = WIDTH < 32 ? 32 : WIDTH; \
    const size_t pos = NAME##_one_core##KIND(data, length + lead - 1, pattern, length); \
    return pos != SIZE_MAX ? pos : NAME##_long(data, size, pattern, length); \
}
FP_DEFINE_X86_SEARCH(fp_sse2, "sse2", __m128i, __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD,
                     _mm_and_si128, FP_SSE_EQ, FP_SSE_MATCH, FP_SSE_ANY, FP_SSE_BITS)
#define FP_DEFINE_SSE2_EXACT(KIND, VERIFY) \
    FP_DEFINE_X86_ONE(fp_sse2, KIND, VERIFY, "sse2", __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD, FP_SSE_EQ, \
                      FP_SSE_MATCH, FP_SSE_BITS, fp_exact_small) \
    FP_DEFINE_X86_EXACT(fp_sse2, KIND, VERIFY, "sse2", __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD, FP_SSE_MATCH, \
                        FP_SSE_BITS)
FP_DEFINE_SSE2_EXACT(_s, fp_verify_exact)
FP_DEFINE_SSE2_EXACT(_w, fp_verify_exact8)
#if defined(FP_WIDE_X86)
#define FP_AVX_LOAD(p) _mm256_loadu_si256((const __m256i*)(const void*)(p))
#define FP_AVX_BITS(x) ((uint64_t)(unsigned int)_mm256_movemask_epi8(x))
#define FP_AVX_EQ(x, y) FP_AVX_BITS(_mm256_cmpeq_epi8((x), (y)))
#define FP_AVX_MATCH(x, xv, y, yv) _mm256_and_si256(_mm256_cmpeq_epi8((x), (xv)), _mm256_cmpeq_epi8((y), (yv)))
#define FP_AVX_ANY(x, y) _mm256_movemask_epi8(_mm256_or_si256((x), (y)))
FP_DEFINE_X86_SEARCH(fp_avx2, "avx2", __m256i, __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD,
                     _mm256_and_si256, FP_AVX_EQ, FP_AVX_MATCH, FP_AVX_ANY, FP_AVX_BITS)
/* fp_avx2x16 uses 16-byte blocks compiled for AVX2 (one-instruction
 * broadcasts, VEX encoding) for buffers shorter than 32 bytes. */
#define FP_DEFINE_AVX2_EXACT(KIND, VERIFY) \
    FP_DEFINE_X86_ONE(fp_avx2x16, KIND, VERIFY, "avx2", __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD, FP_SSE_EQ, \
                      FP_SSE_MATCH, FP_SSE_BITS, fp_exact_small) \
    FP_DEFINE_X86_ONE(fp_avx2, KIND, VERIFY, "avx2", __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD, FP_AVX_EQ, \
                      FP_AVX_MATCH, FP_AVX_BITS, fp_avx2x16_one##KIND) \
    FP_DEFINE_X86_TWO(fp_avx2, KIND, VERIFY, "avx2", __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD, FP_AVX_MATCH, \
                      FP_AVX_BITS) \
    FP_DEFINE_X86_EXACT(fp_avx2, KIND, VERIFY, "avx2", __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD, FP_AVX_MATCH, \
                        FP_AVX_BITS)
FP_DEFINE_AVX2_EXACT(_s, fp_verify_exact)
FP_DEFINE_AVX2_EXACT(_w, fp_verify_exact8)
#define FP_AVX512_LOAD(p) _mm512_loadu_si512((const void*)(p))
#define FP_AVX512_BITS(x) ((uint64_t)(x))
#define FP_AVX512_EQ(x, y) ((uint64_t)_mm512_cmpeq_epi8_mask((x), (y)))
/* Merge masking ANDs the second compare into the first for free. */
#define FP_AVX512_MATCH(x, xv, y, yv) \
    _mm512_mask_cmpeq_epi8_mask(_mm512_cmpeq_epi8_mask((x), (xv)), (y), (yv))
#define FP_AVX512_ANY(x, y) ((x) | (y))
FP_DEFINE_X86_SEARCH(fp_avx512, "avx512f,avx512bw", __m512i, __mmask64, 64, _mm512_set1_epi8,
                     FP_AVX512_LOAD, _mm512_and_si512, FP_AVX512_EQ, FP_AVX512_MATCH,
                     FP_AVX512_ANY, FP_AVX512_BITS)
/* AVX-512BW masked loads do not fault on masked-out lanes, so a block of up
 * to 64 exact starts loads exactly the bytes it needs, at any size: the one
 * block of fp_avx512_one and the second block of fp_avx512_two. */
#define FP_DEFINE_AVX512_EXACT(KIND, VERIFY) \
FP_TARGET_INLINE("avx512f,avx512bw") static size_t fp_avx512_one_core##KIND(const uint8_t* data, size_t size, \
                                                                            const uint8_t* pattern, \
                                                                            size_t length) { \
    const size_t last = length - 1; \
    const __mmask64 lanes = (__mmask64)(~UINT64_C(0) >> (64 - (size - last))); \
    const __m512i x = _mm512_maskz_loadu_epi8(lanes, data), y = _mm512_maskz_loadu_epi8(lanes, data + last); \
    const __mmask64 bits = _mm512_mask_cmpeq_epi8_mask( \
        _mm512_mask_cmpeq_epi8_mask(lanes, x, _mm512_set1_epi8((char)pattern[0])), y, \
        _mm512_set1_epi8((char)pattern[last])); \
    return VERIFY(data, 0, bits, pattern, length); \
} \
FP_TARGET("avx512f,avx512bw") static size_t fp_avx512_one##KIND(const uint8_t* data, size_t size, \
                                                                const uint8_t* pattern, size_t length) { \
    return fp_avx512_one_core##KIND(data, size, pattern, length); \
} \
FP_TARGET("avx512f,avx512bw") static size_t fp_avx512_two##KIND(const uint8_t* data, size_t size, \
                                                                const uint8_t* pattern, size_t length) { \
    const size_t last = length - 1; \
    const __mmask64 more = (__mmask64)(~UINT64_C(0) >> (128 - (size - last))); \
    const __m512i av = _mm512_set1_epi8((char)pattern[0]), bv = _mm512_set1_epi8((char)pattern[last]); \
    const __m512i x = _mm512_maskz_loadu_epi8(more, data + 64); \
    const __m512i y = _mm512_maskz_loadu_epi8(more, data + 64 + last); \
    const uint64_t lo = FP_AVX512_MATCH(FP_AVX512_LOAD(data), av, FP_AVX512_LOAD(data + last), bv); \
    const uint64_t hi = _mm512_mask_cmpeq_epi8_mask(_mm512_mask_cmpeq_epi8_mask(more, x, av), y, bv); \
    const size_t pos = VERIFY(data, 0, lo, pattern, length); \
    return pos != SIZE_MAX ? pos : VERIFY(data, 64, hi, pattern, length); \
} \
FP_DEFINE_X86_EXACT(fp_avx512, KIND, VERIFY, "avx512f,avx512bw", __m512i, 64, _mm512_set1_epi8, FP_AVX512_LOAD, \
                    FP_AVX512_MATCH, FP_AVX512_BITS)
FP_DEFINE_AVX512_EXACT(_s, fp_verify_exact)
FP_DEFINE_AVX512_EXACT(_w, fp_verify_exact8)
#endif
#undef FP_DEFINE_X86_SEARCH

#if defined(FP_SSE2_BASELINE)
FP_INLINE uint64_t fp_sse2_eq(const uint8_t* p, __m128i k, __m128i v) {
    return FP_SSE_EQ(_mm_and_si128(FP_SSE_LOAD(p), k), v);
}

/* Candidate bits of a masked plan for the first min(count, 32) starts;
 * requires size >= 16. This needs no dispatch or call, so short buffers and
 * early matches finish here. With fewer than 16 starts, each anchor's
 * 16-byte load is moved back to end inside the buffer, and its bits are
 * shifted down to lane 0. */
FP_INLINE uint64_t fp_sse2_head(const uint8_t* data, size_t size, size_t count, const fp_plan* plan) {
    const size_t a = plan->first, b = plan->second;
    const __m128i ak = _mm_set1_epi8((char)plan->mask[a]), bk = _mm_set1_epi8((char)plan->mask[b]);
    const __m128i av = _mm_set1_epi8((char)(plan->bytes[a] & plan->mask[a]));
    const __m128i bv = _mm_set1_epi8((char)(plan->bytes[b] & plan->mask[b]));
    if (count >= 16) {
        const size_t next = count < 32 ? count - 16 : 16;
        return (fp_sse2_eq(data + a, ak, av) & fp_sse2_eq(data + b, bk, bv)) |
               (fp_sse2_eq(data + next + a, ak, av) & fp_sse2_eq(data + next + b, bk, bv)) << next;
    } else {
        const size_t ao = a < size - 16 ? a : size - 16, bo = b < size - 16 ? b : size - 16;
        return (fp_sse2_eq(data + ao, ak, av) >> (a - ao)) & (fp_sse2_eq(data + bo, bk, bv) >> (b - bo)) &
               ((UINT64_C(1) << count) - 1);
    }
}
#endif
#endif

#if defined(FP_X86)
/* Returns from the enclosing function with the widest available exact
 * search (see FP_DEFINE_X86_EXACT); falls through when the backend has no
 * SIMD. The pattern kind and the number of starts are tested first, the
 * backend last, so whichever backend is in use reaches its leaf with one
 * taken branch. BACKEND is a local, evaluated more than once. */
#if defined(FP_WIDE_X86)
#define FP_RETURN_LEAF(backend, LEAF, data, size, pattern, length) do { \
        if ((backend) >= FP_AVX512) return fp_avx512_##LEAF(data, size, pattern, length); \
        if ((backend) >= FP_AVX2) return fp_avx2_##LEAF(data, size, pattern, length); \
        if ((backend) >= FP_SSE2) return fp_sse2_##LEAF(data, size, pattern, length); \
    } while (0)
/* Buffers shorter than 32 bytes take AVX2's 16-byte leaf directly. */
#define FP_RETURN_ONE(backend, LEAF, data, size, pattern, length) do { \
        if ((backend) >= FP_AVX512) return fp_avx512_##LEAF(data, size, pattern, length); \
        if ((backend) >= FP_AVX2) return (size) < 32 ? fp_avx2x16_##LEAF(data, size, pattern, length) \
                                                     : fp_avx2_##LEAF(data, size, pattern, length); \
        if ((backend) >= FP_SSE2) return fp_sse2_##LEAF(data, size, pattern, length); \
    } while (0)
/* For 65 to 128 starts, SSE2's loop beats merging eight blocks into masks. */
#define FP_RETURN_TWO(backend, KIND, data, size, pattern, length) do { \
        if ((backend) >= FP_AVX512) return fp_avx512_two##KIND(data, size, pattern, length); \
        if ((backend) >= FP_AVX2) return fp_avx2_two##KIND(data, size, pattern, length); \
        if ((backend) >= FP_SSE2) return fp_sse2_small##KIND(data, size, pattern, length); \
    } while (0)
#else
#define FP_RETURN_LEAF(backend, LEAF, data, size, pattern, length) do { \
        if ((backend) >= FP_SSE2) return fp_sse2_##LEAF(data, size, pattern, length); \
    } while (0)
#define FP_RETURN_ONE FP_RETURN_LEAF
#define FP_RETURN_TWO(backend, KIND, data, size, pattern, length) do { \
        if ((backend) >= FP_SSE2) return fp_sse2_small##KIND(data, size, pattern, length); \
    } while (0)
#endif
#define FP_RETURN_EXACT_KIND(backend, KIND, data, size, pattern, length) do { \
        const size_t fp_starts = (size) - (length); \
        if (fp_starts < 64) FP_RETURN_ONE(backend, one##KIND, data, size, pattern, length); \
        else if (fp_starts < 128) FP_RETURN_TWO(backend, KIND, data, size, pattern, length); \
        else if (fp_starts < 1024) FP_RETURN_LEAF(backend, small##KIND, data, size, pattern, length); \
        else FP_RETURN_LEAF(backend, lead##KIND, data, size, pattern, length); \
    } while (0)
#define FP_RETURN_EXACT(backend, data, size, pattern, length) do { \
        if ((length) - 8 <= 8) FP_RETURN_EXACT_KIND(backend, _w, data, size, pattern, length); \
        else FP_RETURN_EXACT_KIND(backend, _s, data, size, pattern, length); \
    } while (0)
#endif

#if defined(FP_NEON)
/* SHRN #4 narrows each 16-bit pair of 0x00/0xff compare bytes, leaving one
 * nibble per lane (lane j -> bits 4j..4j+3) in a scalar register. This
 * replaces a UMAXV reduction followed by a 16-lane store and scan. */
static uint64_t fp_neon_bits(uint8x16_t eq) {
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
}

static uint64_t fp_neon_block(const uint8_t* p, size_t first, size_t second, int exact,
                              uint8x16_t fmask, uint8x16_t smask,
                              uint8x16_t fbyte, uint8x16_t sbyte) {
    uint8x16_t f = vld1q_u8(p + first), s = vld1q_u8(p + second);
    if (!exact) {
        f = vandq_u8(f, fmask);
        s = vandq_u8(s, smask);
    }
    return fp_neon_bits(vandq_u8(vceqq_u8(f, fbyte), vceqq_u8(s, sbyte)));
}

/* Requires count >= 16. The scan loops contain no calls: AAPCS64 preserves
 * only the low 64 bits of v8-v15, so a verification call inside the loop
 * would make the compiler reload the broadcasts from the stack on every
 * iteration. Candidates are verified after leaving the loop instead. The
 * final vector ends exactly at the last candidate, overlapping lanes that
 * were already scanned (masked off), so no scalar tail is needed. */
static size_t fp_neon(const uint8_t* data, size_t count, const fp_plan* plan) {
    const size_t first = plan->first, second = plan->second;
    const uint8_t fm = plan->mask ? plan->mask[first] : 0xff;
    const uint8_t sm = plan->mask ? plan->mask[second] : 0xff;
    const int exact = (fm & sm) == 0xff;
    const uint8x16_t fmask = vdupq_n_u8(fm), smask = vdupq_n_u8(sm);
    const uint8x16_t fbyte = vdupq_n_u8(plan->bytes[first] & fm);
    const uint8x16_t sbyte = vdupq_n_u8(plan->bytes[second] & sm);
    size_t i = 0;
    while (i < count) {
        uint64_t matches = 0;
        size_t base;
        if (exact) {
            for (; count - i >= 16; i += 16) {
                matches = fp_neon_block(data + i, first, second, 1, fmask, smask, fbyte, sbyte);
                if (matches) break;
            }
        } else {
            for (; count - i >= 16; i += 16) {
                matches = fp_neon_block(data + i, first, second, 0, fmask, smask, fbyte, sbyte);
                if (matches) break;
            }
        }
        base = i;
        if (!matches) {
            if (i == count) break;
            base = count - 16;
            matches = fp_neon_block(data + base, first, second, exact, fmask, smask, fbyte, sbyte) &
                      (~UINT64_C(0) << (4 * (i - base)));
            if (!matches) break;
        }
        matches &= UINT64_C(0x8888888888888888);
        do {
            const size_t pos = base + (fp_first_bit(matches) >> 2);
            if (fp_matches(data + pos, plan)) return pos;
            matches &= matches - 1;
        } while (matches);
        i = base + 16;
    }
    return SIZE_MAX;
}
#endif

/* Searches candidates [start, count) once any inline head has run. */
FP_NOINLINE size_t fp_search_from(const uint8_t* data, size_t count, size_t start, const fp_plan* plan) {
    size_t tail;
#if defined(FP_X86)
    if (count >= 16) {
        const int backend = fp_get_backend();
#if defined(FP_WIDE_X86)
        if (backend >= FP_AVX512 && count >= 64) return fp_avx512(data, count, start, plan);
        if (backend >= FP_AVX2 && count >= 32) return fp_avx2(data, count, start, plan);
#endif
        if (backend >= FP_SSE2) return fp_sse2(data, count, start, plan);
    }
#elif defined(FP_NEON)
    if (FP_MAX_SIMD_BITS >= 128 && count >= 16) return fp_neon(data, count, plan);
#endif
    tail = fp_scalar(data + start, count - start, plan);
    return tail == SIZE_MAX ? SIZE_MAX : start + tail;
}

FP_INLINE size_t fp_search(const uint8_t* data, size_t size, const fp_plan* plan) {
    const size_t count = size - plan->length + 1;
    if (plan->wildcard_only) return 0;
#if defined(FP_X86)
    if (!plan->mask) {
        const int backend = fp_get_backend();
        FP_RETURN_EXACT(backend, data, size, plan->bytes, plan->length);
    }
#endif
    if (count == 1) return fp_matches(data, plan) ? 0 : SIZE_MAX;
    if (!plan->mask && plan->length == 1) {
        const uint8_t* found = (const uint8_t*)memchr(data, plan->bytes[0], size);
        return found ? (size_t)(found - data) : SIZE_MAX;
    }
#if defined(FP_SSE2_BASELINE)
    if (size >= 16 && plan->mask) {
        const size_t pos = fp_verify(data, 0, fp_sse2_head(data, size, count, plan), plan);
        return pos != SIZE_MAX || count <= 32 ? pos : fp_search_from(data, count, 32, plan);
    }
#endif
    return fp_search_from(data, count, 0, plan);
}

FP_NOINLINE size_t fp_find_planned(const uint8_t* data, size_t data_size,
                                   const uint8_t* pattern, size_t pattern_size, const uint8_t* mask) {
    fp_plan plan;
    /* The first search detects the backend, so later exact ones in fp_find
     * can read it without making a call. */
    (void)fp_get_backend();
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

#if defined(FP_X86)
/* Exact searches before the backend is known. Kept out of line, like
 * fp_find_planned, so that every call from fp_find passes fp_find's own
 * arguments through; fp_find then needs no stack frame, and its dispatch
 * compiles to conditional tail jumps. */
FP_NOINLINE size_t fp_exact_planned(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) {
    return fp_find_planned(data, size, pattern, length, NULL);
}
#endif

size_t fp_find(const uint8_t* data, size_t data_size,
               const uint8_t* pattern, size_t pattern_size,
               const uint8_t* mask) {
    if (!data || !pattern || !pattern_size || data_size < pattern_size) return SIZE_MAX;
#if defined(FP_X86)
    /* Exact searches skip planning and make a single call to the widest
     * available implementation, which also handles short buffers. Until the
     * backend is known, they take the planned path, which detects it. */
    if (!mask) {
        const int backend = fp_known_backend();
        FP_RETURN_EXACT(backend, data, data_size, pattern, pattern_size);
        return fp_exact_planned(data, data_size, pattern, pattern_size);
    }
#endif
    return fp_find_planned(data, data_size, pattern, pattern_size, mask);
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
