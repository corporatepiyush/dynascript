/*
 * Copyright 2026 Piyush Katariya
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Portable SIMD dispatch layer for ML kernels.
 *
 * Architecture:
 *   - Dispatch table (simd_t) holds function pointers for every kernel.
 *   - At init time, cpu_features() detects the best ISA available and
 *     simd_init_impl() installs the matching override.
 *   - Priority (highest first): AVX-512F+BW+DQ > AVX2+FMA > SSE4.2 > SVE > NEON
 * > Scalar
 *
 * Every kernel has a scalar fallback so the table is always fully populated.
 * Per-ISA modules (simd_{avx2,avx512,neon,sve}.c) override the slots
 * they can accelerate; unaccelerated slots retain the scalar implementation.
 *
 * All "float" kernels operate on float arrays; "double" variants use double.
 * "n" is element count, "d" is dimensionality, alignment is 32 bytes.
 */

#ifndef DYNAJS_SIMD_KERNELS_H
#define DYNAJS_SIMD_KERNELS_H

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* x86 SIMD intrinsics (__m128/__m256/_mm_*): the per-ISA files and the core's
 * hsum helper need these; the scl build pulled them via scl_ml.h, which we drop.
 * ARM NEON's arm_neon.h is included by the neon file under its own arch gate. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* ══════════════════════════════════════════════════════════════════
 * CPU feature flags
 * ══════════════════════════════════════════════════════════════════ */
typedef enum {
  CPU_SSE42 = 1u << 0,    /* x86 SSE4.2 */
  CPU_AVX2 = 1u << 1,     /* x86 AVX2 + FMA */
  CPU_AVX512F = 1u << 2,  /* x86 AVX-512 Foundation */
  CPU_AVX512BW = 1u << 3, /* x86 AVX-512 Byte/Word */
  CPU_AVX512DQ = 1u << 4, /* x86 AVX-512 DoubleWord/QuadWord */
  CPU_NEON = 1u << 5,     /* ARM64 NEON (ASIMD) */
  CPU_SVE = 1u << 6,      /* ARM64 SVE / SVE2 */
} cpu_feature_t;

/* Detect available CPU SIMD features at runtime.
 * Uses CPUID on x86-64 (leaf 1/7), getauxval(AT_HWCAP) on ARM64 Linux,
 * sysctl on macOS. Returns bitmask of cpu_feature_t. */
uint64_t cpu_features(void);

/* ══════════════════════════════════════════════════════════════════
 * Dispatch table — one entry per kernel
 * ══════════════════════════════════════════════════════════════════ */
