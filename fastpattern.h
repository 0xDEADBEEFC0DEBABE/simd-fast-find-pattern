#ifndef FASTPATTERN_H
#define FASTPATTERN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t fp_find(const uint8_t* data, size_t data_size,
               const uint8_t* pattern, size_t pattern_size,
               const uint8_t* mask);

size_t fp_find_pattern(const uint8_t* data, size_t data_size, const char* pattern_str);

const char* fp_ver(void);

const char* fp_cpuinfo(void);

#ifdef __cplusplus
}
#endif

#endif