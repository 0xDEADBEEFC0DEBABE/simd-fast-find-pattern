# FastPattern

FastPattern is a small C11 library for finding fixed-length byte signatures in a contiguous buffer: one signature, or many at once in a single scan. Signatures can contain exact bytes, wildcard bytes, and bit masks, and runtime-selected SIMD filtering is used where available. Integrate `fastpattern.c` and `fastpattern.h`; the C standard library is the only dependency.

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

Do not apply global `-march=native`, `-mavx2`, `-mavx512*`, or `/arch:AVX2` options when distributing a binary to CPUs with different features. Such options can introduce unsupported instructions into common code before runtime dispatch. GCC and Clang SIMD functions use individual target attributes, and MSVC compiles the AVX2 and AVX-512 intrinsics without `/arch`, so these global flags are unnecessary.

For a scalar build, add `-DFP_DISABLE_SIMD` when compiling, or configure CMake with `-DFASTPATTERN_DISABLE_SIMD=ON`. `FP_DISABLE_SIMD` disables this library's explicit SIMD paths; compiler auto-vectorization and C library routines may still use vector instructions. On supported GCC/Clang builds, `-DFASTPATTERN_SANITIZE=ON` enables AddressSanitizer and UndefinedBehaviorSanitizer; use a separate build directory for this configuration.

## Usage

Build a set once, then scan any number of buffers with it. A set of one signature returns its offset directly:

```c
#include "fastpattern.h"

const uint8_t data[] = {0x00, 0x46, 0x50, 0xa7, 0x2f, 0x01, 0xff};
const char *text = "46 50 A? ?F";
fp_set *set = fp_set_from_text(&text, 1, NULL);
if (set != NULL) {
    size_t offset = fp_scan(data, sizeof(data), set, NULL);
    /* offset == 1; SIZE_MAX means no match. */
    fp_set_free(set);
}
```

Several signatures are searched in one scan, with an offset for each:

```c
const char *const signatures[] = {"46 50 A? ?F", "2F 01 FF", "DE AD BE EF"};
size_t offsets[3];
fp_error error;
fp_set *set = fp_set_from_text(signatures, 3, &error);
if (set == NULL) {
    /* error.status says why; error.index names the signature, and for
     * FP_ERROR_SYNTAX error.position is the offset of the bad token. */
} else {
    size_t lowest = fp_scan(data, sizeof(data), set, offsets);
    /* offsets == {1, 4, SIZE_MAX}, lowest == 1 */
    fp_set_free(set);
}
```

To require that each signature occur only once, as loaders that reject ambiguous signatures do, count matches in the same single scan. With a limit of 2, the scan stops looking for a signature at its second match:

```c
size_t counts[3];
fp_scan_counts(data, sizeof(data), set, offsets, counts, 2);
/* counts[i] == 1: signature i is unique, at offsets[i]; 2: it repeats; 0: absent. */
```

Signatures can also be given as bytes and masks, which is how to express masks that are not whole nibbles:

```c
const uint8_t bytes[] = {0x46, 0x50, 0xa0, 0x0f};
const uint8_t mask[] = {0xff, 0xff, 0xf0, 0x0f};
const fp_sig sig = {bytes, mask, sizeof(bytes)};
fp_set *set = fp_set_from_bytes(&sig, 1, NULL);
```

Sets own copies of their signatures and are immutable: the source strings and arrays may be changed or released after construction. The same set can be scanned concurrently, provided the input buffers remain readable and `fp_set_free` is called only after all scans finish.

## API

All declarations are in `fastpattern.h`, with C++ linkage guards.

| Function | Behavior |
| --- | --- |
| `fp_set_from_text(sigs, count, error)` | Parse `count` text signatures into a new set, or return `NULL` and describe the failure in `*error` (which may be `NULL`). |
| `fp_set_from_bytes(sigs, count, error)` | The same for `fp_sig` signatures: bytes, a mask or `NULL` for exact bytes, and a length. |
| `fp_scan(data, size, set, offsets)` | Search `data` once for every signature. Returns the lowest offset at which any signature matches, or `SIZE_MAX`. If `offsets` is not `NULL`, `offsets[i]` receives the first offset of signature `i`, or `SIZE_MAX`. |
| `fp_scan_counts(data, size, set, offsets, counts, limit)` | `fp_scan`, also counting: `counts[i]` receives the number of offsets at which signature `i` matches, overlapping matches included, up to `limit` (`0` for no limit), where the scan stops looking for it. |
| `fp_set_count(set)` | The number of signatures, `0` for `NULL`. |
| `fp_set_free(set)` | Release a set; `NULL` is accepted. |
| `fp_ver()` | Return the library version string. |
| `fp_cpuinfo()` | Describe the strongest enabled backend. Short inputs can use narrower SIMD or a scalar path. |