typedef struct simd {
  /* ── BLAS Level 1: vector–vector ───────────────────────────── */
  float (*dot_f)(const float *restrict a, const float *restrict b,
                 size_t n);
  float (*dot)(const float *restrict a, const float *restrict b,
               size_t n);
  float (*norm_l2_sq)(const float *restrict x, size_t n);
  float (*norm_l2)(const float *restrict x, size_t n);
  float (*norm_l1)(const float *restrict x, size_t n);
  void (*axpy)(float *restrict y, float alpha, const float *restrict x,
               size_t n);
  void (*axpby)(float *restrict z, float a, const float *restrict x,
                float b, const float *restrict y, size_t n);

  /* ── Reductions ───────────────────────────────────────────── */
  float (*sum)(const float *restrict x, size_t n);
  float (*max)(const float *restrict x, size_t n);
  float (*min)(const float *restrict x, size_t n);
  size_t (*argmax)(const float *restrict x, size_t n);
  size_t (*argmin)(const float *restrict x, size_t n);
  void (*argminmax)(const float *restrict x, size_t n, size_t *argmin_out,
                    size_t *argmax_out);

  /* ── Element-wise: vector–vector → vector ────────────────── */
  void (*add)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*sub)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*mul)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*div)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);
  void (*abs)(float *restrict out, const float *restrict in, size_t n);
  void (*fma)(float *restrict z, const float *restrict a,
              const float *restrict b, size_t n);

  /* ── Element-wise: scalar–vector → vector ────────────────── */
  void (*add_s)(float *restrict z, const float *restrict x, float s,
                size_t n);
  void (*mul_s)(float *restrict z, const float *restrict x, float s,
                size_t n);
  void (*scale_add_s)(float *restrict z, float alpha,
                      const float *restrict x, float beta, size_t n);

  /* ── Activations (in-place safe: out may alias in) ───────── */
  void (*sigmoid)(float *restrict out, const float *restrict in,
                  size_t n);
  void (*relu)(float *restrict out, const float *restrict in, size_t n);
  void (*relu6)(float *restrict out, const float *restrict in,
                size_t n);
  void (*leaky_relu)(float *restrict out, const float *restrict in,
                     float slope, size_t n);
  void (*elu)(float *restrict out, const float *restrict in,
              float alpha, size_t n);
  void (*tanh_fast)(float *restrict out, const float *restrict in,
                    size_t n);
  void (*gelu)(float *restrict out, const float *restrict in, size_t n);
  void (*silu)(float *restrict out, const float *restrict in, size_t n);

  /* ── Softmax family ──────────────────────────────────────── */
  void (*softmax)(float *restrict out, const float *restrict in,
                  size_t n);
  void (*log_softmax)(float *restrict out, const float *restrict in,
                      size_t n);

  /* ── Element-wise unary math ──────────────────────────────── */
  void (*vexp)(float *restrict out, const float *restrict in, size_t n);
  void (*vlog)(float *restrict out, const float *restrict in, size_t n);
  void (*vsqrt)(float *restrict out, const float *restrict in,
                size_t n);
  void (*vrsqrt)(float *restrict out, const float *restrict in,
                 size_t n);
  void (*vinv)(float *restrict out, const float *restrict in, size_t n);

  /* ── Distance: vector–vector → scalar ────────────────────── */
  float (*dist_l2_sq)(const float *restrict a, const float *restrict b,
                      size_t d);
  float (*dist_l1)(const float *restrict a, const float *restrict b,
                   size_t d);
  float (*dist_cos)(const float *restrict a, const float *restrict b,
                    size_t d);
  float (*dist_cheb)(const float *restrict a, const float *restrict b,
                     size_t d);
  float (*dist_l2)(const float *restrict a, const float *restrict b,
                   size_t d);

  /* ── Distance matrix: batch computation ──────────────────── */
  void (*dist_matrix_l2_sq)(float *restrict out,
                            const float *restrict a,
                            const float *restrict b, size_t n, size_t m,
                            size_t d);
  void (*dist_matrix_cos)(float *restrict out, const float *restrict a,
                          const float *restrict b, size_t n, size_t m,
                          size_t d);
  void (*dist_matrix_l1)(float *restrict out, const float *restrict a,
                         const float *restrict b, size_t n, size_t m,
                         size_t d);

  /* ── BLAS Level 2: matrix–vector ─────────────────────────── */
  void (*gemv)(float *restrict y, const float *restrict a,
               const float *restrict x, size_t m, size_t n, float beta);
  void (*gemv_t)(float *restrict y, const float *restrict a,
                 const float *restrict x, size_t m, size_t n, float beta);

  /* ── BLAS Level 3: matrix–matrix ─────────────────────────── */
  void (*gemm)(float *restrict c, const float *restrict a,
               const float *restrict b, size_t m, size_t n, size_t k,
               float alpha, float beta);

  /* ── Comparison / Selection ──────────────────────────────── */
  void (*threshold)(float *restrict out, const float *restrict in,
                    float t, size_t n);
  void (*threshold_sign)(float *restrict out, const float *restrict in,
                         float t, size_t n);

  /* ── Hamming ─────────────────────────────────────────────── */
  float (*hamming)(const uint32_t *restrict a,
                   const uint32_t *restrict b, size_t n_words);

  /* ── Scalar fast math (for inner loops) ──────────────────── */
  float (*sigmoid_f)(float x);
  float (*tanh_f)(float x);
  float (*exp_f)(float x);

  /* ── Top-K selection ─────────────────────────────────────── */
  void (*topk_indices)(const float *restrict vals,
                       uint32_t *restrict indices, size_t n, size_t k);

  /* ── Clamp ───────────────────────────────────────────────── */
  void (*clamp)(float *restrict out, const float *restrict in, float lo,
                float hi, size_t n);

  /* ── Forward value search: first index of `v` in p[0..n), or SIZE_MAX.
   *    Used by String.indexOf/includes (find_u8/u16) and TypedArray
   *    indexOf/includes (find_u8/u16/u32/f32/f64). ─────────────────── */
  size_t (*find_u8)(const uint8_t *restrict p, uint8_t v, size_t n);
  size_t (*find_u16)(const uint16_t *restrict p, uint16_t v, size_t n);
  size_t (*find_u32)(const uint32_t *restrict p, uint32_t v, size_t n);
  size_t (*find_f32)(const float *restrict p, float v, size_t n);
  size_t (*find_f64)(const double *restrict p, double v, size_t n);

} simd_t;

