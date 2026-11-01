#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif

#include "fastpattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

static unsigned long checks;

static void check_result(size_t actual, size_t expected, const char *label, int line)
{
    ++checks;
    if (actual != expected) {
        fprintf(stderr, "%s:%d: %s: expected %zu, got %zu\n",
                __FILE__, line, label, expected, actual);
        exit(EXIT_FAILURE);
    }
}

#define CHECK_EQ(actual, expected) check_result((actual), (expected), #actual, __LINE__)

/* Deliberately independent: compare each significant bit of each byte. */
static size_t reference_find(const uint8_t *data, size_t data_size,
                             const uint8_t *pattern, size_t pattern_size,
                             const uint8_t *mask)
{
    size_t offset;
    if (!data || !pattern || !pattern_size || pattern_size > data_size) {
        return SIZE_MAX;
    }
    for (offset = 0; offset <= data_size - pattern_size; ++offset) {
        size_t j;
        for (j = 0; j < pattern_size; ++j) {
            const unsigned significant = mask ? mask[j] : 255u;
            if (((unsigned)data[offset + j] ^ pattern[j]) & significant) {
                break;
            }
        }
        if (j == pattern_size) {
            return offset;
        }
    }
    return SIZE_MAX;
}

static uint32_t random_state = UINT32_C(0x1234abcd);

static uint32_t next_random(void)
{
    uint32_t value = random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    random_state = value;
    return value;
}

/* Every search is checked both as fp_scan's return value and through
 * offsets, which for one signature must agree. */
static size_t scan_set(const uint8_t *data, size_t size, const fp_set *set)
{
    size_t offset = 0;
    const size_t result = fp_scan(data, size, set, NULL);
    CHECK_EQ(fp_scan(data, size, set, &offset), result);
    CHECK_EQ(offset, result);
    return result;
}

static size_t find_bytes(const uint8_t *data, size_t size, const uint8_t *pattern, size_t length,
                         const uint8_t *mask)
{
    fp_sig sig;
    fp_set *set;
    size_t result;
    sig.bytes = pattern;
    sig.mask = mask;
    sig.length = length;
    set = fp_set_from_bytes(&sig, 1, NULL);
    if (!set) {
        return SIZE_MAX;
    }
    result = scan_set(data, size, set);
    fp_set_free(set);
    return result;
}

static fp_set *compile_text(const char *text)
{
    return fp_set_from_text(&text, 1, NULL);
}

static size_t find_text(const uint8_t *data, size_t size, const char *text)
{
    fp_set *set = compile_text(text);
    size_t result;
    if (!set) {
        return SIZE_MAX;
    }
    result = scan_set(data, size, set);
    fp_set_free(set);
    return result;
}

