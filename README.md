# FastPattern - High-Performance SIMD Pattern Matching Library

FastPattern is a single-file C library for high-speed pattern matching in binary data using advanced SIMD instructions. It automatically detects CPU capabilities and uses the fastest available instruction set (AVX-512, AVX2, SSE4.2).

## Features

- Zero Dependencies: Only requires standard C library
- High Performance: Optimized SIMD implementations for modern CPUs
- Wildcard Support: Supports IDA-style (??) and single (?) wildcards
- Auto-Detection: Automatically detects and uses best CPU features
- Cross-Platform: Works on Windows, Linux, macOS
- Single File: Just two files to integrate

## Files

- `fastpattern.h` - Header file with API declarations
- `fastpattern.c` - Implementation file with all functionality
- `example_simple.c` - Complete usage example

## Quick Start

### Integration

```bash
# Copy files to your project
cp fastpattern.h fastpattern.c your_project/

# Compile with optimizations
gcc -O3 -march=native -mavx2 -mavx512f your_code.c fastpattern.c -o your_program
```

### Basic Usage

```c
#include "fastpattern.h"

// Simple text search
const char* text = "Hello World";
const char* pattern = "World";
size_t pos = fp_find((uint8_t*)text, strlen(text), 
                     (uint8_t*)pattern, strlen(pattern), NULL);

if (pos != SIZE_MAX) {
    printf("Found at position %zu\n", pos);
}
```

### Pattern String Search (with wildcards)

```c
// IDA-style wildcards (??)
size_t pos1 = fp_find_pattern(data, data_size, "48 FF 5C 24 ?? FF 57");

// Single wildcards (?)
size_t pos2 = fp_find_pattern(data, data_size, "48 89 FF FF ? FF");

// Mixed patterns
size_t pos3 = fp_find_pattern(data, data_size, "FF ?? FF ? FF");
```

### Manual Wildcard Matching

```c
// Hex pattern with wildcards
uint8_t data[] = {0x48, 0xFF, 0x5C, 0x24, 0x00, 0xFF, 0x57};
uint8_t pattern[] = {0x48, 0x89, 0xFF, 0xFF, 0x00, 0xFF};
uint8_t mask[] = {0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF};  // 0x00 = wildcard

size_t pos = fp_find(data, sizeof(data), pattern, sizeof(pattern), mask);
```

## API Reference

### Core Functions

**`size_t fp_find(data, data_size, pattern, pattern_size, mask)`**
- `data`: Pointer to data to search in
- `data_size`: Size of data in bytes
- `pattern`: Pointer to pattern to find
- `pattern_size`: Size of pattern in bytes
- `mask`: Optional mask for wildcards (NULL = no wildcards)
- Returns: Position of first match or SIZE_MAX if not found

**`size_t fp_find_pattern(data, data_size, pattern_str)`**
- `data`: Pointer to data to search in
- `data_size`: Size of data in bytes
- `pattern_str`: Hex pattern string with wildcards
- Returns: Position of first match or SIZE_MAX if not found

**`const char* fp_ver(void)`**
- Returns: Library version string

**`const char* fp_cpuinfo(void)`**
- Returns: Detected CPU features string

### Wildcard Formats

**IDA Style:**
- `??` = Wildcard byte (any value matches)
- `48 89 ?? 24 10` = Match 48 89 [any] 24 10

**Single Question Mark:**
- `?` = Wildcard byte (any value matches)  
- `48 89 ? 24 10` = Match 48 89 [any] 24 10

**Manual Mask:**
- `0xFF` = Exact match required for this byte
- `0x00` = Wildcard (any byte value matches)

## Performance Benchmarks

Real-world performance testing on CS2 game patterns (25MB binary):

### Pattern: [REDACTED] (24 bytes)
```
Pattern: [REDACTED]

FastPattern:           0.095ms  (Found at [REDACTED])
Algorithm SIMD AVX2:   1.252ms  (13.2x slower)
0x80.pl SIMD (AVX2):   1.295ms  (13.7x slower)
0x80.pl SIMD (SSE2):   1.971ms  (20.8x slower)
```