/* Global dispatch table — safe to read from any thread after init. */
extern simd_t simd;

/* Initialize the dispatch table. Idempotent / thread-safe (uses once). */
void simd_init(void);

/* ══════════════════════════════════════════════════════════════════
 * Shared SIMD helpers (used across ISA backends)
 * ══════════════════════════════════════════════════════════════════ */

/* Horizontal sum of a float array using pairwise addition. */
static inline float simd_hsum_f32(const float *x, size_t n) {
  float acc = 0.0f;
  size_t i = 0;
  for (; i + 8 <= n; i += 8)
    acc += x[i] + x[i + 1] + x[i + 2] + x[i + 3] + x[i + 4] + x[i + 5] +
           x[i + 6] + x[i + 7];
  for (; i < n; i++)
    acc += x[i];
  return acc;
}

/* ══════════════════════════════════════════════════════════════════
 * Per-ISA override functions
 * ══════════════════════════════════════════════════════════════════ */
void simd_override_scalar(simd_t *t);
void simd_override_sse42(simd_t *t);
void simd_override_avx2(simd_t *t);
void simd_override_avx512(simd_t *t);
void simd_override_neon(simd_t *t);
void simd_override_sve(simd_t *t);

/* Shared helper: scalar top-k used by NEON and other ISAs. */
void simd_scalar_topk_indices(const float *restrict vals,
                                     uint32_t *restrict indices, size_t n,
                                     size_t k);

/* ══════════════════════════════════════════════════════════════════
 * Inline helpers
 * ══════════════════════════════════════════════════════════════════ */

/* Fast exponent approximation: Schraudolph's method.
 * e^x ≈ 2^(log2(e)·x) via IEEE-754 bit hack. ~1e-4 rel error for x∈[-88,88]. */
static inline float fast_exp(float x) {
  if (unlikely(x < -88.0f))
    return 0.0f;
  if (unlikely(x > 88.0f))
    return INFINITY;
  union {
    float f;
    int32_t i;
  } u;
  u.i = (int32_t)(12102203.0f * x + 1065353216.0f);
  return u.f;
}

static inline float fast_sigmoid(float x) {
  if (unlikely(x < -30.0f))
    return 0.0f;
  if (unlikely(x > 30.0f))
    return 1.0f;
  float ex = fast_exp(-x);
  return 1.0f / (1.0f + ex);
}

static inline float fast_tanh(float x) {
  if (unlikely(x < -10.0f))
    return -1.0f;
  if (unlikely(x > 10.0f))
    return 1.0f;
  return 2.0f * fast_sigmoid(2.0f * x) - 1.0f;
}

static inline bool is_pow2(size_t v) { return v && !(v & (v - 1)); }
static inline size_t align_up(size_t v, size_t a) {
  return (v + a - 1) & ~(a - 1);
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#endif /* DYNAJS_SIMD_KERNELS_H */