static void test_arguments(void)
{
    const uint8_t data[] = {0, 0x10, 0x20, 0x10, 0x20, 0xff};
    const uint8_t pattern[] = {0x10, 0x20};
    const uint8_t zero_mask[] = {0, 0};
    const uint8_t partial[] = {0xa7, 0x3c};
    const uint8_t partial_mask[] = {0x50, 0xc3};
    const uint8_t partial_data[] = {0xf2, 0xa7, 0x3c};
    const char *texts[2] = {"10 20", NULL};
    size_t offsets[2] = {7, 7};
    fp_sig sigs[2];
    fp_error error;
    fp_set *set;
    CHECK_EQ(find_bytes(NULL, sizeof(data), pattern, 2, NULL), SIZE_MAX);
    CHECK_EQ(find_bytes(data, 0, pattern, 2, NULL), SIZE_MAX);
    CHECK_EQ(find_bytes(data, 1, pattern, 2, NULL), SIZE_MAX);
    CHECK_EQ(find_bytes(data, sizeof(data), pattern, 2, NULL), 1);
    CHECK_EQ(find_bytes(data, sizeof(data), data, sizeof(data), NULL), 0);
    CHECK_EQ(find_bytes(data, sizeof(data), pattern, 2, zero_mask), 0);
    CHECK_EQ(find_bytes(data, 1, pattern, 2, zero_mask), SIZE_MAX);
    CHECK_EQ(find_bytes(partial_data, sizeof(partial_data), partial, 2, partial_mask),
             reference_find(partial_data, sizeof(partial_data), partial, 2, partial_mask));

    /* Construction failures name the signature and, for text, the token. */
    sigs[0].bytes = pattern;
    sigs[0].mask = NULL;
    sigs[0].length = 2;
    sigs[1] = sigs[0];
    CHECK_EQ(fp_set_from_bytes(NULL, 1, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_ARGUMENT);
    CHECK_EQ(fp_set_from_bytes(sigs, 0, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_ARGUMENT);
    sigs[1].bytes = NULL;
    CHECK_EQ(fp_set_from_bytes(sigs, 2, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_ARGUMENT);
    CHECK_EQ(error.index, 1);
    sigs[1].bytes = pattern;
    sigs[1].length = 0;
    CHECK_EQ(fp_set_from_bytes(sigs, 2, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_EMPTY);
    CHECK_EQ(error.index, 1);
    sigs[1].length = SIZE_MAX;
    CHECK_EQ(fp_set_from_bytes(sigs, 2, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_MEMORY);
    CHECK_EQ(error.index, 1);
    CHECK_EQ(fp_set_from_bytes(sigs, 2, NULL) == NULL, 1);
    CHECK_EQ(fp_set_from_text(NULL, 1, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_ARGUMENT);
    CHECK_EQ(fp_set_from_text(texts, 2, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_ARGUMENT);
    CHECK_EQ(error.index, 1);
    texts[1] = "  10 2G";
    CHECK_EQ(fp_set_from_text(texts, 2, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_SYNTAX);
    CHECK_EQ(error.index, 1);
    CHECK_EQ(error.position, 5);
    texts[1] = " \t";
    CHECK_EQ(fp_set_from_text(texts, 2, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_EMPTY);
    CHECK_EQ(error.index, 1);

    /* A set of two: the return value is the lowest offset of either. */
    texts[1] = "?? FF";
    set = fp_set_from_text(texts, 2, &error);
    CHECK_EQ(set != NULL, 1);
    CHECK_EQ(error.status, FP_OK);
    CHECK_EQ(fp_set_count(set), 2);
    CHECK_EQ(fp_scan(data, sizeof(data), set, offsets), 1);
    CHECK_EQ(offsets[0], 1);
    CHECK_EQ(offsets[1], 4);
    CHECK_EQ(fp_scan(data, sizeof(data), set, NULL), 1);
    CHECK_EQ(fp_scan(data + 2, sizeof(data) - 2, set, offsets), 1);
    CHECK_EQ(offsets[0], 1);
    CHECK_EQ(offsets[1], 2);
    CHECK_EQ(fp_scan(data + 4, 2, set, offsets), 0);
    CHECK_EQ(offsets[0], SIZE_MAX);
    CHECK_EQ(offsets[1], 0);
    CHECK_EQ(fp_scan(NULL, sizeof(data), set, offsets), SIZE_MAX);
    CHECK_EQ(offsets[0], SIZE_MAX);
    CHECK_EQ(offsets[1], SIZE_MAX);
    CHECK_EQ(fp_scan(data, 0, set, offsets), SIZE_MAX);
    fp_set_free(set);
    offsets[0] = 7;
    CHECK_EQ(fp_scan(data, sizeof(data), NULL, offsets), SIZE_MAX);
    CHECK_EQ(offsets[0], 7);
    CHECK_EQ(fp_set_count(NULL), 0);
    fp_set_free(NULL);
    CHECK_EQ(fp_ver() != NULL && fp_ver()[0] != '\0', 1);
    CHECK_EQ(fp_cpuinfo() != NULL && fp_cpuinfo()[0] != '\0', 1);
}

static void test_parser(void)
{
    static const char *const invalid[] = {
        "", " ", "\t\r\n\v\f", "A", "0", "0x10", "100", "GG", "1G", "G1",
        "10 GG", "10 2", "1020", "10,20", "10;20", "10:20", "10-20",
        "???", "??20", "?20", "A??", "?G", "G?", "10 ??Z", "+1", "-1", "10 A?x"
    };
    const uint8_t data[] = {0, 0x10, 0xab, 0x2f, 0xff, 0x10, 0x20};
    char high_bit[] = {'1', '0', ' ', (char)0xff, '\0'};
    size_t i;
    /* Status and position of a few, in the order of invalid[]. */
    static const struct {
        size_t index;
        fp_status status;
        size_t position;
    } reported[] = {
        {0, FP_ERROR_EMPTY, 0}, {1, FP_ERROR_EMPTY, 0}, {3, FP_ERROR_SYNTAX, 0},
        {5, FP_ERROR_SYNTAX, 0}, {10, FP_ERROR_SYNTAX, 3}, {11, FP_ERROR_SYNTAX, 3},
        {12, FP_ERROR_SYNTAX, 0}, {23, FP_ERROR_SYNTAX, 3}, {26, FP_ERROR_SYNTAX, 3}
    };
    const char *source;
    fp_error error;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK_EQ(find_text(data, sizeof(data), invalid[i]), SIZE_MAX);
        CHECK_EQ(compile_text(invalid[i]) == NULL, 1);
    }
    for (i = 0; i < sizeof(reported) / sizeof(reported[0]); ++i) {
        source = invalid[reported[i].index];
        CHECK_EQ(fp_set_from_text(&source, 1, &error) == NULL, 1);
        CHECK_EQ(error.status, reported[i].status);
        CHECK_EQ(error.index, 0);
        if (reported[i].status == FP_ERROR_SYNTAX) {
            CHECK_EQ(error.position, reported[i].position);
        }
    }
    CHECK_EQ(find_text(data, sizeof(data), high_bit), SIZE_MAX);
    source = high_bit;
    CHECK_EQ(fp_set_from_text(&source, 1, &error) == NULL, 1);
    CHECK_EQ(error.status, FP_ERROR_SYNTAX);
    CHECK_EQ(error.position, 3);
    CHECK_EQ(find_text(data, sizeof(data), "00"), 0);
    CHECK_EQ(find_text(data, sizeof(data), "10"), 1);
    CHECK_EQ(find_text(data, sizeof(data), "20"), 6);
    CHECK_EQ(find_text(data, sizeof(data), "AB 2f ff"), 2);
    CHECK_EQ(find_text(data, sizeof(data), "\t10\nAB\r2F\vFF\f"), 1);
    CHECK_EQ(find_text(data, sizeof(data), "  10  AB  "), 1);
    CHECK_EQ(find_text(data, sizeof(data), "10 ? 2F"), 1);
    CHECK_EQ(find_text(data, sizeof(data), "10 ?? 2F"), 1);
    CHECK_EQ(find_text(data, sizeof(data), "A? ?F"), 2);
    CHECK_EQ(find_text(data, sizeof(data), "a? ?f"), 2);
    CHECK_EQ(find_text(data, sizeof(data), "?"), 0);
    CHECK_EQ(find_text(data, sizeof(data), "??"), 0);
    CHECK_EQ(find_text(data, sizeof(data), "? ? ? ? ? ? ?"), 0);
    CHECK_EQ(find_text(data, sizeof(data), "? ? ? ? ? ? ? ?"), SIZE_MAX);
    CHECK_EQ(find_text(data, sizeof(data), "10 FF"), SIZE_MAX);
    /* A long signature, longer than the buffer in some searches. */
    {
        uint8_t large[600];
        char text[1801];
        memset(large, 0xa5, sizeof(large));
        for (i = 0; i < 600; ++i) {
            memcpy(text + 3 * i, "A5 ", 3);
        }
        text[1800] = '\0';
        CHECK_EQ(find_text(large, sizeof(large), text), 0);
        {
            fp_set *compiled = compile_text(text);
            CHECK_EQ(compiled != NULL, 1);
            CHECK_EQ(scan_set(large, sizeof(large), compiled), 0);
            CHECK_EQ(scan_set(large, sizeof(large) - 1, compiled), SIZE_MAX);
            large[599] = 0;
            CHECK_EQ(scan_set(large, sizeof(large), compiled), SIZE_MAX);
            fp_set_free(compiled);
        }
        text[1798] = 'G';
        CHECK_EQ(find_text(large, sizeof(large), text), SIZE_MAX);
        CHECK_EQ(compile_text(text) == NULL, 1);
    }
}

static void test_compiled_ownership(void)
{
    uint8_t data[] = {0, 0x10, 0xab, 0x10, 0xcb};
    char text[] = "10 ?B";
    fp_set *compiled = compile_text(text);
    fp_set *wildcard = compile_text("? ?? ?");
    CHECK_EQ(compiled != NULL, 1);
    CHECK_EQ(wildcard != NULL, 1);
    memset(text, 'G', sizeof(text) - 1);
    CHECK_EQ(scan_set(data, sizeof(data), compiled), 1);
    CHECK_EQ(scan_set(NULL, sizeof(data), compiled), SIZE_MAX);
    CHECK_EQ(scan_set(data, 0, compiled), SIZE_MAX);
    CHECK_EQ(scan_set(data, 1, compiled), SIZE_MAX);
    data[1] = 0;
    CHECK_EQ(scan_set(data, sizeof(data), compiled), 3);
    data[3] = 0;
    CHECK_EQ(scan_set(data, sizeof(data), compiled), SIZE_MAX);
    CHECK_EQ(scan_set(data, sizeof(data), wildcard), 0);
    CHECK_EQ(scan_set(data, 2, wildcard), SIZE_MAX);
    fp_set_free(compiled);
    fp_set_free(wildcard);
}

static void test_boundaries(void)
{
    static const size_t sizes[] = {
        1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 95, 96, 97,
        127, 128, 129, 255, 256, 257, 511, 512, 513, 1024
    };
    static const size_t pattern_sizes[] = {1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127};
    uint8_t storage[1024 + 64];
    uint8_t pattern[127];
    uint8_t mask[127];
    size_t a, b, alignment;
    memset(pattern, 0xa5, sizeof(pattern));
    for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]); ++a) {
        const size_t n = sizes[a];
        for (b = 0; b < sizeof(pattern_sizes) / sizeof(pattern_sizes[0]); ++b) {
            const size_t m = pattern_sizes[b];
            if (m > n) {
                continue;
            }
            for (alignment = 0; alignment < 64; alignment += 7) {
                uint8_t *data = storage + alignment;
                size_t location;
                memset(data, 0x11, n);
                CHECK_EQ(find_bytes(data, n, pattern, m, NULL), SIZE_MAX);
                /* Walk every candidate in short buffers, and all vector/tail
                 * boundaries in larger buffers. */
                for (location = 0; location <= n - m; ++location) {
                    if (n > 129 && location > 1 && location != n - m &&
                        location % 16 != 0 && location % 16 != 15 && location % 16 != 1) {
                        continue;
                    }
                    memset(data, 0x11, n);
                    memcpy(data + location, pattern, m);
                    CHECK_EQ(find_bytes(data, n, pattern, m, NULL), location);
                }
                /* Wildcard ends must not hide a constrained middle byte. */
                memset(mask, 0, m);
                mask[m / 2] = 0xff;
                memset(data, 0x11, n);
                data[n - m + m / 2] = 0xa5;
                CHECK_EQ(find_bytes(data, n, pattern, m, mask), n - m);
                /* Exercise first and last anchors with partial bit masks. */
                memset(mask, 0, m);
                mask[0] = 0xf0;
                mask[m - 1] |= 0x0f;
                CHECK_EQ(find_bytes(data, n, pattern, m, mask),
                         reference_find(data, n, pattern, m, mask));
            }
        }
    }
    /* Repeated candidate anchors fail internally before the earliest full hit. */
    memset(storage, 0xa5, sizeof(storage));
    memset(pattern, 0xa5, sizeof(pattern));
    pattern[7] = 0x5a;
    storage[518] = 0x5a;
    CHECK_EQ(find_bytes(storage, sizeof(storage), pattern, 17, NULL), 511);
}

static void test_dense_candidates(void)
{
    /* Both anchors match at every offset, so each vector block (including an
     * overlapping final block) holds many failing candidates before the hit. */
    static const size_t sizes[] = {
        3, 15, 16, 17, 18, 19, 20, 31, 32, 33, 34, 47, 48, 49, 63, 64, 65, 66,
        79, 95, 96, 97, 98, 127, 128, 129, 130, 255, 256, 257
    };
    static const size_t pattern_sizes[] = {3, 4, 9, 16, 17, 33};
    uint8_t data[257];
    uint8_t pattern[33];
    uint8_t mask[33];
    size_t a, b, location;
    for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]); ++a) {
        const size_t n = sizes[a];
        for (b = 0; b < sizeof(pattern_sizes) / sizeof(pattern_sizes[0]); ++b) {
            const size_t m = pattern_sizes[b];
            if (m > n) {
                continue;
            }
            memset(pattern, 0xa5, m);
            pattern[m / 2] = 0x5a;
            memset(mask, 0xff, m);
            mask[0] = 0xf0;
            mask[m - 1] = 0x0f;
            memset(data, 0xa5, n);
            CHECK_EQ(find_bytes(data, n, pattern, m, NULL), SIZE_MAX);
            CHECK_EQ(find_bytes(data, n, pattern, m, mask), SIZE_MAX);
            for (location = 0; location <= n - m; ++location) {
                memset(data, 0xa5, n);
                data[location + m / 2] = 0x5a;
                CHECK_EQ(find_bytes(data, n, pattern, m, NULL), location);
                CHECK_EQ(find_bytes(data, n, pattern, m, mask), location);
            }
        }
    }
}

static void test_long_exact(void)
{
    /* Long enough for the planned exact scan: early matches, unrolled blocks,
     * dense blocks narrowed byte by byte, and the final block. In the flood,
     * equal first and last pattern bytes match every data byte, so the second
     * anchor moves to the differing byte; in the alternating data, distinct
     * anchors pass at every even offset. Only the planted copy holds 0x33. */
    static const size_t sizes[] = {1100, 1151, 1152, 1153, 2047, 2048, 2049, 4111};
    static const size_t pattern_sizes[] = {2, 3, 4, 8, 12, 16, 17, 33, 64, 65};
    static uint8_t data[4111];
    uint8_t pattern[65];
    size_t a, b, kind, j, l;
    for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]); ++a) {
        const size_t n = sizes[a];
        for (b = 0; b < sizeof(pattern_sizes) / sizeof(pattern_sizes[0]); ++b) {
            const size_t m = pattern_sizes[b];
            const size_t locations[] = {
                0, 1, 31, 32, 63, 64, 65, 127, 128, 129, n / 2, n - m - 64, n - m - 1, n - m
            };
            for (kind = 0; kind < 2; ++kind) {
                for (j = 0; j < m; ++j) {
                    pattern[j] = (uint8_t)(kind && j % 2 ? 0x5a : 0xa5);
                }
                pattern[m / 2] = 0x33;
                for (l = 0; l < sizeof(locations) / sizeof(locations[0]); ++l) {
                    for (j = 0; j < n; ++j) {
                        data[j] = (uint8_t)(kind && j % 2 ? 0x5a : 0xa5);
                    }
                    if (l == 0) {
                        CHECK_EQ(find_bytes(data, n, pattern, m, NULL), SIZE_MAX);
                    }
                    memcpy(data + locations[l], pattern, m);
                    CHECK_EQ(find_bytes(data, n, pattern, m, NULL), locations[l]);
                }
            }
        }
    }
}

