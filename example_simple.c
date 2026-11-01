#include "fastpattern.h"

#include <stdio.h>
#include <string.h>

static int expect_offset(const char *label, size_t actual, size_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: unexpected result\n", label);
        return 0;
    }
    if (actual == SIZE_MAX) {
        printf("%s: not found\n", label);
    } else {
        printf("%s: offset %zu\n", label, actual);
    }
    return 1;
}

int main(void)
{
    const char text[] = "Hello, pattern search!";
    const uint8_t binary[] = {0x00, 0x46, 0x50, 0xa7, 0x2f, 0x01, 0xff};
    const uint8_t bytes[] = {0x46, 0x50, 0xa0, 0x0f};
    const uint8_t mask[] = {0xff, 0xff, 0xf0, 0x0f};
    const char *const signatures[] = {"46 50 A? ?F", "2F 01 FF", "DE AD BE EF"};
    const fp_sig exact = {(const uint8_t *)"pattern", NULL, 7};
    const fp_sig masked = {bytes, mask, sizeof(bytes)};
    size_t offsets[3];
    fp_error error;
    fp_set *set;
    int ok = 1;

    printf("%s; %s\n", fp_ver(), fp_cpuinfo());

    /* A set of one signature: fp_scan returns its offset. */
    set = fp_set_from_bytes(&exact, 1, NULL);
    ok &= set && expect_offset("Exact text", fp_scan((const uint8_t *)text, strlen(text), set, NULL), 7);
    fp_set_free(set);
    set = fp_set_from_bytes(&masked, 1, NULL);
    ok &= set && expect_offset("Bit masks", fp_scan(binary, sizeof(binary), set, NULL), 1);
    fp_set_free(set);

    /* Several signatures, one scan: an offset each, SIZE_MAX if absent.
     * Compile once and reuse the immutable set for any number of buffers. */
    set = fp_set_from_text(signatures, 3, &error);
    if (set == NULL) {
        fprintf(stderr, "Signature %zu is invalid at offset %zu\n", error.index, error.position);
        return 1;
    }
    (void)fp_scan(binary, sizeof(binary), set, offsets);
    ok &= expect_offset(signatures[0], offsets[0], 1);
    ok &= expect_offset(signatures[1], offsets[1], 4);
    ok &= expect_offset(signatures[2], offsets[2], SIZE_MAX);
    ok &= expect_offset("Earliest in text", fp_scan((const uint8_t *)text, strlen(text), set, NULL), SIZE_MAX);
    fp_set_free(set);
    return ok ? 0 : 1;
}
