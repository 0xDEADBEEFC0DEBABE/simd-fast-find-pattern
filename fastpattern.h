#ifndef FASTPATTERN_H
#define FASTPATTERN_H

#include <stddef.h>
#include <stdint.h>

#define FASTPATTERN_VERSION "2.0.0"

#ifdef __cplusplus
extern "C" {
#endif

/* An immutable, compiled set of one or more fixed-length signatures. Build
 * it once, search any number of buffers with it, from any number of threads,
 * and free it after the last search has returned. */
typedef struct fp_set fp_set;

/* A signature as bytes: data matches where (data & mask) == (bytes & mask)
 * for every byte. A NULL mask means every bit is significant. */
typedef struct {
    const uint8_t* bytes;
    const uint8_t* mask;
    size_t length;
} fp_sig;

typedef enum {
    FP_OK = 0,
    FP_ERROR_ARGUMENT, /* NULL list or signature pointer, or a count of zero */
    FP_ERROR_EMPTY,    /* a signature has no bytes */
    FP_ERROR_SYNTAX,   /* malformed signature text */
    FP_ERROR_MEMORY    /* allocation failed or the set is too large */
} fp_status;

/* Why construction failed: index is the offending signature, and for
 * FP_ERROR_SYNTAX, position is the offset of the bad token in its text. */
typedef struct {
    fp_status status;
    size_t index;
    size_t position;
} fp_error;

/* Text signatures are whitespace-separated hex bytes, with ? or ?? for any
 * byte and nibble wildcards such as A? and ?F. Both constructors return NULL
 * on failure; error may be NULL, and otherwise receives FP_OK on success. */
fp_set* fp_set_from_text(const char* const* sigs, size_t count, fp_error* error);
fp_set* fp_set_from_bytes(const fp_sig* sigs, size_t count, fp_error* error);

/* Searches data once for every signature in set. Returns the lowest offset at
 * which any signature matches, or SIZE_MAX if none does, so a set of one
 * signature returns its offset directly. If offsets is not NULL, offsets[i]
 * receives the first offset of signature i, or SIZE_MAX, for each of the
 * fp_set_count(set) signatures. A NULL set returns SIZE_MAX and writes
 * nothing; NULL data holds no matches. Buffers need not be aligned or padded,
 * and no byte outside data[0, size) is read. */
size_t fp_scan(const uint8_t* data, size_t size, const fp_set* set, size_t* offsets);

/* fp_scan, also counting each signature's matches: counts[i] receives the
 * number of offsets at which signature i matches, overlapping matches
 * included, up to limit, where the scan stops looking for it; a limit of 0
 * means none. With a limit of 2, one pass tells unique signatures (count 1)
 * from repeated ones (2). offsets may be NULL; with NULL counts, this is
 * fp_scan. */
size_t fp_scan_counts(const uint8_t* data, size_t size, const fp_set* set, size_t* offsets, size_t* counts,
                      size_t limit);

/* The number of signatures in set, or 0 for NULL. */
size_t fp_set_count(const fp_set* set);
/* fp_set_free(NULL) is safe. */
void fp_set_free(fp_set* set);

const char* fp_ver(void);
/* Immutable description of the best enabled backend. Small searches may use
 * a narrower backend or scalar path. Safe to call concurrently with searches. */
const char* fp_cpuinfo(void);

#ifdef __cplusplus
}
#endif

#endif