/* Writes a pattern with masks of 0, 0x0f, 0xf0, or 0xff as parser text. */
static void format_pattern(char *text, const uint8_t *pattern, const uint8_t *mask, size_t m)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i, written = 0;
    for (i = 0; i < m; ++i) {
        text[written++] = mask[i] & 0xf0 ? hex[pattern[i] >> 4] : '?';
        text[written++] = mask[i] & 0x0f ? hex[pattern[i] & 15] : '?';
        text[written++] = ' ';
    }
    text[written] = '\0';
}

static void test_masked_ends(void)
{
    /* Masked patterns whose first and last constrained bytes are exact are
     * searched like exact ones; compiled patterns trim wildcard ends first.
     * Walk matches through buffers around each size class (64, 128, and 1024
     * starts), in a flood of the anchor byte and in sparse data. Layouts:
     * a nibble and a wildcard inside, leading wildcards, trailing wildcards,
     * and wildcards between the first byte and the distinguishing one (none
     * for 3 bytes, so the mask is all 0xff). */
    static const size_t sizes[] = {
        8, 15, 16, 17, 23, 24, 31, 32, 33, 47, 63, 64, 65, 71, 72, 79, 80, 95, 96, 127, 128, 129,
        135, 136, 143, 191, 192, 255, 256, 1030, 1031, 1032, 1100
    };
    static const size_t pattern_sizes[] = {2, 3, 4, 8, 9, 12, 15, 16, 17, 24, 33};
    static uint8_t data[1100];
    uint8_t pattern[33];
    uint8_t mask[33];
    char text[3 * 33 + 1];
    size_t a, b, layout, background, location, l, j;
    for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]); ++a) {
        const size_t n = sizes[a];
        for (b = 0; b < sizeof(pattern_sizes) / sizeof(pattern_sizes[0]); ++b) {
            const size_t m = pattern_sizes[b];
            if (m > n) {
                continue;
            }
            for (layout = 0; layout < 4; ++layout) {
                fp_set *compiled;
                memset(pattern, 0xa5, m);
                pattern[m / 2] = 0x5a;
                memset(mask, 0xff, m);
                switch (layout) {
                case 0:
                    if (m < 4) {
                        continue;
                    }
                    mask[1] = 0x0f;
                    mask[m - 2] = 0;
                    break;
                case 1:
                    mask[0] = 0;
                    if (m > 3) {
                        mask[1] = 0;
                    }
                    break;
                case 2:
                    mask[m - 1] = 0;
                    if (m > 3) {
                        mask[m - 2] = 0;
                    }
                    break;
                default:
                    for (j = 1; j < m / 2; ++j) {
                        mask[j] = 0;
                    }
                    break;
                }
                format_pattern(text, pattern, mask, m);
                compiled = compile_text(text);
                CHECK_EQ(compiled != NULL, 1);
                for (background = 0; background < 2; ++background) {
                    const uint8_t fill = background ? 0x11 : 0xa5;
                    const size_t locations[] = {0, 1, 31, 32, 63, 64, 65, 127, 128, 129, n / 2, n - m - 1, n - m};
                    const size_t walked = n > 300 ? sizeof(locations) / sizeof(locations[0]) : n - m + 1;
                    size_t expected;
                    memset(data, fill, n);
                    expected = reference_find(data, n, pattern, m, mask);
                    CHECK_EQ(find_bytes(data, n, pattern, m, mask), expected);
                    CHECK_EQ(scan_set(data, n, compiled), expected);
                    for (l = 0; l < walked; ++l) {
                        location = n > 300 ? locations[l] : l;
                        memset(data, fill, n);
                        for (j = 0; j < m; ++j) {
                            /* Unconstrained bits differ from the pattern's. */
                            data[location + j] = (uint8_t)((pattern[j] & mask[j]) | (0x3c & (uint8_t)~mask[j]));
                        }
                        expected = reference_find(data, n, pattern, m, mask);
                        CHECK_EQ(find_bytes(data, n, pattern, m, mask), expected);
                        CHECK_EQ(scan_set(data, n, compiled), expected);
                    }
                    /* A copy cut short by the buffer end, by a trailing
                     * wildcard in layout 2, does not fit. */
                    memset(data, fill, n);
                    memcpy(data + n - m + 1, pattern, m - 1);
                    expected = reference_find(data, n, pattern, m, mask);
                    CHECK_EQ(find_bytes(data, n, pattern, m, mask), expected);
                    CHECK_EQ(scan_set(data, n, compiled), expected);
                }
                fp_set_free(compiled);
            }
        }
    }
}

