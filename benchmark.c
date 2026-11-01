#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "fastpattern.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Deterministic fixtures, independent correctness checks, then warm-cache timing. */
enum { NEEDLE_SIZE = 8, TIMING_SAMPLES = 3, MAX_SIGS = 256 };

struct bench_case {
    const uint8_t *data;
    size_t size;
    uint8_t needle[NEEDLE_SIZE];
    uint8_t mask[NEEDLE_SIZE];
    int masked;
    char expression[NEEDLE_SIZE * 3 + 1];
    const char *text;
    fp_set *set;
    /* Several signatures: one set of them all, and a set of one for each. */
    size_t count;
    fp_set *singles[MAX_SIGS];
    size_t offsets[MAX_SIGS];
};

typedef size_t (*search_function)(struct bench_case *);
static volatile size_t result_sink;

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) {
        fprintf(stderr, "Performance timer unavailable\n");
        exit(EXIT_FAILURE);
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static size_t reference_find(const uint8_t *data, size_t size, const uint8_t *needle,
                             const uint8_t *mask, size_t length)
{
    size_t i, j;
    if (size < length) return SIZE_MAX;
    for (i = 0; i <= size - length; ++i) {
        for (j = 0; j < length; ++j) {
            const uint8_t m = mask ? mask[j] : UINT8_MAX;
            if ((data[i + j] & m) != (needle[j] & m)) break;
        }
        if (j == length) return i;
    }
    return SIZE_MAX;
}

static size_t reference_search(struct bench_case *c)
{
    return reference_find(c->data, c->size, c->needle, c->masked ? c->mask : NULL, NEEDLE_SIZE);
}

static size_t scan_search(struct bench_case *c)
{
    return fp_scan(c->data, c->size, c->set, NULL);
}

static size_t offsets_search(struct bench_case *c)
{
    return fp_scan(c->data, c->size, c->set, c->offsets);
}

static size_t build_search(struct bench_case *c)
{
    fp_set *set = fp_set_from_text(&c->text, 1, NULL);
    const size_t result = fp_scan(c->data, c->size, set, NULL);
    fp_set_free(set);
    return result;
}

/* Each signature on its own, collecting the lowest offset. */
static size_t singles_search(struct bench_case *c)
{
    size_t i, lowest = SIZE_MAX;
    for (i = 0; i < c->count; ++i) {
        const size_t offset = fp_scan(c->data, c->size, c->singles[i], NULL);
        c->offsets[i] = offset;
        if (offset < lowest) lowest = offset;
    }
    return lowest;
}

static double time_batch(search_function function, struct bench_case *c, size_t iterations)
{
    /* A volatile indirect call also prevents hoisting in whole-program builds. */
    search_function volatile invoke = function;
    const double start = now_seconds();
    size_t i;
    for (i = 0; i < iterations; ++i) result_sink ^= invoke(c);
    return now_seconds() - start;
}

static double median_latency(search_function function, struct bench_case *c, size_t *iterations)
{
    double samples[TIMING_SAMPLES], elapsed;
    size_t count = 1;
    int i, j;
    do {
        elapsed = time_batch(function, c, count);
        if (elapsed >= 0.01 || count >= ((size_t)1 << 24)) break;
        count *= 2;
    } while (1);
    for (i = 0; i < TIMING_SAMPLES; ++i) {
        samples[i] = time_batch(function, c, count) / (double)count;
    }
    for (i = 1; i < TIMING_SAMPLES; ++i) {
        for (j = i; j > 0 && samples[j] < samples[j - 1]; --j) {
            const double tmp = samples[j];
            samples[j] = samples[j - 1];
            samples[j - 1] = tmp;
        }
    }
    *iterations = count;
    return samples[TIMING_SAMPLES / 2];
}

static void format_signature(char *text, const uint8_t *bytes, const uint8_t *mask, size_t length)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i, used = 0;
    for (i = 0; i < length; ++i) {
        const uint8_t m = mask ? mask[i] : UINT8_MAX;
        if (i != 0) text[used++] = ' ';
        if (m == 0 && i % 2 == 0) {
            text[used++] = '?';
        } else {
            text[used++] = (m & 0xf0) ? hex[bytes[i] >> 4] : '?';
            text[used++] = (m & 0x0f) ? hex[bytes[i] & 0x0f] : '?';
        }
    }
    text[used] = '\0';
}

static void fill_background(uint8_t *data, size_t size, int dataset)
{
    static const char short_text[] = "short-text-for-substring-search!!";
    uint32_t state = UINT32_C(0x4d595df4);
    size_t i;
    for (i = 0; i < size; ++i) {
        if (dataset == 0) {
            data[i] = (uint8_t)short_text[i % (sizeof(short_text) - 1)];
        } else if (dataset == 1) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            data[i] = (uint8_t)state;
        } else {
            data[i] = 0x41;
        }
    }
}

