# FastPattern

FastPattern is a small C11 library for finding the first fixed-length byte pattern in a contiguous buffer. It supports exact bytes, wildcard bytes, and bit masks, with runtime-selected SIMD filtering where available. Integrate `fastpattern.c` and `fastpattern.h`; the C standard library is the only dependency.

## Build and run

Build a portable binary with your compiler's baseline architecture settings:

```sh
cc -O3 -std=c11 example_simple.c fastpattern.c -o fp_example
./fp_example
```

Or use CMake 3.21 or newer (the test suite also requires a C++11 compiler):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFASTPATTERN_BUILD_TESTS=ON -DFASTPATTERN_BUILD_BENCHMARKS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/fp_example
./build/fp_benchmark
```

With multi-configuration generators, executables are in the configuration directory, such as `build/Release/`. For MSVC, the standalone example can also be built with:

```bat
cl /O2 /std:c11 example_simple.c fastpattern.c /Fe:fp_example.exe
```

Do not apply global `-march=native`, `-mavx2`, `-mavx512*`, or `/arch:AVX2` options when distributing a binary to CPUs with different features. Such options can introduce unsupported instructions into common code before runtime dispatch. GCC and Clang SIMD functions use individual target attributes, so these global flags are unnecessary.

For a scalar build, add `-DFP_DISABLE_SIMD` when compiling, or configure CMake with `-DFASTPATTERN_DISABLE_SIMD=ON`. `FP_DISABLE_SIMD` disables this library's explicit SIMD paths; compiler auto-vectorization and C library routines may still use vector instructions. On supported GCC/Clang builds, `-DFASTPATTERN_SANITIZE=ON` enables AddressSanitizer and UndefinedBehaviorSanitizer; use a separate build directory for this configuration.

## Usage

```c
#include "fastpattern.h"
#include <string.h>

const char text[] = "Hello, pattern search!";
size_t offset = fp_find((const uint8_t *)text, strlen(text),
                       (const uint8_t *)"pattern", 7, NULL);