/* Random patterns, masks, and buffers of up to max_size bytes; a nonzero
 * alphabet draws data and pattern bytes from that many values, giving dense
 * candidates and repeated matches. */
static void test_random_sizes(size_t iterations, size_t max_size, unsigned alphabet)
{
    static uint8_t storage[8192 + 64];
    uint8_t pattern[193];
    uint8_t mask[193];
    size_t iteration;
    for (iteration = 0; iteration < iterations; ++iteration) {
        const size_t n = next_random() % (max_size + 1);
        const size_t m = next_random() % 194;
        uint8_t *data = storage + next_random() % 64;
        const unsigned mode = next_random() % 6;
        const uint8_t *selected_mask = mode == 0 ? NULL : mask;
        size_t j;
        for (j = 0; j < n; ++j) {
            data[j] = (uint8_t)(alphabet ? next_random() % alphabet : next_random());
        }
        for (j = 0; j < m; ++j) {
            pattern[j] = (uint8_t)(alphabet ? next_random() % alphabet : next_random());
            switch (mode) {
            case 1: mask[j] = 0xff; break;
            case 2: mask[j] = next_random() % 2 ? 0xff : 0; break;
            case 3: mask[j] = (uint8_t)next_random(); break;
            case 4: mask[j] = 0; break;
            default: mask[j] = next_random() % 2 ? 0xf0 : 0x0f; break;
            }
        }
        if (m && m <= n && next_random() % 2) {
            const size_t location = next_random() % (n - m + 1);
            for (j = 0; j < m; ++j) {
                const uint8_t significant = selected_mask ? selected_mask[j] : 0xff;
                data[location + j] = (uint8_t)((data[location + j] & (uint8_t)~significant) |
                                               (pattern[j] & significant));
            }
        }
        CHECK_EQ(find_bytes(data, n, pattern, m, selected_mask),
                 reference_find(data, n, pattern, m, selected_mask));
    }
}

