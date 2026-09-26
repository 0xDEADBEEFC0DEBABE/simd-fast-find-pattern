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
enum { NEEDLE_SIZE = 8, TIMING_SAMPLES = 3 };

struct bench_case {
    const uint8_t *data;
    size_t size;
    uint8_t needle[NEEDLE_SIZE];
    uint8_t mask[NEEDLE_SIZE];
    int masked;
    char expression[NEEDLE_SIZE * 3 + 1];
    fp_pattern *compiled;
};

typedef size_t (*search_function)(const struct bench_case *);
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

static size_t reference_search(const struct bench_case *c)
{
    size_t i, j;
    if (c->size < NEEDLE_SIZE) return SIZE_MAX;
    for (i = 0; i <= c->size - NEEDLE_SIZE; ++i) {
        for (j = 0; j < NEEDLE_SIZE; ++j) {
            const uint8_t m = c->masked ? c->mask[j] : UINT8_MAX;
            if ((c->data[i + j] & m) != (c->needle[j] & m)) break;
        }
        if (j == NEEDLE_SIZE) return i;
    }
    return SIZE_MAX;
}

static size_t raw_search(const struct bench_case *c)
{
    return fp_find(c->data, c->size, c->needle, NEEDLE_SIZE,
        c->masked ? c->mask : NULL);
}

static size_t compiled_search(const struct bench_case *c)
{
    return fp_find_compiled(c->data, c->size, c->compiled);
}

static size_t parsed_search(const struct bench_case *c)
{
    return fp_find_pattern(c->data, c->size, c->expression);
}

static double time_batch(search_function function, const struct bench_case *c,
                         size_t iterations)
{
    /* A volatile indirect call also prevents hoisting in whole-program builds. */
    search_function volatile invoke = function;
    const double start = now_seconds();
    size_t i;
    for (i = 0; i < iterations; ++i) result_sink ^= invoke(c);
    return now_seconds() - start;
}

static double median_latency(search_function function, const struct bench_case *c,
                             size_t *iterations)
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

static void format_pattern(struct bench_case *c)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i, used = 0;
    for (i = 0; i < NEEDLE_SIZE; ++i) {
        const uint8_t m = c->masked ? c->mask[i] : UINT8_MAX;
        if (i != 0) c->expression[used++] = ' ';
        if (m == 0 && i % 2 == 0) {
            c->expression[used++] = '?';
        } else {
            c->expression[used++] = (m & 0xf0) ? hex[c->needle[i] >> 4] : '?';
            c->expression[used++] = (m & 0x0f) ? hex[c->needle[i] & 0x0f] : '?';
        }
    }
    c->expression[used] = '\0';
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

static int run_case(uint8_t *data, size_t size, int dataset, int pattern_kind,
                    int position)
{
    static const char *const dataset_names[] = {"short-text", "random", "repetitive"};
    static const char *const pattern_names[] = {"exact", "masked", "wildcard"};
    static const char *const position_names[] = {"begin", "end", "miss"};
    static const char *const method_names[] = {"reference", "raw", "compiled", "parse+find"};
    static const search_function methods[] = {
        reference_search, raw_search, compiled_search, parsed_search
    };
    struct bench_case c;
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
    format_pattern(&c);
    c.compiled = fp_compile(c.expression);
    if (c.compiled == NULL) {
        fprintf(stderr, "Could not compile benchmark pattern: %s\n", c.expression);
        return 0;
    }
    reference = reference_search(&c);
    if (reference != expected) {
        fprintf(stderr, "Fixture mismatch: %s/%s/%s, expected %zu, got %zu\n",
            dataset_names[dataset], pattern_names[pattern_kind],
            position_names[position], expected, reference);
        fp_free(c.compiled);
        return 0;
    }
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        const size_t result = methods[i](&c);
        if (result != reference) {
            fprintf(stderr, "Correctness failure: %s/%s/%s via %s, expected %zu, got %zu\n",
                dataset_names[dataset], pattern_names[pattern_kind],
                position_names[position], method_names[i], reference, result);
            fp_free(c.compiled);
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
        const double throughput = (double)search_span / seconds / (1024.0 * 1024.0 * 1024.0);
        printf("%-11s %-8s %-5s %9zu %8s %-10s %11.2f %9zu ",
            dataset_names[dataset], pattern_names[pattern_kind],
            position_names[position], size, result_text, method_names[i],
            seconds * 1e9, iterations);
        if (search_span == 0) printf("       n/a\n");
        else printf("%10.3f\n", throughput);
    }
    fp_free(c.compiled);
    return 1;
}

int main(int argc, char **argv)
{
    size_t large_size = (size_t)4 * 1024 * 1024;
    uint8_t *data;
    int dataset, pattern_kind, position;
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
    printf("Timing includes a volatile indirect call; parse+find includes parsing (8-byte patterns use stack storage).\n\n");
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
    free(data);
    return EXIT_SUCCESS;
}