static void print_row(const char *dataset, const char *pattern, const char *where, size_t size,
                      const char *result, const char *method, double seconds, size_t iterations,
                      size_t span)
{
    printf("%-11s %-8s %-5s %9zu %8s %-10s %11.2f %9zu ", dataset, pattern, where, size, result,
           method, seconds * 1e9, iterations);
    if (span == 0) printf("       n/a\n");
    else printf("%10.3f\n", (double)span / seconds / (1024.0 * 1024.0 * 1024.0));
}

static int run_case(uint8_t *data, size_t size, int dataset, int pattern_kind, int position)
{
    static const char *const dataset_names[] = {"short-text", "random", "repetitive"};
    static const char *const pattern_names[] = {"exact", "masked", "wildcard"};
    static const char *const position_names[] = {"begin", "end", "miss"};
    static const char *const method_names[] = {"reference", "scan", "offsets", "build+scan"};
    static const search_function methods[] = {
        reference_search, scan_search, offsets_search, build_search
    };
    static struct bench_case c;
    size_t i, expected, reference, search_span;
    char result_text[32];
    memset(&c, 0, sizeof(c));
    c.data = data;
    c.size = size;
    c.masked = pattern_kind != 0;
    memset(c.needle, 0x41, sizeof(c.needle));
    c.needle[NEEDLE_SIZE - 1] = 0x5a;
    memset(c.mask, 0xff, sizeof(c.mask));
    if (pattern_kind == 1) {
        c.mask[1] = 0xf0;
        c.mask[2] = 0;
        c.mask[4] = 0x0f;
    } else if (pattern_kind == 2) {
        memset(c.mask, 0, sizeof(c.mask));
    }
    fill_background(data, size, dataset);
    expected = position == 0 ? 0 : position == 1 ? size - NEEDLE_SIZE : SIZE_MAX;
    if (pattern_kind == 2) expected = 0;
    if (expected != SIZE_MAX && pattern_kind != 2) {
        for (i = 0; i < NEEDLE_SIZE; ++i) {
            const uint8_t m = c.masked ? c.mask[i] : UINT8_MAX;
            data[expected + i] = (uint8_t)((c.needle[i] & m) | (0xa5 & (uint8_t)~m));
        }
    }
    format_signature(c.expression, c.needle, c.masked ? c.mask : NULL, NEEDLE_SIZE);
    c.text = c.expression;
    c.set = fp_set_from_text(&c.text, 1, NULL);
    if (c.set == NULL) {
        fprintf(stderr, "Could not compile benchmark signature: %s\n", c.expression);
        return 0;
    }
    reference = reference_search(&c);
    if (reference != expected) {
        fprintf(stderr, "Fixture mismatch: %s/%s/%s, expected %zu, got %zu\n",
            dataset_names[dataset], pattern_names[pattern_kind],
            position_names[position], expected, reference);
        fp_set_free(c.set);
        return 0;
    }
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        const size_t result = methods[i](&c);
        if (result != reference || (i == 2 && c.offsets[0] != reference)) {
            fprintf(stderr, "Correctness failure: %s/%s/%s via %s, expected %zu, got %zu\n",
                dataset_names[dataset], pattern_names[pattern_kind],
                position_names[position], method_names[i], reference, result);
            fp_set_free(c.set);
            return 0;
        }
    }
    if (expected == SIZE_MAX) strcpy(result_text, "miss");
    else (void)snprintf(result_text, sizeof(result_text), "%zu", expected);
    /* Logical prefix extent, not an estimate of physical reads or memory bandwidth. */
    search_span = expected == SIZE_MAX ? size : expected + NEEDLE_SIZE;
    if (pattern_kind == 2) search_span = 0;
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        size_t iterations;
        const double seconds = median_latency(methods[i], &c, &iterations);
        print_row(dataset_names[dataset], pattern_names[pattern_kind], position_names[position], size,
                  result_text, method_names[i], seconds, iterations, search_span);
    }
    fp_set_free(c.set);
    return 1;
}

/* count signatures of 8 to 15 bytes copied from the second half of random
 * data, every fourth byte a wildcard; with miss set, the last byte of each
 * copy is altered in the data. Timed as one set, and as a set of one each. */