static void test_random(void)
{
    test_random_sizes(20000, 1024, 0);
    test_random_sizes(3000, 8192, 0);
    test_random_sizes(3000, 8192, 3);
    test_random_sizes(5000, 1024, 2);
}

/* Random sets of up to max_count signatures (masks drawn like
 * test_random_sizes, lengths up to 24) in buffers of up to max_size bytes,
 * some with members planted. Every offset is checked, the return value is
 * their minimum, and scanning without offsets agrees. */
static void test_random_sets(size_t iterations, size_t max_count, size_t max_size, unsigned alphabet)
{
    static uint8_t storage[16384 + 64];
    static uint8_t patterns[256][24];
    static uint8_t masks[256][24];
    static fp_sig sigs[256];
    static size_t offsets[256];
    static size_t expected[256];
    size_t iteration;
    for (iteration = 0; iteration < iterations; ++iteration) {
        const size_t count = 1 + next_random() % max_count;
        const size_t n = next_random() % (max_size + 1);
        uint8_t *data = storage + next_random() % 64;
        size_t i, j, lowest = SIZE_MAX;
        fp_set *set;
        for (j = 0; j < n; ++j) {
            data[j] = (uint8_t)(alphabet ? next_random() % alphabet : next_random());
        }
        for (i = 0; i < count; ++i) {
            const size_t m = 1 + next_random() % 24;
            const unsigned mode = next_random() % 6;
            for (j = 0; j < m; ++j) {
                patterns[i][j] = (uint8_t)(alphabet ? next_random() % alphabet : next_random());
                switch (mode) {
                case 0: case 1: masks[i][j] = 0xff; break;
                case 2: masks[i][j] = next_random() % 3 ? 0xff : 0; break;
                case 3: masks[i][j] = (uint8_t)next_random(); break;
                case 4: masks[i][j] = next_random() % 4 ? 0xff : next_random() % 2 ? 0xf0 : 0x0f; break;
                default: masks[i][j] = next_random() % 8 ? 0xff : 0; break;
                }
            }
            sigs[i].bytes = patterns[i];
            sigs[i].mask = mode == 0 ? NULL : masks[i];
            sigs[i].length = m;
            if (m <= n && next_random() % 3 == 0) {
                const size_t location = next_random() % (n - m + 1);
                for (j = 0; j < m; ++j) {
                    const uint8_t significant = sigs[i].mask ? masks[i][j] : 0xff;
                    data[location + j] = (uint8_t)((data[location + j] & (uint8_t)~significant) |
                                                   (patterns[i][j] & significant));
                }
            }
        }
        if (count > 1 && next_random() % 4 == 0) {
            sigs[count - 1] = sigs[next_random() % (count - 1)];
        }
        set = fp_set_from_bytes(sigs, count, NULL);
        CHECK_EQ(set != NULL, 1);
        for (i = 0; i < count; ++i) {
            expected[i] = reference_find(data, n, sigs[i].bytes, sigs[i].length, sigs[i].mask);
            offsets[i] = 7;
            if (expected[i] < lowest) {
                lowest = expected[i];
            }
        }
        CHECK_EQ(fp_scan(data, n, set, offsets), lowest);
        for (i = 0; i < count; ++i) {
            CHECK_EQ(offsets[i], expected[i]);
        }
        CHECK_EQ(fp_scan(data, n, set, NULL), lowest);
        fp_set_free(set);
    }
}