/* offset == 7; SIZE_MAX means no match or invalid input. */
```

Binary buffers can include zero bytes; lengths are explicit and buffers need no terminator or extra padding:

```c
const uint8_t data[] = {0x00, 0x46, 0x50, 0xa7, 0x2f, 0x01};
size_t offset = fp_find_pattern(data, sizeof(data), "46 50 A? ?F");
/* offset == 1 */
```

Compile a string pattern once when searching repeatedly:

```c
fp_pattern *pattern = fp_compile("46 50 ? ?F");
if (pattern != NULL) {
    size_t offset = fp_find_compiled(data, sizeof(data), pattern);
    /* Reuse pattern for other buffers. */
    (void)offset;
    fp_free(pattern);
}
```

Compiled patterns own their storage and are immutable. The source string may be changed or released after compilation. The same pattern can be searched concurrently, provided the input buffers remain readable and `fp_free` is called only after all searches finish.

## API

All declarations are in `fastpattern.h`, with C++ linkage guards. Existing entry points remain available.

| Function | Behavior |
| --- | --- |
| `fp_find(data, data_size, pattern, pattern_size, mask)` | Search raw byte arrays. A `NULL` mask means exact matching. No pattern allocation. |
| `fp_find_pattern(data, data_size, pattern_str)` | Parse and search on every call. Patterns up to 256 bytes use stack storage; larger patterns use temporary heap storage. |
| `fp_compile(pattern_str)` | Return an owned `fp_pattern *`, or `NULL` for invalid input or allocation failure. |
| `fp_find_compiled(data, data_size, pattern)` | Search using a previously compiled pattern. No pattern allocation. |
| `fp_free(pattern)` | Release a compiled pattern; `NULL` is accepted. |
| `fp_ver()` | Return the library version string. |
| `fp_cpuinfo()` | Describe the strongest enabled backend. Short inputs can use narrower SIMD or a scalar path. |

Search functions return the byte offset of the **first** match, or `SIZE_MAX` for no match or invalid input. Null required pointers, empty patterns, and patterns longer than the buffer do not match. String search also returns `SIZE_MAX` if pattern allocation fails; the API does not distinguish these failures from a miss. An all-wildcard pattern returns zero when the buffer is large enough. Functions search only the supplied readable buffer; they do not inspect other process memory or join separate buffers.

For `fp_find`, each byte matches when:

```c
(data_byte & mask_byte) == (pattern_byte & mask_byte)
```

Every mask bit is significant: `0xff` requires an exact byte, `0x00` accepts any byte, and masks such as `0xf0`, `0x0f`, or `0x81` select individual bits. The mask must contain at least `pattern_size` bytes. `data`, `pattern`, and a non-null `mask` must remain readable for their declared lengths for the duration of the call.

### String pattern syntax

Tokens must be separated by whitespace. Leading and trailing whitespace are allowed; hex digits are case-insensitive.

| Token | Meaning |
| --- | --- |
| `4F` or `4f` | Exact byte `0x4f` |
| `?` or `??` | Any one byte |
| `A?` | High nibble is `0xa` |
| `?F` | Low nibble is `0xf` |

For example, `46 50 ? ?? A? ?F` contains six bytes. Empty strings and malformed tokens such as `4`, `GG`, `0x4F`, `???`, or `4650` are rejected in full. Valid prefixes of malformed patterns are never silently searched. Use the raw mask API for masks that cannot be represented by whole nibbles.

## Runtime dispatch and portability

| Build target | Available implementations |
| --- | --- |
| x86 with GCC or Clang | Scalar, SSE2, AVX2, AVX-512BW; selected using runtime CPU/OS support |
| x86 with MSVC | Baseline SSE2 where available, otherwise scalar |
| AArch64 with GCC or Clang | NEON and scalar |
| Other compiler/architecture combinations (including MSVC ARM64), or `FP_DISABLE_SIMD` | Portable scalar implementation |

The backend description reports the strongest available and enabled implementation; searches can select narrower SIMD or scalar code when the number of candidate offsets is small. For diagnostic comparisons, GCC/Clang x86 builds can set `-DFP_MAX_SIMD_BITS=128` or `=256` to cap SIMD width while preserving runtime checks.

SIMD paths filter multiple candidate offsets before verifying a complete match, including multi-byte patterns and masks. Vector loads stay within the supplied buffer, and a scalar tail handles the remainder. ISA selection does not guarantee the lowest latency for every CPU or workload: pattern length, wildcard distribution, input contents, match position, cache state, and processor frequency all matter. AVX-512 availability alone does not guarantee a speedup over AVX2. Repetitive data and weakly selective masks can require many candidate verifications; worst-case work can grow with both buffer and pattern length.

## Reproducible benchmarks

The self-contained benchmark uses deterministic synthetic data and does not require external binaries:

```sh
cc -O3 -std=c11 benchmark.c fastpattern.c -o fp_benchmark
./fp_benchmark       # 32-byte short text and 4 MiB large buffers
./fp_benchmark 16    # change large-buffer size to 16 MiB (range: 1..256)
```

It covers random and repetitive large buffers, exact and masked 8-byte patterns, beginning/end/missing matches, and all-wildcard patterns. Each fixture has a known expected result. Every API's result is checked against an independent scalar reference **before that fixture is timed**; mismatches fail the run. Timings include the reference, raw search, compiled search, and parse-plus-search separately.

The report prints the selected backend, nanoseconds per call, iterations per batch, and logical search throughput. Each result is the median of three warm-cache batches after calibration to at least 10 ms per batch, subject to a fixed iteration cap. The deterministic random generator starts from seed `0x4d595df4` for each fixture. Timing includes a volatile indirect function call, which is significant for very short searches. The reference is deliberately simple, not a claim about the best possible scalar algorithm.

For a found pattern, throughput counts only `offset + pattern_length` bytes; a miss counts the full buffer. This is the logical prefix through the first result, **not physical bytes loaded or memory bandwidth**. All-wildcard throughput is reported as `n/a`, since it can return without scanning the buffer. The `parse+find` measurement includes parsing on every call; these 8-byte fixtures use stack storage, so no timed method allocates pattern memory. Compilation for the compiled API occurs outside timing.

These are warm-cache microbenchmarks, not entity-list or application-level measurements. Record the compiler/version, flags, CPU, backend, buffer size, and power settings when sharing results. Run both normal and scalar builds on the target machine; do not infer x86 SIMD speed from an ARM or emulated run. No universal speedup is claimed.

## Scope

FastPattern performs unanchored, fixed-length, case-sensitive byte matching. Its `?` wildcard consumes exactly one byte. It does not implement variable-length `*` globs, Unicode case folding, whole-string matching, linked-list traversal, indexes, or streaming state across chunks. For chunked input, callers must preserve up to `pattern_length - 1` bytes between chunks to detect boundary-spanning matches.

Short names and pointer-heavy entity lists may spend little time searching strings, and may not benefit from SIMD. Whole-name equality, glob matching, and class indexes need algorithms appropriate to their semantics. Measure the complete workload before replacing an existing lookup.

See [CHANGELOG.md](CHANGELOG.md) for version changes. Released under the [MIT license](LICENSE).
