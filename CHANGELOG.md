# Changelog

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
