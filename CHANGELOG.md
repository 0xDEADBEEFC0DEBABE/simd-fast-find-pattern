# Changelog

## Unreleased (2.0.0)

This release replaces the search API. See [Migrating from 1.x](README.md#migrating-from-1x).

- **Breaking:** one interface for one or many signatures. `fp_set_from_text` and `fp_set_from_bytes` build an immutable set, `fp_scan` searches a buffer once for all of its signatures, returning the lowest matching offset and, on request, each signature's first offset, and `fp_set_count` and `fp_set_free` complete it. `fp_find`, `fp_find_pattern`, `fp_compile`, `fp_find_compiled`, `fp_free` and `fp_pattern` are removed.
- **Breaking:** construction failures report why (`FP_ERROR_ARGUMENT`, `FP_ERROR_EMPTY`, `FP_ERROR_SYNTAX`, `FP_ERROR_MEMORY`), which signature, and for syntax errors the offset of the bad token, instead of a bare `NULL` or `SIZE_MAX`.
- x86 sets of five or more signatures search with Teddy, using SSSE3, AVX2, or AVX-512BW. Each signature is fingerprinted on the 4 consecutive bytes least likely to occur in x86-64 code, by byte frequencies measured on Windows system binaries; wildcards and nibble masks cost nothing in its nibble tables. Buckets are filled by the estimated cost of the false candidates they admit, and a bucket leaves the scan once its signatures are found. Where candidates grow dense, as in padding, a group gives way to single-signature searches that resume where it stopped. Other backends, and smaller sets, search each signature in turn.
- Add an SSSE3 backend level, reported by `fp_cpuinfo`.
- A set stores each masked signature's bytes, from the first to the last constrained one, followed by their mask, so masked single-signature searches reach their SIMD code through tail jumps alone, with all arguments in registers on Win64.
- AArch64 NEON: build candidate masks with `SHRN` instead of `UMAXV` plus a 16-lane store and scan, skip anchor masking when both anchors are exact bytes, verify candidates outside the scan loop so the broadcast vectors stay in registers, and finish with an overlapping final vector instead of a scalar tail.
- Enable the NEON backend for MSVC ARM64 builds, which previously used the scalar path.
- x86: verify candidates with inline word compares instead of `memcmp`, keep calls out of the scan loops, unroll them twice, and finish with an overlapping final vector instead of a scalar tail.
- Pair the first anchor with the last differing exact byte when the first and last pattern bytes are equal, and on x86 narrow dense candidate blocks one pattern byte at a time across all lanes.
- x86 exact signatures, and masked ones whose first and last constrained bytes are exact, reach a small leaf function through a few conditional tail jumps. The leaves are sized to the number of starts, and 8- to 16-byte signatures get a branch-free two-word verification; AVX-512 uses masked loads for the shortest buffers.
- x86 long scans start where the first anchor's vector loads are aligned, so throughput depends less on the buffer's alignment.
- Read the cached x86 backend without a locked instruction; clang builds targeting MSVC previously paid an interlocked compare-exchange on every search.
- Tests: signature sets at random and directed (fingerprints deep inside signatures, floods that force the single-signature fallback, sets at guard pages), dense candidates, long exact searches, masked signatures with exact ends, low-entropy data, and guard-page checks up to 4096 bytes. CI: Linux GCC and Windows MSVC ARM64 jobs.
- The benchmark times single signatures through the return value, through offsets, and with construction on every call, and sets of 2 to 256 signatures against a search per signature.

## 1.1.0

- Fix short-buffer SIMD bounds handling and keep vector reads within the supplied buffer.
- Isolate x86 SIMD code behind per-function compiler targets and runtime CPU/OS feature checks, preserving portable baseline builds.
- Add multi-byte masked SIMD filtering for SSE2, AVX2, and AVX-512BW, AArch64 NEON support, and a portable scalar fallback.
- Parse complete whitespace-separated patterns without truncating single `?` tokens. Reject malformed and empty input consistently.
- Add nibble wildcards (`A?`, `?F`) and document raw bit-mask comparison semantics.
- Add reusable immutable compiled patterns through `fp_compile`, `fp_find_compiled`, and `fp_free`, retaining the existing four entry points.
- Add CMake builds, regression tests, sanitizer options, and a scalar-only configuration.
- Replace the external DLL example and unverifiable benchmark claims with self-contained examples and deterministic benchmarks that validate results before timing.
- Document API errors, ownership, portability, runtime dispatch, and the limits of applying fixed-length byte searching to other matching problems.
