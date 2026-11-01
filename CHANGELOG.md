# Changelog

## Unreleased

- AArch64 NEON: build candidate masks with `SHRN` instead of `UMAXV` plus a 16-lane store and scan, skip anchor masking when both anchors are exact bytes, verify candidates outside the scan loop so the broadcast vectors stay in registers, and finish with an overlapping final vector instead of a scalar tail.
- Enable the NEON backend for MSVC ARM64 builds, which previously used the scalar path.
- x86: verify candidates with inline word compares instead of `memcmp`, keep calls out of the scan loops, unroll them twice, and finish with an overlapping final vector instead of a scalar tail.
- Pair the first anchor with the last differing exact byte when the first and last pattern bytes are equal, and on x86 narrow dense candidate blocks one pattern byte at a time across all lanes.
- x86 exact searches skip planning: `fp_find`, with no stack frame, reaches a small leaf function through a few conditional tail jumps. The leaves are sized to the number of starts, and 8- to 16-byte patterns get a branch-free two-word verification; AVX-512 uses masked loads for the shortest buffers.
- Read the cached x86 backend without a locked instruction; clang builds targeting MSVC previously paid an interlocked compare-exchange on every search.
- Add a dense-candidate regression test, long exact-search and low-entropy random tests, guard-page checks up to 4096 bytes, plus Linux GCC and Windows MSVC ARM64 CI jobs.

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
