#ifndef FASTPATTERN_H
#define FASTPATTERN_H

#include <stddef.h>
#include <stdint.h>

#define FASTPATTERN_VERSION "1.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Return the first matching offset, or SIZE_MAX for no match/invalid input.
 * Empty patterns and NULL data/pattern pointers are invalid. A NULL mask means
 * exact bytes; otherwise every byte obeys (data & mask) == (pattern & mask).
 * Buffers need not be aligned or padded. Search is case-sensitive, unanchored,
 * and fixed-length; this API does not interpret text or glob '*' syntax. */
size_t fp_find(const uint8_t* data, size_t data_size,
               const uint8_t* pattern, size_t pattern_size,
               const uint8_t* mask);

/* Whitespace-separated hex bytes, ?, ??, and nibble wildcards A? / ?F.
 * Rejects the entire pattern on malformed/empty input or allocation failure. */
size_t fp_find_pattern(const uint8_t* data, size_t data_size,
                      const char* pattern_str);

/* Parse and prepare once for repeated searches. The returned object owns its
 * bytes and is immutable; concurrent searches are safe. NULL means invalid
 * syntax, an empty pattern, or allocation failure. Free only after all users
 * have finished. fp_free(NULL) is safe. */
typedef struct fp_pattern fp_pattern;
fp_pattern* fp_compile(const char* pattern_str);
size_t fp_find_compiled(const uint8_t* data, size_t data_size,
                        const fp_pattern* pattern);
void fp_free(fp_pattern* pattern);

const char* fp_ver(void);
/* Immutable description of the best enabled backend. Small searches may use
 * a narrower backend or scalar path. Safe to call concurrently with searches. */
const char* fp_cpuinfo(void);

#ifdef __cplusplus
}
#endif

#endif