Construction fails with `FP_ERROR_ARGUMENT` for a `NULL` list or signature pointer or a count of zero, `FP_ERROR_EMPTY` for a signature without bytes, `FP_ERROR_SYNTAX` for malformed text, and `FP_ERROR_MEMORY` when allocation fails or the set would be too large. `fp_error.index` names the signature and, for syntax errors, `fp_error.position` is the offset of the bad token in its text. On success, `error->status` is `FP_OK`.

`fp_scan` with a `NULL` set returns `SIZE_MAX` and writes nothing; `NULL` data holds no matches. `fp_scan_counts` behaves the same, and with `NULL` counts it is `fp_scan`. Signatures longer than the buffer do not match, and an all-wildcard signature matches at zero when the buffer is large enough. Scans read only `data[0, size)`; they do not inspect other process memory or join separate buffers. Buffers need no alignment, terminator, or padding.

For byte signatures, each byte matches when:

```c
(data_byte & mask_byte) == (signature_byte & mask_byte)
```

Every mask bit is significant: `0xff` requires an exact byte, `0x00` accepts any byte, and masks such as `0xf0`, `0x0f`, or `0x81` select individual bits.

### Text signature syntax

Tokens must be separated by whitespace. Leading and trailing whitespace are allowed; hex digits are case-insensitive.

| Token | Meaning |
| --- | --- |
| `4F` or `4f` | Exact byte `0x4f` |
| `?` or `??` | Any one byte |
| `A?` | High nibble is `0xa` |
| `?F` | Low nibble is `0xf` |

For example, `46 50 ? ?? A? ?F` contains six bytes. Empty strings and malformed tokens such as `4`, `GG`, `0x4F`, `???`, or `4650` are rejected in full, and the error names the token. Valid prefixes of malformed signatures are never silently searched. Use byte signatures for masks that cannot be represented by whole nibbles.

## Signature sets

On x86 with SSSE3 or better, sets of five or more signatures are searched with Teddy, the multi-literal filter from Hyperscan, adapted to masks:

- Each signature gets a fingerprint: the 4 consecutive bytes least likely to occur in x86-64 code, judged by byte frequencies measured on the executable sections of Windows system binaries. `PSHUFB` looks up both nibbles of every data byte in per-group tables, so wildcards and nibble masks cost nothing.
- Fingerprints share 8 buckets per group. Signatures are sorted so that similar fingerprints meet, and each joins the bucket whose estimated false-candidate rate it raises least; a group closes when its budget is spent, so groups of rare fingerprints hold more signatures than groups of common ones.
- With `offsets`, a bucket leaves the scan once its signatures are found, and the scan ends when all are; when counting, once they reach the limit. Without either, it stops where no signature could start below the lowest match found.
- Where candidates grow dense, as in long runs of padding, a group gives way to searching its remaining signatures one by one, from where it stopped. Signatures too short for a fingerprint, or without an informative one, are always searched one by one.

Smaller sets, buffers under 67 bytes, and other backends (SSE2 without SSSE3, NEON, scalar) search each signature in turn with the single-signature code. A set's result never depends on the path taken.

On a Ryzen 9 7950X (AVX-512BW, Windows, Clang 22), one scan of 66 MB of code from Windows system DLLs, with signatures of 10 to 20 bytes copied from it, took 3.5 ms for 16 signatures and 64 ms for 256, against 14.5 ms and 271 ms for one scan per signature (4.1x and 4.2x); across runs, as the machine's state moved both timings, the gain ranged from 3.3x to 4.3x. On the benchmark's random data the gain for 256 signatures is about 4x; with 4 signatures or fewer a set is no faster. Dense candidate data can make a set slightly slower than separate scans. Counting keeps each signature in the scan until it reaches the limit, so a uniqueness check with a limit of 2 makes a full pass for every group: on the benchmark's random data it costs about as much as a scan with offsets, and twice as much as a scan for the lowest offset alone. These figures depend on the CPU, the data, and the signatures; measure your own workload.

