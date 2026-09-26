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

static void test_arguments(void)
{
    const uint8_t data[] = {0, 0x10, 0x20, 0x10, 0x20, 0xff};
    const uint8_t pattern[] = {0x10, 0x20};
    const uint8_t zero_mask[] = {0, 0};
    const uint8_t partial[] = {0xa7, 0x3c};
    const uint8_t partial_mask[] = {0x50, 0xc3};
    const uint8_t partial_data[] = {0xf2, 0xa7, 0x3c};
    CHECK_EQ(fp_find(NULL, sizeof(data), pattern, 2, NULL), SIZE_MAX);
    CHECK_EQ(fp_find(data, sizeof(data), NULL, 2, NULL), SIZE_MAX);
    CHECK_EQ(fp_find(data, 0, pattern, 2, NULL), SIZE_MAX);
    CHECK_EQ(fp_find(data, sizeof(data), pattern, 0, NULL), SIZE_MAX);
    CHECK_EQ(fp_find(data, 1, pattern, 2, NULL), SIZE_MAX);
    CHECK_EQ(fp_find(data, sizeof(data), pattern, SIZE_MAX, NULL), SIZE_MAX);
    CHECK_EQ(fp_find(data, sizeof(data), pattern, 2, NULL), 1);
    CHECK_EQ(fp_find(data, sizeof(data), data, sizeof(data), NULL), 0);
    CHECK_EQ(fp_find(data, sizeof(data), pattern, 2, zero_mask), 0);
    CHECK_EQ(fp_find(data, 1, pattern, 2, zero_mask), SIZE_MAX);
    CHECK_EQ(fp_find(partial_data, sizeof(partial_data), partial, 2, partial_mask),
             reference_find(partial_data, sizeof(partial_data), partial, 2, partial_mask));
    CHECK_EQ(fp_find_pattern(data, sizeof(data), NULL), SIZE_MAX);
    CHECK_EQ(fp_find_pattern(NULL, sizeof(data), "10"), SIZE_MAX);
    CHECK_EQ(fp_find_pattern(data, 0, "10"), SIZE_MAX);
    CHECK_EQ(fp_ver() != NULL && fp_ver()[0] != '\0', 1);
    CHECK_EQ(fp_cpuinfo() != NULL && fp_cpuinfo()[0] != '\0', 1);
    CHECK_EQ(fp_compile(NULL) == NULL, 1);
    CHECK_EQ(fp_find_compiled(data, sizeof(data), NULL), SIZE_MAX);
    fp_free(NULL);
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
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK_EQ(fp_find_pattern(data, sizeof(data), invalid[i]), SIZE_MAX);
        CHECK_EQ(fp_compile(invalid[i]) == NULL, 1);
    }
    CHECK_EQ(fp_find_pattern(data, sizeof(data), high_bit), SIZE_MAX);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "00"), 0);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "10"), 1);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "20"), 6);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "AB 2f ff"), 2);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "\t10\nAB\r2F\vFF\f"), 1);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "  10  AB  "), 1);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "10 ? 2F"), 1);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "10 ?? 2F"), 1);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "A? ?F"), 2);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "a? ?f"), 2);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "?"), 0);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "??"), 0);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "? ? ? ? ? ? ?"), 0);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "? ? ? ? ? ? ? ?"), SIZE_MAX);
    CHECK_EQ(fp_find_pattern(data, sizeof(data), "10 FF"), SIZE_MAX);
    /* A long input also exercises the parser's heap allocation path. */
    {
        uint8_t large[600];
        char text[1801];
        memset(large, 0xa5, sizeof(large));
        for (i = 0; i < 600; ++i) {
            memcpy(text + 3 * i, "A5 ", 3);
        }
        text[1800] = '\0';
        CHECK_EQ(fp_find_pattern(large, sizeof(large), text), 0);
        {
            fp_pattern *compiled = fp_compile(text);
            CHECK_EQ(compiled != NULL, 1);
            CHECK_EQ(fp_find_compiled(large, sizeof(large), compiled), 0);
            CHECK_EQ(fp_find_compiled(large, sizeof(large) - 1, compiled), SIZE_MAX);
            large[599] = 0;
            CHECK_EQ(fp_find_compiled(large, sizeof(large), compiled), SIZE_MAX);
            fp_free(compiled);
        }
        text[1798] = 'G';
        CHECK_EQ(fp_find_pattern(large, sizeof(large), text), SIZE_MAX);
        CHECK_EQ(fp_compile(text) == NULL, 1);
    }
}