static int run_set_case(uint8_t *data, size_t size, size_t count, int miss)
{
    static struct bench_case c;
    static uint8_t bytes[MAX_SIGS][16], masks[MAX_SIGS][16];
    static size_t expected[MAX_SIGS];
    fp_sig sigs[MAX_SIGS];
    uint32_t state = UINT32_C(0x9e3779b9);
    size_t i, j, lowest = SIZE_MAX, found = 0, iterations;
    double seconds;
    char label[16], result[24];
    memset(&c, 0, sizeof(c));
    c.data = data;
    c.size = size;
    c.count = count;
    fill_background(data, size, 1);
    for (i = 0; i < count; ++i) {
        const size_t length = 8 + i % 8;
        size_t at;
        state = state * 1664525u + 1013904223u;
        at = size / 2 + (size_t)(state >> 8) % (size / 2 - 16);
        for (j = 0; j < length; ++j) {
            bytes[i][j] = data[at + j];
            masks[i][j] = j % 4 == 3 ? 0 : 0xff;
        }
        if (miss) data[at + length - 1] ^= 0x80;
        sigs[i].bytes = bytes[i];
        sigs[i].mask = masks[i];
        sigs[i].length = length;
    }
    c.set = fp_set_from_bytes(sigs, count, NULL);
    for (i = 0; i < count; ++i) {
        c.singles[i] = fp_set_from_bytes(&sigs[i], 1, NULL);
        if (!c.singles[i]) c.set = NULL;
    }
    if (!c.set) {
        fprintf(stderr, "Could not build %zu-signature sets\n", count);
        return 0;
    }
    for (i = 0; i < count; ++i) {
        expected[i] = reference_find(data, size, sigs[i].bytes, sigs[i].mask, sigs[i].length);
        if (expected[i] < lowest) lowest = expected[i];
        found += expected[i] != SIZE_MAX;
    }
    if (singles_search(&c) != lowest || offsets_search(&c) != lowest || scan_search(&c) != lowest) {
        fprintf(stderr, "Correctness failure: %zu-signature set\n", count);
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (c.offsets[i] != expected[i]) {
            fprintf(stderr, "Correctness failure: signature %zu of %zu\n", i, count);
            return 0;
        }
    }
    (void)snprintf(label, sizeof(label), "%zu sigs", count);
    (void)snprintf(result, sizeof(result), "%zu/%zu", found, count);
    seconds = median_latency(singles_search, &c, &iterations);
    print_row("random", label, miss ? "miss" : "found", size, result, "singles", seconds, iterations, size);
    seconds = median_latency(offsets_search, &c, &iterations);
    print_row("random", label, miss ? "miss" : "found", size, result, "set", seconds, iterations, size);
    seconds = median_latency(scan_search, &c, &iterations);
    print_row("random", label, miss ? "miss" : "found", size, lowest == SIZE_MAX ? "miss" : "first",
              "set-first", seconds, iterations, lowest == SIZE_MAX ? size : lowest + 16);
    fp_set_free(c.set);
    for (i = 0; i < count; ++i) fp_set_free(c.singles[i]);
    return 1;
}

int main(int argc, char **argv)
{
    static const size_t set_counts[] = {2, 4, 8, 16, 32, 64, 128, 256};
    size_t large_size = (size_t)4 * 1024 * 1024, i;
    uint8_t *data;
    int dataset, pattern_kind, position, miss;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [large-buffer-MiB: 1..256]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end;
        unsigned long value;
        errno = 0;
        value = strtoul(argv[1], &end, 10);
        if (errno != 0 || *argv[1] == '\0' || *end != '\0' || value < 1 || value > 256) {
            fprintf(stderr, "Buffer size must be an integer from 1 to 256 MiB\n");
            return EXIT_FAILURE;
        }
        large_size = (size_t)value * 1024 * 1024;
    }
    data = (uint8_t *)malloc(large_size);
    if (data == NULL) {
        fprintf(stderr, "Could not allocate %zu bytes\n", large_size);
        return EXIT_FAILURE;
    }
    printf("%s; %s\n", fp_ver(), fp_cpuinfo());
    printf("Deterministic seed 0x4d595df4; needle length %d; median of %d warm-cache batches.\n",
        NEEDLE_SIZE, TIMING_SAMPLES);
    printf("Each batch targets >=10 ms; iterations are per batch. Every method is checked first.\n");
    printf("GiB/s uses prefix-to-first-match (offset+needle), or full size for a miss.\n");
    printf("It is logical search throughput, not memory bandwidth. Wildcard throughput is n/a.\n");
    printf("scan returns the offset; offsets also fills the offsets array; build+scan compiles the\n");
    printf("text signature on every call. Timing includes a volatile indirect call.\n\n");
    printf("dataset     pattern  where     bytes   result method           ns/call     iters      GiB/s\n");
    for (dataset = 0; dataset < 3; ++dataset) {
        const size_t size = dataset == 0 ? 32 : large_size;
        for (pattern_kind = 0; pattern_kind < 3; ++pattern_kind) {
            const int positions = pattern_kind == 2 ? 1 : 3;
            for (position = 0; position < positions; ++position) {
                if (!run_case(data, size, dataset, pattern_kind, position)) {
                    free(data);
                    return EXIT_FAILURE;
                }
            }
        }
    }
    printf("\nSignature sets: copies of random data from its second half, every fourth byte a\n");
    printf("wildcard. singles scans with a set of one per signature, set scans once for all\n");
    printf("offsets, set-first only for the lowest. GiB/s counts the buffer once per call.\n\n");
    for (miss = 0; miss < 2; ++miss) {
        for (i = 0; i < sizeof(set_counts) / sizeof(set_counts[0]); ++i) {
            if (!run_set_case(data, large_size, set_counts[i], miss)) {
                free(data);
                return EXIT_FAILURE;
            }
        }
    }
    free(data);
    return EXIT_SUCCESS;
}