### Pattern: [REDACTED] (15 bytes)
```
Pattern: [REDACTED]

FastPattern:           0.371ms  (Found at [REDACTED])
Algorithm SIMD AVX2:   1.519ms  (4.1x slower)
0x80.pl SIMD (AVX2):   0.391ms  (1.1x slower)
0x80.pl SIMD (SSE2):   0.489ms  (1.3x slower)
```

### Pattern: [REDACTED] (20 bytes)
```
Pattern: [REDACTED]

FastPattern:           0.148ms  (Found at [REDACTED])
Algorithm SIMD AVX2:   1.433ms  (9.7x slower)
0x80.pl SIMD (AVX2):   0.129ms  (FastPattern 14.7% slower)
0x80.pl SIMD (SSE2):   0.277ms  (1.9x slower)
```

### Pattern: [REDACTED] (13 bytes)
```
Pattern: [REDACTED]

FastPattern:           0.096ms  (Found at [REDACTED])
Algorithm SIMD AVX2:   0.724ms  (7.5x slower)
0x80.pl SIMD (AVX2):   0.146ms  (1.5x slower)
0x80.pl SIMD (SSE2):   0.270ms  (2.8x slower)
```

### Pattern: [REDACTED] (31 bytes)
```
Pattern: [REDACTED]

FastPattern:           0.034ms  (Found at [REDACTED])
Algorithm SIMD AVX2:   0.318ms  (9.5x slower)
0x80.pl SIMD (AVX2):   0.303ms  (9.0x slower)
0x80.pl SIMD (SSE2):   0.435ms  (12.9x slower)
```

### Pattern: [REDACTED] (7 bytes)
```
Pattern: [REDACTED]

FastPattern:           0.026ms  (Found at [REDACTED])
Algorithm SIMD AVX2:   0.068ms  (2.6x slower)
0x80.pl SIMD (AVX2):   0.047ms  (1.8x slower)
0x80.pl SIMD (SSE2):   0.087ms  (3.3x slower)
```

## Technical Implementation

### Algorithm Strategy

FastPattern uses an adaptive approach:

1. **Single-byte patterns**: 4x unrolled AVX-512 with prefetching
2. **Multi-byte patterns**: First+last byte SIMD filtering with scalar verification
3. **CPU Detection**: Runtime detection of AVX-512, AVX2, SSE4.2 capabilities
4. **Memory Optimization**: Prefetching and cache-friendly access patterns

### Pattern Parsing

Supports multiple wildcard formats:
- **IDA Style**: `48 89 ?? 5C` (double question marks)
- **Single**: `48 89 ? 5C` (single question mark)
- **Mixed**: `48 ?? 5C ? 10` (both formats in same pattern)

### Compilation Recommendations

For optimal performance:

```bash
# GCC/Clang
gcc -O3 -march=native -mavx2 -mavx512f -mavx512bw -funroll-loops -ffast-math

# MSVC
cl /O2 /arch:AVX2
```

## Use Cases

- Game modding and reverse engineering
- Binary analysis and forensics
- Real-time data stream processing
- Memory scanning applications
- Antivirus signature matching

## Example Usage

```c
// Search for function prologue
size_t pos = fp_find_pattern(binary_data, size, "55 FF EC FF EC ??");

// Search for specific instruction sequence
size_t pos = fp_find_pattern(binary_data, size, "FF 89 FF 24 ?? FF 89 FF 24 ??");

// Search with mixed wildcards
size_t pos = fp_find_pattern(binary_data, size, "E8 ?? ?? ?? ?? 85 FF FF ?");
```

## License

MIT License - Free for commercial and open source use.

## Requirements

- C11 compatible compiler
- CPU with SSE2+ (AVX-512 recommended for best performance)
- x86-64 architecture