static void test_compiled_ownership(void)
{
    uint8_t data[] = {0, 0x10, 0xab, 0x10, 0xcb};
    char text[] = "10 ?B";
    fp_pattern *compiled = fp_compile(text);
    fp_pattern *wildcard = fp_compile("? ?? ?");
    CHECK_EQ(compiled != NULL, 1);
    CHECK_EQ(wildcard != NULL, 1);
    memset(text, 'G', sizeof(text) - 1);
    CHECK_EQ(fp_find_compiled(data, sizeof(data), compiled), 1);
    CHECK_EQ(fp_find_compiled(NULL, sizeof(data), compiled), SIZE_MAX);
    CHECK_EQ(fp_find_compiled(data, 0, compiled), SIZE_MAX);
    CHECK_EQ(fp_find_compiled(data, 1, compiled), SIZE_MAX);
    data[1] = 0;
    CHECK_EQ(fp_find_compiled(data, sizeof(data), compiled), 3);
    data[3] = 0;
    CHECK_EQ(fp_find_compiled(data, sizeof(data), compiled), SIZE_MAX);
    CHECK_EQ(fp_find_compiled(data, sizeof(data), wildcard), 0);
    CHECK_EQ(fp_find_compiled(data, 2, wildcard), SIZE_MAX);
    fp_free(compiled);
    fp_free(wildcard);
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
                CHECK_EQ(fp_find(data, n, pattern, m, NULL), SIZE_MAX);
                /* Walk every candidate in short buffers, and all vector/tail
                 * boundaries in larger buffers. */
                for (location = 0; location <= n - m; ++location) {
                    if (n > 129 && location > 1 && location != n - m &&
                        location % 16 != 0 && location % 16 != 15 && location % 16 != 1) {
                        continue;
                    }
                    memset(data, 0x11, n);
                    memcpy(data + location, pattern, m);
                    CHECK_EQ(fp_find(data, n, pattern, m, NULL), location);
                }
                /* Wildcard ends must not hide a constrained middle byte. */
                memset(mask, 0, m);
                mask[m / 2] = 0xff;
                memset(data, 0x11, n);
                data[n - m + m / 2] = 0xa5;
                CHECK_EQ(fp_find(data, n, pattern, m, mask), n - m);
                /* Exercise first and last anchors with partial bit masks. */
                memset(mask, 0, m);
                mask[0] = 0xf0;
                mask[m - 1] |= 0x0f;
                CHECK_EQ(fp_find(data, n, pattern, m, mask),
                         reference_find(data, n, pattern, m, mask));
            }
        }
    }
    /* Repeated candidate anchors fail internally before the earliest full hit. */
    memset(storage, 0xa5, sizeof(storage));
    memset(pattern, 0xa5, sizeof(pattern));
    pattern[7] = 0x5a;
    storage[518] = 0x5a;
    CHECK_EQ(fp_find(storage, sizeof(storage), pattern, 17, NULL), 511);
}

static void test_random(void)
{
    uint8_t storage[1024 + 64];
    uint8_t pattern[193];
    uint8_t mask[193];
    size_t iteration;
    for (iteration = 0; iteration < 20000; ++iteration) {
        const size_t n = next_random() % 1025;
        const size_t m = next_random() % 194;
        uint8_t *data = storage + next_random() % 64;
        const unsigned mode = next_random() % 6;
        const uint8_t *selected_mask = mode == 0 ? NULL : mask;
        size_t j;
        for (j = 0; j < n; ++j) {
            data[j] = (uint8_t)next_random();
        }
        for (j = 0; j < m; ++j) {
            pattern[j] = (uint8_t)next_random();
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
        CHECK_EQ(fp_find(data, n, pattern, m, selected_mask),
                 reference_find(data, n, pattern, m, selected_mask));
    }
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
        CHECK_EQ(fp_find_pattern(data, n, text), reference_find(data, n, pattern, m, mask));
        {
            fp_pattern *compiled = fp_compile(text);
            CHECK_EQ(compiled != NULL, 1);
            CHECK_EQ(fp_find_compiled(data, n, compiled), reference_find(data, n, pattern, m, mask));
            fp_free(compiled);
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
    static const size_t sizes[] = {1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257};
    guarded_page data_region = allocate_guarded_page();
    guarded_page pattern_region = allocate_guarded_page();
    guarded_page mask_region = allocate_guarded_page();
    size_t a, b;
    unsigned edge;
    for (edge = 0; edge < 2; ++edge) {
        for (a = 0; a < sizeof(sizes) / sizeof(sizes[0]); ++a) {
            const size_t n = sizes[a];
            uint8_t *data = data_region.page + (edge ? data_region.page_size - n : 0);
            for (b = 0; b <= a; ++b) {
                const size_t m = sizes[b];
                uint8_t *pattern = pattern_region.page + (edge ? pattern_region.page_size - m : 0);
                uint8_t *mask = mask_region.page + (edge ? mask_region.page_size - m : 0);
                memset(data, 0x11, n);
                memset(pattern, 0xa5, m);
                memset(mask, 0xff, m);
                CHECK_EQ(fp_find(data, n, pattern, m, NULL), SIZE_MAX);
                CHECK_EQ(fp_find(data, n, pattern, m, mask), SIZE_MAX);
                memcpy(data + n - m, pattern, m);
                CHECK_EQ(fp_find(data, n, pattern, m, NULL), n - m);
                CHECK_EQ(fp_find(data, n, pattern, m, mask), n - m);
                memset(mask, 0, m);
                mask[m / 2] = 0xff;
                CHECK_EQ(fp_find(data, n, pattern, m, mask), reference_find(data, n, pattern, m, mask));
                memset(mask, 0, m);
                CHECK_EQ(fp_find(data, n, pattern, m, mask), 0);
            }
        }
    }
    /* The parser may inspect a token and its terminator, but nothing beyond it. */
    {
        char *text = (char *)(pattern_region.page + pattern_region.page_size - 3);
        uint8_t data = 0xa5;
        memcpy(text, "A5", 3);
        CHECK_EQ(fp_find_pattern(&data, 1, text), 0);
        memcpy(text, "A?", 3);
        CHECK_EQ(fp_find_pattern(&data, 1, text), 0);
        text[1] = '\0';
        CHECK_EQ(fp_find_pattern(&data, 1, text), SIZE_MAX);
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
    test_random();
    test_random_parser();
    test_guard_pages();
    printf("Passed %lu checks; %s\n", checks, fp_cpuinfo());
    return EXIT_SUCCESS;
}
