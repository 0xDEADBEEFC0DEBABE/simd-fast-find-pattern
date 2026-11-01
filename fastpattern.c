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
/* MSVC compiles AVX2 and AVX-512 intrinsics without /arch options (since
 * Visual Studio 2017 15.3 for AVX-512), and the runtime checks choose what
 * runs, as with GCC and Clang. */
#define FP_TARGET(isa) __declspec(noinline)
#define FP_TARGET_INLINE(isa) __forceinline
#if defined(_MSC_VER) && _MSC_VER >= 1911
#define FP_WIDE_X86 1
#endif
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

enum fp_backend { FP_SCALAR = 1, FP_SSE2, FP_SSSE3, FP_AVX2, FP_AVX512, FP_ARM_NEON };

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
    if ((regs[3] & (1u << 26)) && FP_MAX_SIMD_BITS >= 128) {
        backend = FP_SSE2;
        if (regs[2] & (1u << 9)) backend = FP_SSSE3;
    }
#if defined(FP_WIDE_X86)
    /* XGETBV itself requires OSXSAVE; AVX additionally needs XMM/YMM state. */
    if (max_leaf >= 7 && FP_MAX_SIMD_BITS >= 256 &&
        (regs[2] & (7u << 26)) == (7u << 26)) {
        const uint64_t xcr0 = fp_xgetbv();
        if ((xcr0 & 6u) == 6u) {
            fp_cpuid(7, 0, regs);
            if (regs[1] & (1u << 5)) {
                backend = FP_AVX2;
                if ((xcr0 & 0xe6u) == 0xe6u &&
                    (regs[1] & ((1u << 16) | (1u << 30))) == ((1u << 16) | (1u << 30)) &&
                    FP_MAX_SIMD_BITS >= 512) {
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
    /* Masked, with exact bytes at first and last; leaf then holds the bytes
     * from first to last, followed by their mask. */
    int anchored;
    const uint8_t* leaf;
} fp_plan;


FP_INLINE void fp_prepare(fp_plan* plan, const uint8_t* bytes, size_t length,
                          const uint8_t* mask) {
    size_t first = 0, last = length - 1;
    plan->bytes = bytes;
    plan->length = length;
    plan->wildcard_only = 0;
    plan->anchored = 0;
    plan->leaf = NULL;
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
    plan->anchored = mask && (mask[first] & mask[last]) == 0xff;
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

/* The offset, below width (a power of two), at which p becomes aligned. */
FP_INLINE size_t fp_aligned_start(const uint8_t* p, size_t width) {
    return (size_t)(0 - (uintptr_t)p) & (width - 1);
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

/* The same for masked patterns whose first and last bytes are exact, which
 * the masked leaves below search like exact ones; fp_masked_small finds the
 * mask after the pattern, as the leaves do. fp_verify_masked8 is
 * fp_verify_exact8 under the mask. */
FP_INLINE size_t fp_verify_masked(const uint8_t* data, size_t base, uint64_t bits,
                                  const uint8_t* pattern, size_t length, const uint8_t* mask) {
    for (; bits; bits &= bits - 1) {
        const size_t pos = base + fp_first_bit(bits);
        if (fp_equal(data + pos, pattern, mask, length)) return pos;
    }
    return SIZE_MAX;
}

FP_INLINE size_t fp_verify_masked8(const uint8_t* data, size_t base, uint64_t bits,
                                   const uint8_t* pattern, size_t length, const uint8_t* mask) {
    const size_t tail = length - 8;
    const uint64_t head = fp_load64(pattern), head_mask = fp_load64(mask);
    const uint64_t last = fp_load64(pattern + tail), last_mask = fp_load64(mask + tail);
    for (; bits; bits &= bits - 1) {
        const uint8_t* q = data + base + fp_first_bit(bits);
        if ((((fp_load64(q) ^ head) & head_mask) | ((fp_load64(q + tail) ^ last) & last_mask)) == 0)
            return (size_t)(q - data);
    }
    return SIZE_MAX;
}

FP_NOINLINE size_t fp_masked_small(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) {
    const uint8_t* const mask = pattern + length;
    const size_t count = size - length + 1;
    size_t pos;
    for (pos = 0; pos < count; ++pos) {
        if (data[pos] == pattern[0] && fp_equal(data + pos, pattern, mask, length)) return pos;
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
 * scalar tail is needed. NAME_long_exact and NAME_long_mask plan an exact or
 * masked pattern and scan it; masked plans gain nothing from a copy of the
 * scan specialized for them, so NAME_long_mask calls NAME. Their callers
 * (NAME_lead) have checked the first WIDTH starts, so the scan begins where
 * the first anchor's loads are aligned, and only the second anchor's may
 * split cache lines. */
#define FP_DEFINE_X86_SEARCH(NAME, ATTR, VECTOR, LANES, WIDTH, SET, LOAD, AND, EQ, MATCH, ANY, BITS) \
FP_TARGET_INLINE(ATTR) static size_t NAME##_pair(const uint8_t* data, size_t base, uint64_t lo, uint64_t hi, \
                                                 const fp_plan* plan) { \
    size_t j = plan->first, pos; \
    if (!plan->mask && plan->length - 8 <= 8 && WIDTH == 16) { \
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
FP_TARGET(ATTR) static size_t NAME##_long_exact(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                                size_t length) { \
    fp_plan plan; \
    fp_prepare(&plan, pattern, length, NULL); \
    return NAME##_scan(data, size - length + 1, fp_aligned_start(data + plan.first, WIDTH), &plan); \
} \
FP_TARGET(ATTR) static size_t NAME##_long_mask(const uint8_t* data, size_t size, const uint8_t* pattern, \
                                               size_t length) { \
    fp_plan plan; \
    fp_prepare(&plan, pattern, length, pattern + length); \
    return NAME(data, size - length + 1, fp_aligned_start(data + plan.first, WIDTH), &plan); \
}
#define FP_SSE_LOAD(p) _mm_loadu_si128((const __m128i*)(const void*)(p))
#define FP_SSE_BITS(x) ((uint64_t)(unsigned int)_mm_movemask_epi8(x))
#define FP_SSE_EQ(x, y) FP_SSE_BITS(_mm_cmpeq_epi8((x), (y)))
#define FP_SSE_MATCH(x, xv, y, yv) _mm_and_si128(_mm_cmpeq_epi8((x), (xv)), _mm_cmpeq_epi8((y), (yv)))
#define FP_SSE_ANY(x, y) _mm_movemask_epi8(_mm_or_si128((x), (y)))
/* Exact patterns, and masked ones whose first and last bytes are exact, are
 * anchored on those bytes. FP_RETURN_EXACT and FP_RETURN_MASKED dispatch
 * them by tail call to small leaf functions of four kinds: _s and _w for
 * exact patterns, _ms and _mw for masked ones, where _w and _mw are for
 * patterns of 8 to 16 bytes (see fp_verify_exact8). Every leaf takes the
 * buffer, a pattern and its length; a masked pattern is followed by its
 * mask, which keeps the arguments in registers on Win64. A kind's CLASS,
 * _exact or _mask, gives the arguments of its verification (FP_ARGS, after
 * FP_PROLOGUE has found the mask) and its scalar search for buffers shorter
 * than a vector. NAME_one handles up to 64 starts, NAME_two up to 128 (SSE2
 * uses NAME_small), NAME_small up to 1024, and NAME_lead everything else. */
#define FP_PROLOGUE_exact
#define FP_PROLOGUE_mask const uint8_t* const mask = pattern + length;
#define FP_ARGS_exact pattern, length
#define FP_ARGS_mask pattern, length, mask
#define FP_SMALL_exact fp_exact_small
#define FP_SMALL_mask fp_masked_small
/* Up to 64 starts, gathered into one candidate mask before a single
 * verification loop. The last block ends at the last start and overlaps the
 * one before; ORing the same bits twice is harmless. With fewer than WIDTH
 * starts, the last-byte block ends at the buffer end; shifting it down to
 * line up with the first-byte block also clears the lanes past the last
 * start. A buffer shorter than a vector goes to the next narrower width,
 * NARROW. */
#define FP_DEFINE_X86_ONE(NAME, KIND, CLASS, VERIFY, ATTR, VECTOR, WIDTH, SET, LOAD, EQ, MATCH, BITS, NARROW) \
FP_TARGET_INLINE(ATTR) static size_t NAME##_one_core##KIND(const uint8_t* data, size_t size, \
                                                           const uint8_t* pattern, size_t length) { \
    FP_PROLOGUE##CLASS \
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
    return VERIFY(data, 0, bits, FP_ARGS##CLASS); \
} \
FP_TARGET(ATTR) static size_t NAME##_one##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) { \
    return NAME##_one_core##KIND(data, size, pattern, length); \
}
/* 65 to 128 starts in two candidate masks, lo for starts 0-63 from whole
 * blocks and hi for the rest; the last block ends at the last start and may
 * straddle both. */
#define FP_DEFINE_X86_TWO(NAME, KIND, CLASS, VERIFY, ATTR, VECTOR, WIDTH, SET, LOAD, MATCH, BITS) \
FP_TARGET(ATTR) static size_t NAME##_two##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) { \
    FP_PROLOGUE##CLASS \
    const size_t count = size - length + 1, last = length - 1, base = count - WIDTH; \
    const VECTOR av = SET((char)pattern[0]), bv = SET((char)pattern[last]); \
    uint64_t lo = 0, hi = 0, bits; \
    size_t j, pos; \
    for (j = 0; j < 64; j += WIDTH) lo |= BITS(MATCH(LOAD(data + j), av, LOAD(data + j + last), bv)) << j; \
    for (; j < base; j += WIDTH) hi |= BITS(MATCH(LOAD(data + j), av, LOAD(data + j + last), bv)) << (j - 64); \
    bits = BITS(MATCH(LOAD(data + base), av, LOAD(data + base + last), bv)); \
    hi |= base >= 64 ? bits << (base - 64) : bits >> (64 - base); \
    pos = VERIFY(data, 0, lo, FP_ARGS##CLASS); \
    return pos != SIZE_MAX ? pos : VERIFY(data, 64, hi, FP_ARGS##CLASS); \
}
/* NAME_small: more than WIDTH starts, two blocks per test, the last block
 * overlapping the one before. NAME_lead checks the first WIDTH starts (at
 * least 32) with the one-block code before the long search plans the
 * pattern, so early matches in long buffers skip the planning. */
#define FP_DEFINE_X86_SMALL(NAME, KIND, CLASS, VERIFY, ATTR, VECTOR, WIDTH, SET, LOAD, MATCH, BITS) \
FP_TARGET(ATTR) static size_t NAME##_small##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) { \
    FP_PROLOGUE##CLASS \
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
            if ((pos = VERIFY(data, (size_t)(p - data), bits, FP_ARGS##CLASS)) != SIZE_MAX) return pos; \
            if ((pos = VERIFY(data, (size_t)(p - data) + WIDTH, next, FP_ARGS##CLASS)) != SIZE_MAX) return pos; \
        } \
    } \
    if (p < end) { \
        bits = BITS(MATCH(LOAD(p), av, LOAD(p + last), bv)); \
        if (bits && (pos = VERIFY(data, (size_t)(p - data), bits, FP_ARGS##CLASS)) != SIZE_MAX) return pos; \
        p += WIDTH; \
    } \
    bits = BITS(MATCH(LOAD(end), av, LOAD(end + last), bv)) & (~UINT64_C(0) << (p - end)); \
    return VERIFY(data, (size_t)(end - data), bits, FP_ARGS##CLASS); \
} \
FP_TARGET(ATTR) static size_t NAME##_lead##KIND(const uint8_t* data, size_t size, const uint8_t* pattern, size_t length) { \
    const size_t lead = WIDTH < 32 ? 32 : WIDTH; \
    const size_t pos = NAME##_one_core##KIND(data, length + lead - 1, pattern, length); \
    return pos != SIZE_MAX ? pos : NAME##_long##CLASS(data, size, pattern, length); \
}
FP_DEFINE_X86_SEARCH(fp_sse2, "sse2", __m128i, __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD,
                     _mm_and_si128, FP_SSE_EQ, FP_SSE_MATCH, FP_SSE_ANY, FP_SSE_BITS)
#define FP_DEFINE_SSE2_LEAVES(KIND, CLASS, VERIFY) \
    FP_DEFINE_X86_ONE(fp_sse2, KIND, CLASS, VERIFY, "sse2", __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD, FP_SSE_EQ, \
                      FP_SSE_MATCH, FP_SSE_BITS, FP_SMALL##CLASS) \
    FP_DEFINE_X86_SMALL(fp_sse2, KIND, CLASS, VERIFY, "sse2", __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD, \
                        FP_SSE_MATCH, FP_SSE_BITS)
FP_DEFINE_SSE2_LEAVES(_s, _exact, fp_verify_exact)
FP_DEFINE_SSE2_LEAVES(_w, _exact, fp_verify_exact8)
FP_DEFINE_SSE2_LEAVES(_ms, _mask, fp_verify_masked)
FP_DEFINE_SSE2_LEAVES(_mw, _mask, fp_verify_masked8)
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
#define FP_DEFINE_AVX2_LEAVES(KIND, CLASS, VERIFY) \
    FP_DEFINE_X86_ONE(fp_avx2x16, KIND, CLASS, VERIFY, "avx2", __m128i, 16, _mm_set1_epi8, FP_SSE_LOAD, \
                      FP_SSE_EQ, FP_SSE_MATCH, FP_SSE_BITS, FP_SMALL##CLASS) \
    FP_DEFINE_X86_ONE(fp_avx2, KIND, CLASS, VERIFY, "avx2", __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD, \
                      FP_AVX_EQ, FP_AVX_MATCH, FP_AVX_BITS, fp_avx2x16_one##KIND) \
    FP_DEFINE_X86_TWO(fp_avx2, KIND, CLASS, VERIFY, "avx2", __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD, \
                      FP_AVX_MATCH, FP_AVX_BITS) \
    FP_DEFINE_X86_SMALL(fp_avx2, KIND, CLASS, VERIFY, "avx2", __m256i, 32, _mm256_set1_epi8, FP_AVX_LOAD, \
                        FP_AVX_MATCH, FP_AVX_BITS)
FP_DEFINE_AVX2_LEAVES(_s, _exact, fp_verify_exact)
FP_DEFINE_AVX2_LEAVES(_w, _exact, fp_verify_exact8)
FP_DEFINE_AVX2_LEAVES(_ms, _mask, fp_verify_masked)
FP_DEFINE_AVX2_LEAVES(_mw, _mask, fp_verify_masked8)
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
 * to 64 starts loads exactly the bytes it needs, at any size: the one block
 * of fp_avx512_one and the second block of fp_avx512_two. */
#define FP_DEFINE_AVX512_LEAVES(KIND, CLASS, VERIFY) \
FP_TARGET_INLINE("avx512f,avx512bw") static size_t fp_avx512_one_core##KIND(const uint8_t* data, size_t size, \
                                                                            const uint8_t* pattern, size_t length) { \
    FP_PROLOGUE##CLASS \
    const size_t last = length - 1; \
    const __mmask64 lanes = (__mmask64)(~UINT64_C(0) >> (64 - (size - last))); \
    const __m512i x = _mm512_maskz_loadu_epi8(lanes, data), y = _mm512_maskz_loadu_epi8(lanes, data + last); \
    const __mmask64 bits = _mm512_mask_cmpeq_epi8_mask( \
        _mm512_mask_cmpeq_epi8_mask(lanes, x, _mm512_set1_epi8((char)pattern[0])), y, \
        _mm512_set1_epi8((char)pattern[last])); \
    return VERIFY(data, 0, bits, FP_ARGS##CLASS); \
} \
FP_TARGET("avx512f,avx512bw") static size_t fp_avx512_one##KIND(const uint8_t* data, size_t size, \
                                                                const uint8_t* pattern, size_t length) { \
    return fp_avx512_one_core##KIND(data, size, pattern, length); \
} \
FP_TARGET("avx512f,avx512bw") static size_t fp_avx512_two##KIND(const uint8_t* data, size_t size, \
                                                                const uint8_t* pattern, size_t length) { \
    FP_PROLOGUE##CLASS \
    const size_t last = length - 1; \
    const __mmask64 more = (__mmask64)(~UINT64_C(0) >> (128 - (size - last))); \
    const __m512i av = _mm512_set1_epi8((char)pattern[0]), bv = _mm512_set1_epi8((char)pattern[last]); \
    const __m512i x = _mm512_maskz_loadu_epi8(more, data + 64); \
    const __m512i y = _mm512_maskz_loadu_epi8(more, data + 64 + last); \
    const uint64_t lo = FP_AVX512_MATCH(FP_AVX512_LOAD(data), av, FP_AVX512_LOAD(data + last), bv); \
    const uint64_t hi = _mm512_mask_cmpeq_epi8_mask(_mm512_mask_cmpeq_epi8_mask(more, x, av), y, bv); \
    const size_t pos = VERIFY(data, 0, lo, FP_ARGS##CLASS); \
    return pos != SIZE_MAX ? pos : VERIFY(data, 64, hi, FP_ARGS##CLASS); \
} \
FP_DEFINE_X86_SMALL(fp_avx512, KIND, CLASS, VERIFY, "avx512f,avx512bw", __m512i, 64, _mm512_set1_epi8, \
                    FP_AVX512_LOAD, FP_AVX512_MATCH, FP_AVX512_BITS)
FP_DEFINE_AVX512_LEAVES(_s, _exact, fp_verify_exact)
FP_DEFINE_AVX512_LEAVES(_w, _exact, fp_verify_exact8)
FP_DEFINE_AVX512_LEAVES(_ms, _mask, fp_verify_masked)
FP_DEFINE_AVX512_LEAVES(_mw, _mask, fp_verify_masked8)
#endif
#undef FP_DEFINE_X86_SEARCH

/* Teddy, from Hyperscan, tests many signatures per vector block. Each
 * signature in a group has a fingerprint, FP_TEDDY_LENGTH consecutive bytes
 * of it, and one of 8 buckets. For fingerprint byte k, lo[k] and hi[k] map
 * each nibble value to the buckets holding a member that accepts it as that
 * byte's low or high nibble, so wildcards and nibble masks cost nothing.
 * PSHUFB looks up both nibbles of every data byte; ANDing the lookups of
 * bytes i to i + 3 leaves the buckets whose fingerprints may start at i, and
 * their members are verified. Members of a bucket share its table bits,
 * which also admits mixtures of their bytes, so buckets are filled by the
 * estimated cost of those mixtures. On real code, 4-byte fingerprints cost
 * about a tenth more per block than 3-byte ones and pass several times
 * fewer false candidates. */
#define FP_TEDDY_LENGTH 4
enum { FP_TEDDY_BUCKET = 8 };

typedef struct {
    uint8_t lo[FP_TEDDY_LENGTH][16];
    uint8_t hi[FP_TEDDY_LENGTH][16];
} fp_teddy_tables;

typedef struct {
    size_t sig;    /* the signature's index */
    size_t offset; /* of its fingerprint in it */
} fp_member;

typedef struct {
    fp_teddy_tables tables;
    size_t first;     /* bucket b holds members first + start[b] to first + start[b + 1] - 1 */
    uint8_t start[9];
    size_t reach;     /* the largest fingerprint offset */
} fp_teddy;

#define FP_TEDDY_LOOKUP(LOAD, AND, SHUFFLE, SHIFT4, p, lo, hi) \
    AND(SHUFFLE(lo, AND(LOAD(p), nibble)), SHUFFLE(hi, AND(SHIFT4(LOAD(p)), nibble)))
#define FP_TEDDY_TABLES(VECTOR, TABLE) \
    const VECTOR lo0 = TABLE(t->lo[0]), lo1 = TABLE(t->lo[1]), lo2 = TABLE(t->lo[2]), lo3 = TABLE(t->lo[3]); \
    const VECTOR hi0 = TABLE(t->hi[0]), hi1 = TABLE(t->hi[1]), hi2 = TABLE(t->hi[2]), hi3 = TABLE(t->hi[3]);
#define FP_TEDDY_BLOCK(LOAD, AND, SHUFFLE, SHIFT4, p) \
    AND(AND(FP_TEDDY_LOOKUP(LOAD, AND, SHUFFLE, SHIFT4, p, lo0, hi0), \
            FP_TEDDY_LOOKUP(LOAD, AND, SHUFFLE, SHIFT4, (p) + 1, lo1, hi1)), \
        AND(FP_TEDDY_LOOKUP(LOAD, AND, SHUFFLE, SHIFT4, (p) + 2, lo2, hi2), \
            FP_TEDDY_LOOKUP(LOAD, AND, SHUFFLE, SHIFT4, (p) + 3, lo3, hi3)))
/* Scans windows (fingerprint starts) [i, end), WIDTH at a time, for the
 * first block with candidates: stores its bucket bytes in out and the
 * windows holding any in *lanes, and returns the block's first window, or
 * SIZE_MAX. The final block ends at end and masks windows already scanned;
 * if end < WIDTH it starts at 0 instead, so the data must hold WIDTH
 * windows. Callers verify, then call again: the loop makes no calls, so its
 * tables stay in registers. */
#define FP_DEFINE_TEDDY(NAME, ATTR, VECTOR, WIDTH, LOAD, TABLE, SET1, AND, SHUFFLE, SHIFT4, NONZERO, STORE) \
FP_TARGET(ATTR) static size_t NAME(const uint8_t* data, size_t i, size_t end, const fp_teddy_tables* t, \
                                   uint64_t* lanes, uint8_t* out) { \
    const VECTOR nibble = SET1(0x0f); \
    FP_TEDDY_TABLES(VECTOR, TABLE) \
    VECTOR r; \
    uint64_t bits; \
    size_t base; \
    for (; end - i >= WIDTH; i += WIDTH) { \
        r = FP_TEDDY_BLOCK(LOAD, AND, SHUFFLE, SHIFT4, data + i); \
        if ((bits = NONZERO(r)) != 0) { \
            STORE(out, r); \
            *lanes = bits; \
            return i; \
        } \
    } \
    if (i >= end) return SIZE_MAX; \
    base = end >= WIDTH ? end - WIDTH : 0; \
    r = FP_TEDDY_BLOCK(LOAD, AND, SHUFFLE, SHIFT4, data + base); \
    bits = NONZERO(r) & (~UINT64_C(0) << (i - base)); \
    if (end - base < WIDTH) bits &= (UINT64_C(1) << (end - base)) - 1; \
    if (!bits) return SIZE_MAX; \
    STORE(out, r); \
    *lanes = bits; \
    return base; \
}
#define FP_SSE_SET1(x) _mm_set1_epi8((char)(x))
#define FP_SSE_SHIFT4(v) _mm_srli_epi16((v), 4)
#define FP_SSE_NONZERO(r) ((uint64_t)(_mm_movemask_epi8(_mm_cmpeq_epi8((r), _mm_setzero_si128())) ^ 0xffff))
#define FP_SSE_STORE(p, r) _mm_storeu_si128((__m128i*)(void*)(p), (r))
FP_DEFINE_TEDDY(fp_teddy_ssse3, "ssse3", __m128i, 16, FP_SSE_LOAD, FP_SSE_LOAD, FP_SSE_SET1, _mm_and_si128,
                _mm_shuffle_epi8, FP_SSE_SHIFT4, FP_SSE_NONZERO, FP_SSE_STORE)
#if defined(FP_WIDE_X86)
#define FP_AVX_TABLE(p) _mm256_broadcastsi128_si256(FP_SSE_LOAD(p))
#define FP_AVX_SET1(x) _mm256_set1_epi8((char)(x))
#define FP_AVX_SHIFT4(v) _mm256_srli_epi16((v), 4)
#define FP_AVX_NONZERO(r) \
    ((uint64_t)(uint32_t)~_mm256_movemask_epi8(_mm256_cmpeq_epi8((r), _mm256_setzero_si256())))
#define FP_AVX_STORE(p, r) _mm256_storeu_si256((__m256i*)(void*)(p), (r))
FP_DEFINE_TEDDY(fp_teddy_avx2, "avx2", __m256i, 32, FP_AVX_LOAD, FP_AVX_TABLE, FP_AVX_SET1, _mm256_and_si256,
                _mm256_shuffle_epi8, FP_AVX_SHIFT4, FP_AVX_NONZERO, FP_AVX_STORE)
#define FP_AVX512_TABLE(p) _mm512_broadcast_i32x4(FP_SSE_LOAD(p))
#define FP_AVX512_SET1(x) _mm512_set1_epi8((char)(x))
#define FP_AVX512_SHIFT4(v) _mm512_srli_epi16((v), 4)
#define FP_AVX512_NONZERO(r) ((uint64_t)_mm512_test_epi8_mask((r), (r)))
#define FP_AVX512_STORE(p, r) _mm512_storeu_si512((void*)(p), (r))
FP_DEFINE_TEDDY(fp_teddy_avx512, "avx512f,avx512bw", __m512i, 64, FP_AVX512_LOAD, FP_AVX512_TABLE, FP_AVX512_SET1,
                _mm512_and_si512, _mm512_shuffle_epi8, FP_AVX512_SHIFT4, FP_AVX512_NONZERO, FP_AVX512_STORE)
#endif

static size_t fp_teddy_width(int backend) {
#if defined(FP_WIDE_X86)
    if (backend >= FP_AVX512) return 64;
    if (backend >= FP_AVX2) return 32;
#endif
    (void)backend;
    return 16;
}

static size_t fp_teddy_next(int backend, const uint8_t* data, size_t i, size_t end, const fp_teddy_tables* t,
                            uint64_t* lanes, uint8_t* out) {
#if defined(FP_WIDE_X86)
    if (backend >= FP_AVX512) return fp_teddy_avx512(data, i, end, t, lanes, out);
    if (backend >= FP_AVX2) return fp_teddy_avx2(data, i, end, t, lanes, out);
#endif
    (void)backend;
    return fp_teddy_ssse3(data, i, end, t, lanes, out);
}

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
/* Returns from the enclosing function with the widest available leaf (see
 * the leaf kinds above), passing it ARGS, a parenthesized argument list;
 * falls through when the backend has no SIMD. The pattern kind and the
 * number of starts are tested first, the backend last, so whichever backend
 * is in use reaches its leaf with one taken branch. BACKEND is a local,
 * evaluated more than once. */
#if defined(FP_WIDE_X86)
#define FP_RETURN_LEAF(backend, LEAF, ARGS) do { \
        if ((backend) >= FP_AVX512) return fp_avx512_##LEAF ARGS; \
        if ((backend) >= FP_AVX2) return fp_avx2_##LEAF ARGS; \
        if ((backend) >= FP_SSE2) return fp_sse2_##LEAF ARGS; \
    } while (0)
/* Buffers shorter than 32 bytes take AVX2's 16-byte leaf directly. */
#define FP_RETURN_ONE(backend, LEAF, size, ARGS) do { \
        if ((backend) >= FP_AVX512) return fp_avx512_##LEAF ARGS; \
        if ((backend) >= FP_AVX2) return (size) < 32 ? fp_avx2x16_##LEAF ARGS : fp_avx2_##LEAF ARGS; \
        if ((backend) >= FP_SSE2) return fp_sse2_##LEAF ARGS; \
    } while (0)
/* For 65 to 128 starts, SSE2's loop beats merging eight blocks into masks. */
#define FP_RETURN_TWO(backend, KIND, ARGS) do { \
        if ((backend) >= FP_AVX512) return fp_avx512_two##KIND ARGS; \
        if ((backend) >= FP_AVX2) return fp_avx2_two##KIND ARGS; \
        if ((backend) >= FP_SSE2) return fp_sse2_small##KIND ARGS; \
    } while (0)
#else
#define FP_RETURN_LEAF(backend, LEAF, ARGS) do { \
        if ((backend) >= FP_SSE2) return fp_sse2_##LEAF ARGS; \
    } while (0)
#define FP_RETURN_ONE(backend, LEAF, size, ARGS) FP_RETURN_LEAF(backend, LEAF, ARGS)
#define FP_RETURN_TWO(backend, KIND, ARGS) FP_RETURN_LEAF(backend, small##KIND, ARGS)
#endif
#define FP_RETURN_KIND(backend, KIND, size, length, ARGS) do { \
        const size_t fp_starts = (size) - (length); \
        if (fp_starts < 64) FP_RETURN_ONE(backend, one##KIND, size, ARGS); \
        else if (fp_starts < 128) FP_RETURN_TWO(backend, KIND, ARGS); \
        else if (fp_starts < 1024) FP_RETURN_LEAF(backend, small##KIND, ARGS); \
        else FP_RETURN_LEAF(backend, lead##KIND, ARGS); \
    } while (0)
#define FP_RETURN_EXACT(backend, data, size, pattern, length) do { \
        if ((length) - 8 <= 8) FP_RETURN_KIND(backend, _w, size, length, (data, size, pattern, length)); \
        else FP_RETURN_KIND(backend, _s, size, length, (data, size, pattern, length)); \
    } while (0)
/* A masked pattern whose first and last bytes are exact, followed by its mask. */
#define FP_RETURN_MASKED(backend, data, size, pattern, length) do { \
        if ((length) - 8 <= 8) FP_RETURN_KIND(backend, _mw, size, length, (data, size, pattern, length)); \
        else FP_RETURN_KIND(backend, _ms, size, length, (data, size, pattern, length)); \
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

#if defined(FP_X86)
FP_NOINLINE size_t fp_plan_masked(const uint8_t* data, size_t size, const fp_plan* plan);
#endif

FP_INLINE size_t fp_search(const uint8_t* data, size_t size, const fp_plan* plan) {
    const size_t count = size - plan->length + 1;
    if (plan->wildcard_only) return 0;
#if defined(FP_X86)
    if (!plan->mask) {
        const int backend = fp_get_backend();
        FP_RETURN_EXACT(backend, data, size, plan->bytes, plan->length);
    }
    /* With SIMD, fp_plan_masked dispatches every size, so it never comes back. */
    if (plan->anchored && fp_get_backend() >= FP_SSE2) return fp_plan_masked(data, size, plan);
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

/* fp_search out of line, for searches that fp_plan_find does not dispatch
 * itself, including those before the backend is known. */
FP_NOINLINE size_t fp_plan_search(const uint8_t* data, size_t size, const fp_plan* plan) {
    return fp_search(data, size, plan);
}

#if defined(FP_X86)
/* Masked plans whose first and last constrained bytes are exact: wherever
 * the pattern starts in data, its bytes from first to last start at the
 * same offset in data + first, and plan->leaf holds them with their mask.
 * Returns from the enclosing function. */
#define FP_RETURN_PLAN_MASKED(backend, data, size, plan) do { \
        const size_t fp_first = (plan)->first, fp_length = (plan)->last - fp_first + 1; \
        FP_RETURN_MASKED(backend, (data) + fp_first, (size) - ((plan)->length - fp_length), (plan)->leaf, \
                         fp_length); \
    } while (0)

/* Kept out of fp_plan_find, whose exact paths then save no registers; each
 * backend test reads the cached value afresh, a memory operand, for the
 * same reason. */
FP_NOINLINE size_t fp_plan_masked(const uint8_t* data, size_t size, const fp_plan* plan) {
    FP_RETURN_PLAN_MASKED(fp_known_backend(), data, size, plan);
    return fp_plan_search(data, size, plan);
}
#endif

/* Searches for one signature. Exact plans, and masked ones whose first and
 * last constrained bytes are exact, reach a leaf through tail jumps alone. */
FP_NOINLINE size_t fp_plan_find(const uint8_t* data, size_t size, const fp_plan* plan) {
    if (size < plan->length) return SIZE_MAX;
#if defined(FP_X86)
    if (!plan->mask) {
        const int backend = fp_known_backend();
        FP_RETURN_EXACT(backend, data, size, plan->bytes, plan->length);
    } else if (plan->anchored) {
        return fp_plan_masked(data, size, plan);
    } else if (plan->wildcard_only) {
        return 0;
    }
#endif
    return fp_plan_search(data, size, plan);
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

/* Counts the tokens of text, storing them in bytes and mask unless those are
 * NULL; a caller counts first, then fills storage of exactly that size. On
 * malformed input, returns 0 with *error_at the offset of the bad token,
 * which is otherwise SIZE_MAX. */
static size_t fp_parse(const char* text, uint8_t* bytes, uint8_t* mask, size_t* error_at) {
    const unsigned char* p = (const unsigned char*)text;
    size_t count = 0;
    *error_at = SIZE_MAX;
    while (*p) {
        const unsigned char* token;
        int high, low;
        uint8_t byte = 0, bits = 0;
        while (fp_space(*p)) ++p;
        if (!*p) break;
        token = p;
        if (*p == '?' && (!p[1] || fp_space(p[1]))) {
            ++p;
        } else {
            high = *p == '?' ? 0 : fp_hex(*p);
            low = high < 0 || !p[1] ? -1 : p[1] == '?' ? 0 : fp_hex(p[1]);
            if (low < 0) {
                *error_at = (size_t)(token - (const unsigned char*)text);
                return 0;
            }
            if (*p != '?') bits = 0xf0;
            if (p[1] != '?') bits |= 0x0f;
            byte = (uint8_t)((high << 4) | low);
            p += 2;
        }
        if (*p && !fp_space(*p)) {
            *error_at = (size_t)(token - (const unsigned char*)text);
            return 0;
        }
        if (bytes) { bytes[count] = byte; mask[count] = bits; }
        ++count;
    }
    return count;
}

/* A set is one allocation: this header, a plan per signature, on x86 its
 * Teddy groups and strays, then every signature's bytes, every mask, and
 * the leaf copies of anchored plans. The plans are found from the header's
 * address, not loaded through a pointer, which saves single searches a
 * dependent load. */
struct fp_set {
    size_t count;
#if defined(FP_X86)
    /* Signatures in Teddy groups, and strays searched on their own: those
     * too short for a fingerprint, or without an informative one. */
    size_t groups;
    fp_teddy* teddy;
    fp_member* members;
    size_t strays;
    size_t* stray;
#endif
};

FP_INLINE fp_plan* fp_plans(const fp_set* set) { return (fp_plan*)(void*)(set + 1); }

static fp_set* fp_fail(fp_error* error, fp_status status, size_t index, size_t position) {
    if (error) {
        error->status = status;
        error->index = index;
        error->position = position;
    }
    return NULL;
}

/* Allocates a set of count signatures of total bytes; *storage receives
 * room for their bytes, their masks, and leaf copies of both. */
static fp_set* fp_set_alloc(size_t count, size_t total, uint8_t** storage, fp_error* error) {
#if defined(FP_X86)
    const size_t groups = count > 1 ? count : 0;
    const size_t per_signature = sizeof(fp_plan) + sizeof(fp_teddy) + sizeof(fp_member) + sizeof(size_t);
#else
    const size_t per_signature = sizeof(fp_plan);
#endif
    size_t size = sizeof(fp_set);
    fp_set* set;
    if (count > (SIZE_MAX - size) / per_signature) return fp_fail(error, FP_ERROR_MEMORY, 0, 0);
    size += count * sizeof(fp_plan);
#if defined(FP_X86)
    size += groups * sizeof(fp_teddy) + count * (sizeof(fp_member) + sizeof(size_t));
#endif
    if (total > (SIZE_MAX - size) / 4) return fp_fail(error, FP_ERROR_MEMORY, 0, 0);
    set = (fp_set*)malloc(size + 4 * total);
    if (!set) return fp_fail(error, FP_ERROR_MEMORY, 0, 0);
    set->count = count;
#if defined(FP_X86)
    set->groups = 0;
    set->teddy = (fp_teddy*)(fp_plans(set) + count);
    set->members = (fp_member*)(set->teddy + groups);
    set->strays = 0;
    set->stray = (size_t*)(set->members + count);
    *storage = (uint8_t*)(set->stray + count);
#else
    *storage = (uint8_t*)(fp_plans(set) + count);
#endif
    return set;
}

/* Prepares plan i of bytes and mask; an anchored plan copies its bytes from
 * first to last and then their mask to *leaf, which advances. */
static void fp_set_plan(fp_set* set, size_t i, const uint8_t* bytes, size_t length, const uint8_t* mask,
                        uint8_t** leaf) {
    fp_plan* plan = &fp_plans(set)[i];
    fp_prepare(plan, bytes, length, mask);
    if (plan->anchored) {
        const size_t span = plan->last - plan->first + 1;
        memcpy(*leaf, bytes + plan->first, span);
        memcpy(*leaf + span, mask + plan->first, span);
        plan->leaf = *leaf;
        *leaf += 2 * span;
    }
}

#if defined(FP_X86)
/* Frequencies of byte values in x86-64 code, in 1/65536 (at least 1), from
 * the executable sections of 25 Windows system binaries, 50 MB in all: 00 is
 * 10% of bytes, 48 8%, 8B 6%, CC 5%. Sets estimate from them how often a
 * fingerprint matches ordinary code. */
static const uint16_t fp_byte_freq[256] = {
    6811,  832,  367,  370,  347,  443,  158,  236,  554,  119,  117,  136,  172,  232,  102, 1484,
     496,  188,   68,   70,  100,  323,   64,   70,  264,   63,   58,   73,   75,   56,   52,  245,
     609,   65,   55,   85, 1701,   95,   41,   50,  311,   56,   42,  136,   56,   43,   43,   46,
     381,   38,   53,  545,   62,   43,   36,   38,  272,  153,   42,  242,   61,   74,   43,   48,
     547, 1050,  119,  160, 1106,  702,   92,  110, 5283,  686,   63,  101, 1056,  453,   65,   74,
     229,   58,   41,  106,  181,  184,  132,  248,  177,   53,   33,  106,  317,  134,  128,  190,
     168,   42,   31,   71,  107,  120,  239,   39,  123,   44,   27,   43,  117,   43,   40,   52,
     154,   32,   73,   98,  642,  363,   55,   63,  176,   83,   35,   52,  127,   78,   48,   88,
     260,  173,   60, 1000,  387,  842,   53,   80,  208, 1459,  108, 3679,   77, 1223,   40,   48,
     157,   28,   24,   31,   51,   42,   28,   35,   72,   30,   27,   23,   60,   26,   25,   36,
      79,   25,   23,   30,   34,   25,   24,   36,   77,   27,   34,   29,   37,   25,   28,   48,
      87,   40,   29,   31,   41,   29,   88,  136,  211,  124,  183,   79,   49,   39,   52,   69,
     819,  323,  137,  410,  325,   70,  153,  276,  284,  257,   93,  175, 3368,   72,  144,  177,
     184,   95,  188,   86,   43,   50,   79,   94,  235,  100,   63,  121,   45,   33,   41,   55,
     166,   71,   58,   59,   54,   34,   40,   54, 1184,  410,   71,  315,  265,   63,   46,   56,
     221,   83,  105,  142,   45,   44,  188,  129,  215,  135,  104,   86,   82,   99,  176, 2114,
};

/* A fingerprint must match code with an estimated chance below
 * FP_TEDDY_CHANCE, and a group's buckets together below FP_TEDDY_BUDGET per
 * window; a bucket holds at most FP_TEDDY_BUCKET members. The estimate
 * treats bytes as independent, while real code passes 5 to 15 times more
 * windows, so the budget is set by measurement: on real code, tighter ones
 * make more groups and looser ones more false candidates.
 *
 * Teddy pays off from FP_TEDDY_MIN such signatures on (a pass costs more
 * than the searches of fewer), in buffers of FP_TEDDY_SIZE bytes or more:
 * one block for the widest kernel, and on random data faster than the
 * signatures' own searches from 128 bytes on.
 *
 * A candidate costs about as much as one member's own search of
 * FP_TEDDY_COST bytes, so a group gives way to searching its members one by
 * one once its candidates have cost more than searching max(i,
 * FP_TEDDY_WARMUP) bytes for each member left to find, i being the bytes
 * scanned. Floods spend that within kilobytes; a dense stretch of code,
 * soon diluted, does not. */
#define FP_TEDDY_BUDGET (1.0 / 32768)
#define FP_TEDDY_CHANCE (1.0 / 4096)
enum { FP_TEDDY_MIN = 5, FP_TEDDY_SIZE = 64 + FP_TEDDY_LENGTH - 1, FP_TEDDY_COST = 2048 };
#define FP_TEDDY_WARMUP ((size_t)1 << 20)

/* The bytes a bucket accepts at each fingerprint position: any whose low
 * nibble is in lo[k] and high nibble in hi[k]. */
typedef struct {
    uint16_t lo[FP_TEDDY_LENGTH], hi[FP_TEDDY_LENGTH];
} fp_nibbles;

static void fp_member_nibbles(const fp_plan* plan, size_t offset, fp_nibbles* out) {
    size_t k;
    unsigned x;
    for (k = 0; k < FP_TEDDY_LENGTH; ++k) {
        const unsigned v = plan->bytes[offset + k], m = plan->mask ? plan->mask[offset + k] : 0xff;
        out->lo[k] = out->hi[k] = 0;
        for (x = 0; x < 16; ++x) {
            if (((x ^ v) & m & 15) == 0) out->lo[k] |= (uint16_t)(1u << x);
            if (((x ^ (v >> 4)) & (m >> 4)) == 0) out->hi[k] |= (uint16_t)(1u << x);
        }
    }
}

/* The estimated chance that a code window passes these nibble sets. */
static double fp_nibbles_chance(const fp_nibbles* n) {
    double chance = 1;
    size_t k;
    unsigned h, l;
    for (k = 0; k < FP_TEDDY_LENGTH; ++k) {
        unsigned sum = 0;
        for (h = 0; h < 16; ++h) {
            if (!(n->hi[k] >> h & 1)) continue;
            for (l = 0; l < 16; ++l) {
                if (n->lo[k] >> l & 1) sum += fp_byte_freq[h << 4 | l];
            }
        }
        chance *= sum / 65536.0;
    }
    return chance;
}

typedef struct {
    uint32_t key;    /* the fingerprint's significant bits, for sorting */
    size_t sig, offset;
    unsigned bucket;
} fp_candidate;

static int fp_compare_candidates(const void* a, const void* b) {
    const fp_candidate* x = (const fp_candidate*)a;
    const fp_candidate* y = (const fp_candidate*)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->sig < y->sig ? -1 : x->sig > y->sig;
}

/* Lays out the group of candidates [first, last), sorted by bucket, and
 * fills its tables. */
static void fp_finish_group(fp_set* set, fp_candidate* candidate, size_t first, size_t last) {
    fp_teddy* teddy = &set->teddy[set->groups++];
    unsigned b, x;
    size_t j, k, at;
    memset(teddy, 0, sizeof(fp_teddy));
    teddy->first = first;
    for (j = first; j < last; ++j) ++teddy->start[candidate[j].bucket + 1];
    for (b = 0; b < 8; ++b) teddy->start[b + 1] = (uint8_t)(teddy->start[b + 1] + teddy->start[b]);
    for (b = 0, at = first; b < 8; ++b) {
        for (j = first; j < last; ++j) {
            const fp_candidate* c = &candidate[j];
            fp_nibbles n;
            if (c->bucket != b) continue;
            set->members[at].sig = c->sig;
            set->members[at].offset = c->offset;
            ++at;
            if (c->offset > teddy->reach) teddy->reach = c->offset;
            fp_member_nibbles(&fp_plans(set)[c->sig], c->offset, &n);
            for (k = 0; k < FP_TEDDY_LENGTH; ++k) {
                for (x = 0; x < 16; ++x) {
                    if (n.lo[k] >> x & 1) teddy->tables.lo[k][x] |= (uint8_t)(1u << b);
                    if (n.hi[k] >> x & 1) teddy->tables.hi[k][x] |= (uint8_t)(1u << b);
                }
            }
        }
    }
}

/* Fingerprints each signature at its window least likely to match code.
 * Those likely enough to stay rare are sorted, so that similar fingerprints
 * meet, and each joins the bucket of the current group whose chance it
 * raises least, until the group's budget is spent; the rest are strays.
 * Without memory to sort, all are strays. */
static void fp_set_group(fp_set* set) {
    fp_candidate* candidate = (fp_candidate*)malloc(set->count * sizeof(fp_candidate));
    fp_nibbles bucket[8];
    double chance[8], spent = 0;
    unsigned used[8], b;
    size_t i, k, n = 0, first = 0;
    for (i = 0; candidate && i < set->count; ++i) {
        const fp_plan* plan = &fp_plans(set)[i];
        size_t w, offset = 0;
        double best = 2;
        for (w = 0; !plan->wildcard_only && w + FP_TEDDY_LENGTH <= plan->length; ++w) {
            fp_nibbles nibbles;
            double c;
            fp_member_nibbles(plan, w, &nibbles);
            c = fp_nibbles_chance(&nibbles);
            if (c < best) {
                best = c;
                offset = w;
            }
        }
        if (best < FP_TEDDY_CHANCE) {
            uint32_t key = 0;
            for (k = 0; k < FP_TEDDY_LENGTH; ++k) {
                const size_t j = offset + k;
                key = key << 8 | (uint32_t)(plan->bytes[j] & (plan->mask ? plan->mask[j] : 0xff));
            }
            candidate[n].key = key;
            candidate[n].sig = i;
            candidate[n].offset = offset;
            ++n;
        }
    }
    if (n < FP_TEDDY_MIN) n = 0;
    /* Strays in index order, as the candidates still are. */
    for (i = 0, k = 0; i < set->count; ++i) {
        if (k < n && candidate[k].sig == i) ++k;
        else set->stray[set->strays++] = i;
    }
    if (n) qsort(candidate, n, sizeof(fp_candidate), fp_compare_candidates);
    for (i = 0; i < n; ++i) {
        fp_nibbles own, merged;
        double increase = 2;
        unsigned choice = 8;
        if (i == first) {
            memset(used, 0, sizeof(used));
            memset(chance, 0, sizeof(chance));
            spent = 0;
        }
        fp_member_nibbles(&fp_plans(set)[candidate[i].sig], candidate[i].offset, &own);
        for (b = 0; b < 8; ++b) {
            double c;
            if (used[b] == FP_TEDDY_BUCKET) continue;
            merged = own;
            if (used[b]) {
                for (k = 0; k < FP_TEDDY_LENGTH; ++k) {
                    merged.lo[k] |= bucket[b].lo[k];
                    merged.hi[k] |= bucket[b].hi[k];
                }
            }
            c = fp_nibbles_chance(&merged) - chance[b];
            if (c < increase) {
                increase = c;
                choice = b;
            }
        }
        if (i > first && (choice == 8 || spent + increase > FP_TEDDY_BUDGET)) {
            fp_finish_group(set, candidate, first, i);
            first = i;
            --i;
            continue;
        }
        if (used[choice]) {
            for (k = 0; k < FP_TEDDY_LENGTH; ++k) {
                own.lo[k] |= bucket[choice].lo[k];
                own.hi[k] |= bucket[choice].hi[k];
            }
        }
        bucket[choice] = own;
        chance[choice] += increase;
        spent += increase;
        ++used[choice];
        candidate[i].bucket = choice;
    }
    if (n) fp_finish_group(set, candidate, first, n);
    free(candidate);
}
#endif

static fp_set* fp_set_finish(fp_set* set, fp_error* error) {
#if defined(FP_X86)
    if (set->count > 1) fp_set_group(set);
#endif
    if (error) {
        error->status = FP_OK;
        error->index = 0;
        error->position = 0;
    }
    return set;
}

fp_set* fp_set_from_bytes(const fp_sig* sigs, size_t count, fp_error* error) {
    size_t i, total = 0;
    fp_set* set;
    uint8_t *storage, *leaf;
    if (!sigs || !count) return fp_fail(error, FP_ERROR_ARGUMENT, 0, 0);
    for (i = 0; i < count; ++i) {
        if (!sigs[i].bytes) return fp_fail(error, FP_ERROR_ARGUMENT, i, 0);
        if (!sigs[i].length) return fp_fail(error, FP_ERROR_EMPTY, i, 0);
        if (sigs[i].length > SIZE_MAX - total) return fp_fail(error, FP_ERROR_MEMORY, i, 0);
        total += sigs[i].length;
    }
    set = fp_set_alloc(count, total, &storage, error);
    if (!set) return NULL;
    leaf = storage + 2 * total;
    for (i = 0; i < count; ++i) {
        const size_t length = sigs[i].length;
        memcpy(storage, sigs[i].bytes, length);
        if (sigs[i].mask) memcpy(storage + total, sigs[i].mask, length);
        else memset(storage + total, 0xff, length);
        fp_set_plan(set, i, storage, length, storage + total, &leaf);
        storage += length;
    }
    return fp_set_finish(set, error);
}

fp_set* fp_set_from_text(const char* const* sigs, size_t count, fp_error* error) {
    size_t i, total = 0, error_at;
    fp_set* set;
    uint8_t *storage, *leaf;
    if (!sigs || !count) return fp_fail(error, FP_ERROR_ARGUMENT, 0, 0);
    for (i = 0; i < count; ++i) {
        size_t length;
        if (!sigs[i]) return fp_fail(error, FP_ERROR_ARGUMENT, i, 0);
        length = fp_parse(sigs[i], NULL, NULL, &error_at);
        if (error_at != SIZE_MAX) return fp_fail(error, FP_ERROR_SYNTAX, i, error_at);
        if (!length) return fp_fail(error, FP_ERROR_EMPTY, i, 0);
        if (length > SIZE_MAX - total) return fp_fail(error, FP_ERROR_MEMORY, i, 0);
        total += length;
    }
    set = fp_set_alloc(count, total, &storage, error);
    if (!set) return NULL;
    leaf = storage + 2 * total;
    for (i = 0; i < count; ++i) {
        const size_t length = fp_parse(sigs[i], storage, storage + total, &error_at);
        fp_set_plan(set, i, storage, length, storage + total, &leaf);
        storage += length;
    }
    return fp_set_finish(set, error);
}

/* Searches for signature index[0] to index[count - 1] one at a time, with
 * best the lowest offset found so far. Without offsets, only the lowest
 * matters, so each signature searches only the prefix where it could start
 * lower. Returns the lowest offset. */
static size_t fp_scan_each(const uint8_t* data, size_t size, const fp_set* set, const size_t* index,
                           size_t count, size_t* offsets, size_t best) {
    size_t i;
    for (i = 0; i < count; ++i) {
        const size_t sig = index ? index[i] : i;
        const fp_plan* plan = &fp_plans(set)[sig];
        size_t limit = size, pos;
        if (!offsets && best != SIZE_MAX) {
            if (best == 0) break;
            if (plan->length <= size && best - 1 < size - plan->length) limit = best - 1 + plan->length;
        }
        pos = fp_plan_find(data, limit, plan);
        if (offsets) offsets[sig] = pos;
        if (pos < best) best = pos;
    }
    return best;
}

#if defined(FP_X86)
/* Searches the group's members one by one once Teddy has verified every
 * window below from: a member's starts below from - offset are done. */
static size_t fp_teddy_rest(const uint8_t* data, size_t size, const fp_set* set, const fp_teddy* group,
                            size_t* offsets, size_t best, size_t from) {
    size_t m;
    for (m = group->start[0]; m < group->start[8]; ++m) {
        const fp_member* member = &set->members[group->first + m];
        const fp_plan* plan = &fp_plans(set)[member->sig];
        const size_t resume = from > member->offset ? from - member->offset : 0;
        size_t limit = size - resume, pos;
        if (offsets ? offsets[member->sig] != SIZE_MAX : best <= resume) continue;
        if (!offsets && best != SIZE_MAX && plan->length <= limit && best - 1 - resume < limit - plan->length)
            limit = best - 1 - resume + plan->length;
        pos = fp_plan_find(data + resume, limit, plan);
        if (pos == SIZE_MAX) continue;
        pos += resume;
        if (offsets) offsets[member->sig] = pos;
        if (pos < best) best = pos;
    }
    return best;
}

/* Scans for the members of one Teddy group, given best, the lowest offset
 * found so far, and returns the new lowest. With offsets, each member is
 * verified until found, and a bucket whose members are all found leaves the
 * tables, a copy local to this scan. Without, windows stop where no member
 * could start below best. */
static size_t fp_teddy_group(const uint8_t* data, size_t size, const fp_set* set, const fp_teddy* group,
                             size_t* offsets, size_t best, int backend) {
    const size_t width = fp_teddy_width(backend);
    fp_teddy_tables tables = group->tables;
    uint8_t buckets[64];
    unsigned left[8], live = 0, b;
    size_t i = 0, end = size - FP_TEDDY_LENGTH + 1, windows = 0, unfound = group->start[8];
    if (!offsets && best != SIZE_MAX) {
        if (best == 0) return best;
        if (best + group->reach < end) end = best + group->reach;
    }
    for (b = 0; b < 8; ++b) {
        left[b] = (unsigned)(group->start[b + 1] - group->start[b]);
        if (left[b]) live |= 1u << b;
    }
    while (i < end) {
        uint64_t lanes;
        const size_t base = fp_teddy_next(backend, data, i, end, &tables, &lanes, buckets);
        if (base == SIZE_MAX) break;
        for (; lanes; lanes &= lanes - 1) {
            const size_t window = base + fp_first_bit(lanes);
            uint64_t bits = buckets[window - base] & live;
            if (window >= end) break;
            ++windows;
            for (; bits; bits &= bits - 1) {
                const unsigned bucket = fp_first_bit(bits);
                size_t m, x;
                for (m = group->start[bucket]; m < group->start[bucket + 1]; ++m) {
                    const fp_member* member = &set->members[group->first + m];
                    const fp_plan* plan = &fp_plans(set)[member->sig];
                    size_t start;
                    if (window < member->offset) continue;
                    start = window - member->offset;
                    if (plan->length > size - start || (offsets && offsets[member->sig] != SIZE_MAX) ||
                        !fp_matches(data + start, plan)) {
                        continue;
                    }
                    if (start < best) best = start;
                    if (!offsets) {
                        if (best + group->reach < end) end = best + group->reach;
                        continue;
                    }
                    offsets[member->sig] = start;
                    --unfound;
                    if (--left[bucket] == 0) {
                        live &= ~(1u << bucket);
                        if (!live) return best;
                        for (x = 0; x < 16; ++x) tables.lo[0][x] &= (uint8_t)~(1u << bucket);
                    }
                }
            }
        }
        i = base + width;
        if (windows / unfound > (i > FP_TEDDY_WARMUP ? i : FP_TEDDY_WARMUP) / FP_TEDDY_COST)
            return fp_teddy_rest(data, size, set, group, offsets, best, i);
    }
    return best;
}

static size_t fp_scan_teddy(const uint8_t* data, size_t size, const fp_set* set, size_t* offsets, int backend) {
    size_t i, best = SIZE_MAX;
    if (offsets) {
        for (i = 0; i < set->count; ++i) offsets[i] = SIZE_MAX;
    }
    for (i = 0; i < set->groups; ++i) best = fp_teddy_group(data, size, set, &set->teddy[i], offsets, best, backend);
    return fp_scan_each(data, size, set, set->stray, set->strays, offsets, best);
}
#endif

/* A single signature with offsets, apart from fp_scan_all and the
 * registers its general paths save. */
FP_NOINLINE size_t fp_scan_one(const uint8_t* data, size_t size, const fp_set* set, size_t* offsets) {
    const size_t pos = fp_plan_find(data, size, fp_plans(set));
    *offsets = pos;
    return pos;
}

/* Sets of several signatures. */
FP_NOINLINE size_t fp_scan_all(const uint8_t* data, size_t size, const fp_set* set, size_t* offsets) {
#if defined(FP_X86)
    if (set->groups && size >= FP_TEDDY_SIZE) {
        const int backend = fp_get_backend();
        if (backend >= FP_SSSE3) return fp_scan_teddy(data, size, set, offsets, backend);
    }
#endif
    return fp_scan_each(data, size, set, NULL, set->count, offsets, SIZE_MAX);
}

size_t fp_scan(const uint8_t* data, size_t size, const fp_set* set, size_t* offsets) {
    if (!set) return SIZE_MAX;
    if (!data) size = 0;
    if (set->count == 1) return offsets ? fp_scan_one(data, size, set, offsets) : fp_plan_find(data, size, fp_plans(set));
    return fp_scan_all(data, size, set, offsets);
}

size_t fp_set_count(const fp_set* set) { return set ? set->count : 0; }

void fp_set_free(fp_set* set) { free(set); }

const char* fp_ver(void) { return "FastPattern " FASTPATTERN_VERSION; }

const char* fp_cpuinfo(void) {
    switch (fp_get_backend()) {
        case FP_AVX512: return "CPU backend: AVX-512BW";
        case FP_AVX2: return "CPU backend: AVX2";
        case FP_SSSE3: return "CPU backend: SSSE3";
        case FP_SSE2: return "CPU backend: SSE2";
        case FP_ARM_NEON: return "CPU backend: NEON";
        default: return "CPU backend: scalar";
    }
}