## Runtime dispatch and portability

| Build target | Available implementations |
| --- | --- |
| x86 with GCC, Clang, or MSVC (Visual Studio 2017 15.3 or later) | Scalar, SSE2, SSSE3 (signature sets), AVX2, AVX-512BW; selected using runtime CPU/OS support |
| x86 with older MSVC | Baseline SSE2 where available, SSSE3 for signature sets, otherwise scalar |
| AArch64 with GCC, Clang, or MSVC | NEON and scalar; sets search each signature in turn |
| Other compiler/architecture combinations, or `FP_DISABLE_SIMD` | Portable scalar implementation |

The backend description reports the strongest available and enabled implementation; searches can select narrower SIMD or scalar code when the number of candidate offsets is small. For diagnostic comparisons, x86 builds can set `-DFP_MAX_SIMD_BITS=128` or `=256` to cap SIMD width while preserving runtime checks.

Single-signature SIMD paths filter multiple candidate offsets before verifying a complete match. The filter tests two anchor bytes: the first constrained byte and the last one, or, when both are the same exact byte, the last exact byte that differs from it, so runs of a repeated byte such as zero or `0xcc` padding do not make every offset a candidate. On x86, blocks with several candidates are narrowed one signature byte at a time across all lanes before individual verification. Exact signatures, and masked ones whose first and last constrained bytes are exact, reach code sized for the number of possible starts through a few conditional tail jumps; wildcard ends are trimmed when the set is built. Long scans begin where the first anchor's loads are aligned. Vector loads stay within the supplied buffer: the final vector ends at the last possible match and overlaps offsets already scanned, and AVX-512 short searches use masked loads.

ISA selection does not guarantee the lowest latency for every CPU or workload: signature length, wildcard distribution, input contents, match position, cache state, and processor frequency all matter. AVX-512 availability alone does not guarantee a speedup over AVX2. Repetitive data and weakly selective masks can require many candidate verifications; worst-case work can grow with both buffer and signature length.

## Migrating from 1.x

| 1.x | 2.0 |
| --- | --- |
| `fp_find(data, n, bytes, length, mask)` | Build once with `fp_set_from_bytes` (`fp_sig sig = {bytes, mask, length}`), then `fp_scan(data, n, set, NULL)`. |
| `fp_find_pattern(data, n, text)` | `fp_set_from_text(&text, 1, &error)`, then `fp_scan`. Build the set once instead of on every search. |
| `fp_compile(text)` | `fp_set_from_text(&text, 1, &error)` |
| `fp_find_compiled(data, n, pattern)` | `fp_scan(data, n, set, NULL)` |
| `fp_free(pattern)` | `fp_set_free(set)` |
| Several patterns, one search each | One set of all of them: `fp_scan(data, n, set, offsets)` |

Results are unchanged: the first match offset, or `SIZE_MAX`. Invalid signatures now fail at construction, with a reason, rather than returning `SIZE_MAX` from every search.

## Reproducible benchmarks

The self-contained benchmark uses deterministic synthetic data and does not require external binaries:

```sh
cc -O3 -std=c11 benchmark.c fastpattern.c -o fp_benchmark
./fp_benchmark       # 32-byte short text and 4 MiB large buffers
./fp_benchmark 16    # change large-buffer size to 16 MiB (range: 1..256)
```

The first part covers random and repetitive large buffers, exact and masked 8-byte signatures, beginning/end/missing matches, and all-wildcard signatures, through `fp_scan`'s return value (`scan`), with `offsets` (`offsets`), and with the set built and freed on every call (`build+scan`). The second part times sets of 2 to 256 signatures copied from random data, every fourth byte a wildcard, once per signature (`singles`), as one set with offsets (`set`), for the lowest offset only (`set-first`), and as one set counting each signature's matches to 2 (`set-counts`). Each fixture has a known expected result. Every result is checked against an independent scalar reference **before that fixture is timed**; mismatches fail the run.