/* Signatures whose fingerprints lie far inside them, in sets large enough
 * for Teddy. Without offsets, a match found first may not be the lowest:
 * one starting earlier can have its fingerprint later. In a flood every
 * window is a candidate, so the group soon gives way to single searches,
 * which must resume where each member's starts were last covered; the
 * planted match moves across that point, which depends on the vector width
 * and the number of members. */
static void test_set_edges(void)
{
    static uint8_t data[8192];
    static const uint8_t rare_a[] = {0x9a, 0xa9, 0x6a, 0x9d};
    static const uint8_t absent[4][4] = {
        {0x92, 0xa6, 0x9b, 0xa2}, {0x9b, 0x92, 0xa2, 0xa6}, {0xa2, 0x9b, 0xa6, 0x92}, {0xa6, 0xa2, 0x92, 0x9b}
    };
    static const uint8_t floods[5][8] = {
        {0x41, 0x41, 0x41, 0x41, 0x41, 0x00}, {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x00},
        {0x41, 0x41, 0x41, 0x41, 0x00}, {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x00},
        {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x00}
    };
    static const size_t flood_lengths[5] = {6, 7, 5, 8, 8};
    uint8_t late[21], late_mask[21];
    fp_sig sigs[6];
    size_t offsets[6], planted, i;
    fp_set *set;
    /* late: 16 wildcards, then A5 A1 AD 9E; it starts 10 before rare_a. */
    memset(late, 0, sizeof(late));
    memset(late_mask, 0, sizeof(late_mask));
    late[16] = 0xa5;
    late[17] = 0xa1;
    late[18] = 0xad;
    late[19] = 0x9e;
    memset(late_mask + 16, 0xff, 4);
    sigs[0].bytes = rare_a;
    sigs[0].mask = NULL;
    sigs[0].length = sizeof(rare_a);
    sigs[1].bytes = late;
    sigs[1].mask = late_mask;
    sigs[1].length = 20;
    for (i = 2; i < 6; ++i) {
        sigs[i].bytes = absent[i - 2];
        sigs[i].mask = NULL;
        sigs[i].length = 4;
    }
    memset(data, 0x11, 256);
    memcpy(data + 100, rare_a, sizeof(rare_a));
    memcpy(data + 106, late + 16, 4);
    set = fp_set_from_bytes(sigs, 6, NULL);
    CHECK_EQ(set != NULL, 1);
    CHECK_EQ(fp_scan(data, 256, set, NULL), 90);
    CHECK_EQ(fp_scan(data, 256, set, offsets), 90);
    CHECK_EQ(offsets[0], 100);
    CHECK_EQ(offsets[1], 90);
    fp_set_free(set);

    /* late now ends 41 41 41 41 7E; five signatures of 41s and an absent
     * 00 make every window of a 41 flood a candidate. */
    memset(late + 16, 0x41, 4);
    late[20] = 0x7e;
    late_mask[20] = 0xff;
    sigs[1].length = 21;
    sigs[0].bytes = floods[0];
    sigs[0].length = flood_lengths[0];
    for (i = 2; i < 6; ++i) {
        sigs[i].bytes = floods[i - 1];
        sigs[i].length = flood_lengths[i - 1];
    }
    set = fp_set_from_bytes(sigs, 6, NULL);
    CHECK_EQ(set != NULL, 1);
    for (planted = 2000; planted < 4500; ++planted) {
        memset(data, 0x41, sizeof(data));
        data[planted] = 0x7e;
        CHECK_EQ(fp_scan(data, sizeof(data), set, offsets), planted - 20);
        for (i = 0; i < 6; ++i) {
            CHECK_EQ(offsets[i], i == 1 ? planted - 20 : SIZE_MAX);
        }
        CHECK_EQ(fp_scan(data, sizeof(data), set, NULL), planted - 20);
    }
    fp_set_free(set);
}

