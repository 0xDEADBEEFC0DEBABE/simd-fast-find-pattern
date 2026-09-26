#include "fastpattern.h"

#include <stdio.h>
#include <string.h>

static int expect_offset(const char *label, size_t actual, size_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: unexpected result\n", label);
        return 0;
    }
    printf("%s: offset %zu\n", label, actual);
    return 1;
}

int main(void)
{
    const char text[] = "Hello, pattern search!";
    const uint8_t binary[] = {0x00, 0x46, 0x50, 0xa7, 0x2f, 0x01, 0xff};
    const uint8_t bytes[] = {0x46, 0x50, 0xa0, 0x0f};
    const uint8_t mask[] = {0xff, 0xff, 0xf0, 0x0f};
    fp_pattern *compiled;
    int ok = 1;

    printf("%s; %s\n", fp_ver(), fp_cpuinfo());
    ok &= expect_offset("Exact text", fp_find((const uint8_t *)text,
        strlen(text), (const uint8_t *)"pattern", 7, NULL), 7);
    ok &= expect_offset("Bit masks", fp_find(binary, sizeof(binary),
        bytes, sizeof(bytes), mask), 1);
    ok &= expect_offset("Hex and byte wildcard", fp_find_pattern(binary,
        sizeof(binary), "46 50 ? 2F"), 1);

    compiled = fp_compile("46 50 A? ?F");
    if (compiled == NULL) {
        fprintf(stderr, "Could not compile pattern\n");
        return 1;
    }
    /* Compile once and reuse the immutable pattern for multiple searches. */
    ok &= expect_offset("Compiled nibble wildcards", fp_find_compiled(binary,
        sizeof(binary), compiled), 1);
    if (fp_find_compiled((const uint8_t *)text, strlen(text), compiled) != SIZE_MAX) {
        fprintf(stderr, "Expected the text search to miss\n");
        ok = 0;
    }
    fp_free(compiled);
    return ok ? 0 : 1;
}