The report prints the selected backend, nanoseconds per call, iterations per batch, and logical search throughput. Each result is the median of three warm-cache batches after calibration to at least 10 ms per batch, subject to a fixed iteration cap. The deterministic random generator starts from seed `0x4d595df4` for each fixture. Timing includes a volatile indirect function call, which is significant for very short searches. The reference is deliberately simple, not a claim about the best possible scalar algorithm.

For a found signature, throughput counts only `offset + signature_length` bytes; a miss counts the full buffer, and set rows count the buffer once per call. This is logical throughput, **not physical bytes loaded or memory bandwidth**. All-wildcard throughput is reported as `n/a`, since it can return without scanning the buffer.

These are warm-cache microbenchmarks, not application-level measurements. Record the compiler/version, flags, CPU, backend, buffer size, and power settings when sharing results. Run both normal and scalar builds on the target machine; do not infer x86 SIMD speed from an ARM or emulated run. No universal speedup is claimed.

## Compared with 0x80.pl

FastPattern was measured against Wojciech Muła's SIMD substring search ([0x80.pl](http://0x80.pl/articles/simd-strfind.html): `sse2_strstr_v2`, `avx2_strstr_v2` and `avx512bw_strstr_v2` from [sse4-strstr](https://github.com/WojciechMula/sse4-strstr), compiled unmodified) on a real x86-64 Windows binary: 149 signatures searched in its 24.6 MB code section. The binary and the signatures are not published. Those searches take exact needles, so each signature's wildcards were filled in with the bytes where it matches, which gives needles of 6 to 1,777 bytes (median 24) that each occur once. FastPattern was built with `FP_MAX_SIMD_BITS` at 128, 256 and 512, each against the search of the same width, and searched each needle as a set of one, or all 149 as one set: `fp_scan` for first matches, `fp_scan_counts` counting to 2 for the unique check. The unique check searches past each first match, which must find nothing, as a loader that rejects ambiguous signatures does. Every result was compared with a naive search before timing. Times are for all 149 needles, warm, on one pinned core of a Ryzen 9 7950X (Windows, Clang 22, `-O3`): the median over three runs of each run's best of 15. Parentheses give 0x80.pl's time over FastPattern's.

| Width | All 149 needles | 0x80.pl, each needle | FastPattern, each needle | FastPattern, one set |
| --- | --- | ---: | ---: | ---: |
| 128-bit | first matches | 67.51 ms | 47.97 ms (1.41x) | 10.90 ms (6.19x) |
| | unique check | 129.57 ms | 93.00 ms (1.39x) | 11.58 ms (11.19x) |
| 256-bit | first matches | 43.63 ms | 28.67 ms (1.52x) | 5.93 ms (7.36x) |
| | unique check | 88.93 ms | 57.34 ms (1.55x) | 6.46 ms (13.77x) |
| 512-bit | first matches | 41.54 ms | 31.03 ms (1.34x) | 5.53 ms (7.51x) |
| | unique check | 85.11 ms | 61.02 ms (1.39x) | 6.03 ms (14.11x) |

Between runs, the per-needle ratios moved by at most 0.11 and the one-set ratios by at most 0.4. FastPattern's 128-bit build reports SSSE3, which its set scans use; 0x80.pl's 128-bit search is SSE2. With the original wildcard signatures, which those searches cannot take, FastPattern finds all first matches in one set in 8.71 ms and counts the matches to 2 in 10.24 ms. These figures, too, depend on the CPU, the data, and the signatures.

## Scope

FastPattern performs unanchored, fixed-length, case-sensitive byte matching. Its `?` wildcard consumes exactly one byte. It does not implement variable-length `*` globs, Unicode case folding, whole-string matching, linked-list traversal, indexes, or streaming state across chunks. For chunked input, callers must preserve up to `signature_length - 1` bytes between chunks to detect boundary-spanning matches.

Short names and pointer-heavy entity lists may spend little time searching strings, and may not benefit from SIMD. Whole-name equality, glob matching, and class indexes need algorithms appropriate to their semantics. Measure the complete workload before replacing an existing lookup.

See [CHANGELOG.md](CHANGELOG.md) for version changes. Released under the [MIT license](LICENSE).