static void test_sets(void)
{
    test_set_edges();
    test_random_sets(3000, 4, 300, 0);
    test_random_sets(1000, 40, 4096, 0);
    test_random_sets(1000, 40, 4096, 4);
    test_random_sets(150, 256, 16384, 0);
    test_random_sets(150, 256, 16384, 16);
    /* Two symbols: every window is a candidate, and groups give way to single searches. */
    test_random_sets(300, 40, 16384, 2);
}

static void test_random_parser(void)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t data[257];
    uint8_t pattern[80];
    uint8_t mask[80];
    char text[321];
    size_t iteration;
    for (iteration = 0; iteration < 2000; ++iteration) {
        const size_t n = next_random() % 258;
        const size_t m = 1 + next_random() % 80;
        size_t i, written = 0;
        for (i = 0; i < n; ++i) {
            data[i] = (uint8_t)next_random();
        }
        for (i = 0; i < m; ++i) {
            const unsigned kind = next_random() % 4;
            pattern[i] = (uint8_t)next_random();
            mask[i] = kind == 0 ? 0xff : kind == 1 ? 0xf0 : kind == 2 ? 0x0f : 0;
            text[written++] = mask[i] & 0xf0 ? hex[pattern[i] >> 4] : '?';
            text[written++] = mask[i] & 0x0f ? hex[pattern[i] & 15] : '?';
            text[written++] = i % 2 ? '\t' : ' ';
        }
        text[written] = '\0';
        if (m <= n && iteration % 2) {
            const size_t location = next_random() % (n - m + 1);
            memcpy(data + location, pattern, m);
        }
        CHECK_EQ(find_text(data, n, text), reference_find(data, n, pattern, m, mask));
        {
            fp_set *compiled = compile_text(text);
            CHECK_EQ(compiled != NULL, 1);
            CHECK_EQ(scan_set(data, n, compiled), reference_find(data, n, pattern, m, mask));
            fp_set_free(compiled);
        }
    }
}

#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
typedef struct {
    uint8_t *allocation;
    uint8_t *page;
    size_t page_size;
} guarded_page;

static guarded_page allocate_guarded_page(void)
{
    guarded_page region;
#if defined(_WIN32)
    SYSTEM_INFO info;
    DWORD old_protection;
    GetSystemInfo(&info);
    region.page_size = info.dwPageSize;
    region.allocation = (uint8_t *)VirtualAlloc(NULL, 3 * region.page_size,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    CHECK_EQ(region.allocation != NULL, 1);
    region.page = region.allocation + region.page_size;
    CHECK_EQ(VirtualProtect(region.page, region.page_size, PAGE_READWRITE, &old_protection) != 0, 1);
#else
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK_EQ(page_size > 0, 1);
    region.page_size = (size_t)page_size;
#if defined(MAP_ANONYMOUS)
    region.allocation = (uint8_t *)mmap(NULL, 3 * region.page_size, PROT_NONE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    region.allocation = (uint8_t *)mmap(NULL, 3 * region.page_size, PROT_NONE,
                                       MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    CHECK_EQ(region.allocation != MAP_FAILED, 1);
    region.page = region.allocation + region.page_size;
    CHECK_EQ(mprotect(region.page, region.page_size, PROT_READ | PROT_WRITE) == 0, 1);
#endif
    return region;
}

static void free_guarded_page(guarded_page region)
{
#if defined(_WIN32)
    CHECK_EQ(VirtualFree(region.allocation, 0, MEM_RELEASE) != 0, 1);
#else
    CHECK_EQ(munmap(region.allocation, 3 * region.page_size) == 0, 1);
#endif
}

static void test_guard_pages(void)
{
    static const size_t sizes[] = {1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257,
                                   1100, 2049, 4096};
    static char pattern_text[3 * 4096 + 1];
    guarded_page data_region = allocate_guarded_page();
    guarded_page pattern_region = allocate_guarded_page();
    guarded_page mask_region = allocate_guarded_page();
    size_t a, b;
    unsigned edge;
    for (edge = 0; edge < 2; ++edge) {
        for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]) && sizes[a] <= data_region.page_size; ++a) {
            const size_t n = sizes[a];
            uint8_t *data = data_region.page + (edge ? data_region.page_size - n : 0);
            for (b = 0; b <= a; ++b) {
                const size_t m = sizes[b];
                uint8_t *pattern = pattern_region.page + (edge ? pattern_region.page_size - m : 0);
                uint8_t *mask = mask_region.page + (edge ? mask_region.page_size - m : 0);
                memset(data, 0x11, n);
                memset(pattern, 0xa5, m);
                memset(mask, 0xff, m);
                CHECK_EQ(find_bytes(data, n, pattern, m, NULL), SIZE_MAX);
                CHECK_EQ(find_bytes(data, n, pattern, m, mask), SIZE_MAX);
                memcpy(data + n - m, pattern, m);
                CHECK_EQ(find_bytes(data, n, pattern, m, NULL), n - m);
                CHECK_EQ(find_bytes(data, n, pattern, m, mask), n - m);
                if (m >= 3) {
                    fp_set *compiled;
                    mask[m / 2] = 0;
                    CHECK_EQ(find_bytes(data, n, pattern, m, mask), n - m);
                    mask[m - 1] = 0;
                    CHECK_EQ(find_bytes(data, n, pattern, m, mask), reference_find(data, n, pattern, m, mask));
                    format_pattern(pattern_text, pattern, mask, m);
                    compiled = compile_text(pattern_text);
                    CHECK_EQ(compiled != NULL, 1);
                    CHECK_EQ(scan_set(data, n, compiled), reference_find(data, n, pattern, m, mask));
                    fp_set_free(compiled);
                }
                memset(mask, 0, m);
                mask[m / 2] = 0xff;
                CHECK_EQ(find_bytes(data, n, pattern, m, mask), reference_find(data, n, pattern, m, mask));
                memset(mask, 0, m);
                CHECK_EQ(find_bytes(data, n, pattern, m, mask), 0);
            }
        }
    }
    /* Sets at the end of the page: suffixes of the buffer's last 12 bytes,
     * and one that would match only by reading a byte past the end. */
    for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]) && sizes[a] <= data_region.page_size; ++a) {
        const size_t n = sizes[a];
        uint8_t *data = data_region.page + data_region.page_size - n;
        uint8_t tail[13];
        fp_sig sigs[9];
        size_t offsets[9], j;
        fp_set *set;
        if (n < 12) {
            continue;
        }
        memset(data, 0x11, n);
        for (j = 0; j < 12; ++j) {
            data[n - 12 + j] = (uint8_t)(0x91 + 7 * j);
        }
        memcpy(tail, data + n - 12, 12);
        tail[12] = 0x77;
        for (j = 0; j < 8; ++j) {
            sigs[j].bytes = tail + j;
            sigs[j].mask = NULL;
            sigs[j].length = 12 - j;
        }
        sigs[8].bytes = tail + 4;
        sigs[8].mask = NULL;
        sigs[8].length = 9;
        set = fp_set_from_bytes(sigs, 9, NULL);
        CHECK_EQ(set != NULL, 1);
        CHECK_EQ(fp_scan(data, n, set, offsets), n - 12);
        for (j = 0; j < 8; ++j) {
            CHECK_EQ(offsets[j], n - 12 + j);
        }
        CHECK_EQ(offsets[8], SIZE_MAX);
        CHECK_EQ(fp_scan(data, n, set, NULL), n - 12);
        fp_set_free(set);
    }
    /* The parser may inspect a token and its terminator, but nothing beyond it. */
    {
        char *text = (char *)(pattern_region.page + pattern_region.page_size - 3);
        uint8_t data = 0xa5;
        memcpy(text, "A5", 3);
        CHECK_EQ(find_text(&data, 1, text), 0);
        memcpy(text, "A?", 3);
        CHECK_EQ(find_text(&data, 1, text), 0);
        text[1] = '\0';
        CHECK_EQ(find_text(&data, 1, text), SIZE_MAX);
    }
    free_guarded_page(data_region);
    free_guarded_page(pattern_region);
    free_guarded_page(mask_region);
}
#else
static void test_guard_pages(void)
{
    puts("Guard-page checks skipped: virtual memory API unavailable.");
}
#endif

int main(void)
{
    test_arguments();
    test_parser();
    test_compiled_ownership();
    test_boundaries();
    test_dense_candidates();
    test_long_exact();
    test_masked_ends();
    test_random();
    test_sets();
    test_random_parser();
    test_guard_pages();
    printf("Passed %lu checks; %s\n", checks, fp_cpuinfo());
    return EXIT_SUCCESS;
}
