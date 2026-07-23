/*
 * Copyright 2026 Piyush Katariya
 *
 * Licensed under the Apache License, Version 2.0; see LICENSE.
 */

/* ARM64 NEON (ASIMD) SIMD overrides — 128-bit registers, 4 floats per vector.
 * Compiled only on ARM64 (aarch64).
 * Covers the most common BLAS-1 kernels for ARM platforms. */

#include <math.h>
#include "dynajs-simd-kernels.h"
#include <math.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>

/* ── Dot product (float accumulator) ─────────────────────────── */
static float simd_neon_dot(const float *restrict a,
                                  const float *restrict b, size_t n) {
  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    acc = vmlaq_f32(acc, va, vb);
  }
  float result = vaddvq_f32(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

/* ── Dot product (double accumulator for stability) ──────────── */
static float simd_neon_dot_f(const float *restrict a,
                                    const float *restrict b, size_t n) {
  float64x2_t acc0 = vdupq_n_f64(0.0);
  float64x2_t acc1 = vdupq_n_f64(0.0);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    float32x4_t prod = vmulq_f32(va, vb);
    acc0 = vaddq_f64(acc0, vcvt_f64_f32(vget_low_f32(prod)));
    acc1 = vaddq_f64(acc1, vcvt_f64_f32(vget_high_f32(prod)));
  }
  double result = vaddvq_f64(acc0) + vaddvq_f64(acc1);
  for (; i < n; i++)
    result += (double)a[i] * b[i];
  return (float)result;
}

/* ── Norm L2 squared ────────────────────────────────────────── */
static float simd_neon_norm_l2_sq(const float *restrict x,
                                         size_t n) {
  return simd_neon_dot(x, x, n);
}

static float simd_neon_norm_l2(const float *restrict x, size_t n) {
  return sqrtf(simd_neon_norm_l2_sq(x, n));
}

/* ── Norm L1 ─────────────────────────────────────────────────── */
static float simd_neon_norm_l1(const float *restrict x, size_t n) {
  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vx = vld1q_f32(&x[i]);
    acc = vaddq_f32(acc, vabsq_f32(vx));
  }
  float result = vaddvq_f32(acc);
  for (; i < n; i++)
    result += fabsf(x[i]);
  return result;
}

/* ── Sum ─────────────────────────────────────────────────────── */
static float simd_neon_sum(const float *restrict x, size_t n) {
  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    acc = vaddq_f32(acc, vld1q_f32(&x[i]));
  float result = vaddvq_f32(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

/* ── Max ─────────────────────────────────────────────────────── */
static float simd_neon_max(const float *restrict x, size_t n) {
  if (n == 0)
    return -FLT_MAX;
  size_t i;
  float result;
  if (n >= 4) { /* only seed a 4-wide vector when >=4 elements exist (OOB else) */
    float32x4_t vmax = vld1q_f32(x);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = vmaxq_f32(vmax, vld1q_f32(&x[i]));
    result = vmaxvq_f32(vmax);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

/* ── Min ─────────────────────────────────────────────────────── */
static float simd_neon_min(const float *restrict x, size_t n) {
  if (n == 0)
    return FLT_MAX;
  size_t i;
  float result;
  if (n >= 4) { /* only seed a 4-wide vector when >=4 elements exist (OOB else) */
    float32x4_t vmin = vld1q_f32(x);
    for (i = 4; i + 4 <= n; i += 4)
      vmin = vminq_f32(vmin, vld1q_f32(&x[i]));
    result = vminvq_f32(vmin);
  } else {
    result = x[0];
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

/* ── Argmax ──────────────────────────────────────────────────── */
static size_t simd_neon_argmax(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  size_t i;
  float best;
  size_t best_idx;
  if (n >= 4) { /* only seed a 4-wide vector when >=4 elements exist (OOB else) */
    float32x4_t vmax = vld1q_f32(x);
    uint32x4_t vidx = {0, 1, 2, 3};
    uint32x4_t vidx_max = vidx;
    for (i = 4; i + 4 <= n; i += 4) {
      float32x4_t vi = vld1q_f32(&x[i]);
      uint32x4_t vidxi = vmovq_n_u32((uint32_t)i);
      uint32x4_t step = {0, 1, 2, 3};
      vidxi = vaddq_u32(vidxi, step);
      uint32x4_t mask = vcgtq_f32(vi, vmax);
      vmax = vmaxq_f32(vmax, vi);
      vidx_max = vbslq_u32(mask, vidxi, vidx_max);
    }
    float tmp[4];
    uint32_t idx_tmp[4];
    vst1q_f32(tmp, vmax);
    vst1q_u32(idx_tmp, vidx_max);
    best = tmp[0];
    best_idx = idx_tmp[0];
    for (size_t k = 1; k < 4; k++)
      if (tmp[k] > best) {
        best = tmp[k];
        best_idx = idx_tmp[k];
      }
  } else {
    best = x[0];
    best_idx = 0;
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] > best) {
      best = x[i];
      best_idx = i;
    }
  return best_idx;
}

/* ── Argmin ──────────────────────────────────────────────────── */
static size_t simd_neon_argmin(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  size_t i;
  float best;
  size_t best_idx;
  if (n >= 4) { /* only seed a 4-wide vector when >=4 elements exist (OOB else) */
    float32x4_t vmin = vld1q_f32(x);
    uint32x4_t vidx = {0, 1, 2, 3};
    uint32x4_t vidx_min = vidx;
    for (i = 4; i + 4 <= n; i += 4) {
      float32x4_t vi = vld1q_f32(&x[i]);
      uint32x4_t vidxi = vmovq_n_u32((uint32_t)i);
      uint32x4_t step = {0, 1, 2, 3};
      vidxi = vaddq_u32(vidxi, step);
      uint32x4_t mask = vcltq_f32(vi, vmin);
      vmin = vminq_f32(vmin, vi);
      vidx_min = vbslq_u32(mask, vidxi, vidx_min);
    }
    float tmp[4];
    uint32_t idx_tmp[4];
    vst1q_f32(tmp, vmin);
    vst1q_u32(idx_tmp, vidx_min);
    best = tmp[0];
    best_idx = idx_tmp[0];
    for (size_t k = 1; k < 4; k++)
      if (tmp[k] < best) {
        best = tmp[k];
        best_idx = idx_tmp[k];
      }
  } else {
    best = x[0];
    best_idx = 0;
    i = 1;
  }
  for (; i < n; i++)
    if (x[i] < best) {
      best = x[i];
      best_idx = i;
    }
  return best_idx;
}

/* ── Argminmax ───────────────────────────────────────────────── */
static void simd_neon_argminmax(const float *restrict x, size_t n,
                                       size_t *argmin_out, size_t *argmax_out) {
  if (n == 0) {
    *argmin_out = *argmax_out = 0;
    return;
  }
  size_t i;
  float best_min, best_max;
  size_t imin, imax;
  if (n >= 4) { /* only seed a 4-wide vector when >=4 elements exist (OOB else) */
    float32x4_t vmin = vld1q_f32(x);
    float32x4_t vmax = vmin;
    uint32x4_t vidx = {0, 1, 2, 3};
    uint32x4_t vidx_min = vidx, vidx_max = vidx;
    for (i = 4; i + 4 <= n; i += 4) {
      float32x4_t vi = vld1q_f32(&x[i]);
      uint32x4_t vidxi = vmovq_n_u32((uint32_t)i);
      uint32x4_t step = {0, 1, 2, 3};
      vidxi = vaddq_u32(vidxi, step);
      uint32x4_t mask_lt = vcltq_f32(vi, vmin);
      uint32x4_t mask_gt = vcgtq_f32(vi, vmax);
      vmin = vminq_f32(vmin, vi);
      vmax = vmaxq_f32(vmax, vi);
      vidx_min = vbslq_u32(mask_lt, vidxi, vidx_min);
      vidx_max = vbslq_u32(mask_gt, vidxi, vidx_max);
    }
    float tmp_min[4], tmp_max[4];
    uint32_t idx_min[4], idx_max[4];
    vst1q_f32(tmp_min, vmin);
    vst1q_f32(tmp_max, vmax);
    vst1q_u32(idx_min, vidx_min);
    vst1q_u32(idx_max, vidx_max);
    best_min = tmp_min[0], best_max = tmp_max[0];
    imin = idx_min[0], imax = idx_max[0];
    for (size_t k = 1; k < 4; k++) {
      if (tmp_min[k] < best_min) {
        best_min = tmp_min[k];
        imin = idx_min[k];
      }
      if (tmp_max[k] > best_max) {
        best_max = tmp_max[k];
        imax = idx_max[k];
      }
    }
  } else {
    best_min = best_max = x[0];
    imin = imax = 0;
    i = 1;
  }
  for (; i < n; i++) {
    if (x[i] < best_min) {
      best_min = x[i];
      imin = i;
    }
    if (x[i] > best_max) {
      best_max = x[i];
      imax = i;
    }
  }
  *argmin_out = imin;
  *argmax_out = imax;
}

/* ── Element-wise: vector–vector ─────────────────────────────── */
static void simd_neon_add(float *restrict z,
                                 const float *restrict a,
                                 const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    vst1q_f32(&z[i], vaddq_f32(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] + b[i];
}

static void simd_neon_sub(float *restrict z,
                                 const float *restrict a,
                                 const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    vst1q_f32(&z[i], vsubq_f32(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] - b[i];
}

static void simd_neon_mul(float *restrict z,
                                 const float *restrict a,
                                 const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    vst1q_f32(&z[i], vmulq_f32(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] * b[i];
}

static void simd_neon_div(float *restrict z,
                                 const float *restrict a,
                                 const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    /* NEON has no direct div; use reciprocal estimate + Newton-Raphson */
    float32x4_t recip = vrecpeq_f32(vb);
    recip = vmulq_f32(vrecpeq_f32(vb), vrecpsq_f32(vb, recip));
    recip = vmulq_f32(vrecpeq_f32(vb), vrecpsq_f32(vb, recip));
    vst1q_f32(&z[i], vmulq_f32(va, recip));
  }
  for (; i < n; i++)
    z[i] = a[i] / b[i];
}

static void simd_neon_abs(float *restrict out,
                                 const float *restrict in, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    vst1q_f32(&out[i], vabsq_f32(vi));
  }
  for (; i < n; i++)
    out[i] = fabsf(in[i]);
}

static void simd_neon_fma(float *restrict z,
                                 const float *restrict a,
                                 const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vz = vld1q_f32(&z[i]);
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    vst1q_f32(&z[i], vmlaq_f32(vz, va, vb));
  }
  for (; i < n; i++)
    z[i] += a[i] * b[i];
}

/* ── Element-wise: scalar–vector ──────────────────────────────── */
static void simd_neon_add_s(float *restrict z,
                                   const float *restrict x, float s,
                                   size_t n) {
  float32x4_t vs = vdupq_n_f32(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vx = vld1q_f32(&x[i]);
    vst1q_f32(&z[i], vaddq_f32(vx, vs));
  }
  for (; i < n; i++)
    z[i] = x[i] + s;
}

static void simd_neon_mul_s(float *restrict z,
                                   const float *restrict x, float s,
                                   size_t n) {
  float32x4_t vs = vdupq_n_f32(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vx = vld1q_f32(&x[i]);
    vst1q_f32(&z[i], vmulq_f32(vx, vs));
  }
  for (; i < n; i++)
    z[i] = x[i] * s;
}

static void simd_neon_scale_add_s(float *restrict z, float alpha,
                                         const float *restrict x,
                                         float beta, size_t n) {
  float32x4_t va = vdupq_n_f32(alpha);
  float32x4_t vb = vdupq_n_f32(beta);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vx = vld1q_f32(&x[i]);
    vst1q_f32(&z[i], vmlaq_f32(vb, va, vx));
  }
  for (; i < n; i++)
    z[i] = alpha * x[i] + beta;
}

/* ── Activations ──────────────────────────────────────────────── */
static void simd_neon_sigmoid(float *restrict out,
                                     const float *restrict in, size_t n) {
  float32x4_t one = vdupq_n_f32(1.0f);
  float32x4_t lo = vdupq_n_f32(-30.0f);
  float32x4_t hi = vdupq_n_f32(30.0f);
  float32x4_t magic = vdupq_n_f32(-12102203.0f);
  float32x4_t bias = vdupq_n_f32(1.0f);
  uint32x4_t zero = vdupq_n_u32(0);
  uint32x4_t top = vdupq_n_u32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    vi = vmaxq_f32(vminq_f32(vi, hi), lo);
    int32x4_t bits = vcvtq_s32_f32(vmulq_f32(vi, magic));
    bits = vaddq_s32(bits, vreinterpretq_s32_f32(bias));
    bits = vmaxq_s32(bits, vreinterpretq_s32_u32(zero));
    bits = vminq_s32(bits, vreinterpretq_s32_u32(top));
    float32x4_t ve = vaddq_f32(one, vreinterpretq_f32_s32(bits));
    vst1q_f32(&out[i], vdivq_f32(one, ve));
  }
  for (; i < n; i++)
    out[i] = fast_sigmoid(in[i]);
}

/* tanh via fast exp: tanh(x) = 2*sigmoid(2x) - 1 */
static void simd_neon_tanh_fast(float *restrict out,
                                       const float *restrict in, size_t n) {
  float32x4_t two = vdupq_n_f32(2.0f);
  float32x4_t one = vdupq_n_f32(1.0f);
  float32x4_t lo = vdupq_n_f32(-10.0f);
  float32x4_t hi = vdupq_n_f32(10.0f);
  float32x4_t magic = vdupq_n_f32(-12102203.0f * 2.0f);
  float32x4_t bias = vdupq_n_f32(1.0f);
  uint32x4_t zero = vdupq_n_u32(0);
  uint32x4_t top = vdupq_n_u32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    vi = vmaxq_f32(vminq_f32(vi, hi), lo);
    int32x4_t bits = vcvtq_s32_f32(vmulq_f32(vi, magic));
    bits = vaddq_s32(bits, vreinterpretq_s32_f32(bias));
    bits = vmaxq_s32(bits, vreinterpretq_s32_u32(zero));
    bits = vminq_s32(bits, vreinterpretq_s32_u32(top));
    float32x4_t ve = vsubq_f32(
        vdivq_f32(two, vaddq_f32(one, vreinterpretq_f32_s32(bits))), one);
    vst1q_f32(&out[i], ve);
  }
  for (; i < n; i++)
    out[i] = fast_tanh(in[i]);
}

static void simd_neon_gelu(float *restrict out,
                                  const float *restrict in, size_t n) {
  const float sqrt_2_over_pi = 0.7978845608028654f;
  const float c = 0.044715f;
  float32x4_t vsop = vdupq_n_f32(sqrt_2_over_pi);
  float32x4_t vc4 = vdupq_n_f32(c);
  float32x4_t half = vdupq_n_f32(0.5f);
  float32x4_t one = vdupq_n_f32(1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t x = vld1q_f32(&in[i]);
    float32x4_t x3 = vmulq_f32(vmulq_f32(x, x), x);
    float32x4_t inner = vmulq_f32(vsop, vmlaq_f32(x, vc4, x3));
    /* Per-lane libm tanh so lanes match the scalar tail exactly. */
    {
      float tmp[4];
      vst1q_f32(tmp, inner);
      for (int j = 0; j < 4; j++)
        tmp[j] = tanhf(tmp[j]);
      inner = vld1q_f32(tmp);
    }
    vst1q_f32(&out[i], vmulq_f32(vmulq_f32(half, x), vaddq_f32(one, inner)));
  }
  for (; i < n; i++) {
    float x = in[i];
    float x3 = x * x * x;
    float inner = tanhf(sqrt_2_over_pi * (x + c * x3));
    out[i] = 0.5f * x * (1.0f + inner);
  }
}

static void simd_neon_silu(float *restrict out,
                                  const float *restrict in, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t x = vld1q_f32(&in[i]);
    float32x4_t neg_x = vnegq_f32(x);
    /* Fast exp approx of neg_x = exp(-x). Schraudolph magic is POSITIVE
     * (2^23/ln2 ≈ 12102203); a negative constant here double-negates and
     * computes exp(+x) => sigmoid(-x) => silu(-x) (the historical bug). */
    float32x4_t magic = vdupq_n_f32(12102203.0f);
    float32x4_t bias = vdupq_n_f32(1.0f);
    int32x4_t bits = vcvtq_s32_f32(vmulq_f32(neg_x, magic));
    bits = vaddq_s32(bits, vreinterpretq_s32_f32(bias));
    bits = vmaxq_s32(bits, vreinterpretq_s32_u32(vdupq_n_u32(0)));
    bits = vminq_s32(bits, vreinterpretq_s32_u32(vdupq_n_u32(0x7F800000)));
    float32x4_t exp_neg_x =
        vmaxq_f32(vreinterpretq_f32_s32(bits), vdupq_n_f32(0.0f));
    float32x4_t sigmoid_x =
        vdivq_f32(vdupq_n_f32(1.0f), vaddq_f32(vdupq_n_f32(1.0f), exp_neg_x));
    vst1q_f32(&out[i], vmulq_f32(x, sigmoid_x));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v / (1.0f + expf(-v));
  }
}

static void simd_neon_relu(float *restrict out,
                                  const float *restrict in, size_t n) {
  float32x4_t zero = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    vst1q_f32(&out[i], vmaxq_f32(vi, zero));
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

static void simd_neon_relu6(float *restrict out,
                                   const float *restrict in, size_t n) {
  float32x4_t zero = vdupq_n_f32(0.0f);
  float32x4_t six = vdupq_n_f32(6.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    vst1q_f32(&out[i], vminq_f32(vmaxq_f32(vi, zero), six));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
  }
}

static void simd_neon_leaky_relu(float *restrict out,
                                        const float *restrict in,
                                        float slope, size_t n) {
  float32x4_t vzero = vdupq_n_f32(0.0f);
  float32x4_t vslope = vdupq_n_f32(slope);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    uint32x4_t mask = vcgtq_f32(vi, vzero);
    float32x4_t neg = vmulq_f32(vi, vslope);
    vst1q_f32(&out[i], vbslq_f32(mask, vi, neg));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : v * slope;
  }
}

/* Apple clang's arm_neon.h may lack vexpq_f32 on some SDKs; provide a
 * portable vector-exp fallback using expf unpacked across lanes. */
static inline float32x4_t vexpq_f32(float32x4_t vi) {
  float v[4] __attribute__((aligned(16)));
  vst1q_f32(v, vi);
  for (int i = 0; i < 4; i++)
    v[i] = expf(v[i]);
  return vld1q_f32(v);
}

static void simd_neon_elu(float *restrict out,
                                 const float *restrict in, float alpha,
                                 size_t n) {
  float32x4_t vzero = vdupq_n_f32(0.0f);
  float32x4_t valpha = vdupq_n_f32(alpha);
  float32x4_t vone = vdupq_n_f32(1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    uint32x4_t mask = vcgtq_f32(vi, vzero);
    float32x4_t exp_part = vsubq_f32(vexpq_f32(vi), vone);
    float32x4_t neg = vmulq_f32(valpha, exp_part);
    vst1q_f32(&out[i], vbslq_f32(mask, vi, neg));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
  }
}

/* Vectorized Schraudolph exp, matching fast_exp bit-for-bit:
 * bits = (int)(12102203·x + 0x3F800000), clamped to [0, +inf bits] so
 * out-of-range inputs saturate to 0 / +inf instead of garbage. */
static inline float32x4_t neon_fast_exp_f32(float32x4_t x) {
  int32x4_t bits = vcvtq_s32_f32(
      vmlaq_f32(vdupq_n_f32(1065353216.0f), x, vdupq_n_f32(12102203.0f)));
  bits = vmaxq_s32(bits, vdupq_n_s32(0));
  bits = vminq_s32(bits, vdupq_n_s32(0x7F800000));
  return vreinterpretq_f32_s32(bits);
}

static void simd_neon_softmax(float *restrict out,
                                     const float *restrict in, size_t n) {
  if (unlikely(n == 0))
    return;
  /* Max reduction. The 4-wide head load is only safe when n >= 4 —
   * softmax is routinely called with tiny n (class counts). */
  float maxv = in[0];
  size_t i = 1;
  if (n >= 4) {
    float32x4_t vmax = vld1q_f32(in);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = vmaxq_f32(vmax, vld1q_f32(&in[i]));
    maxv = vmaxvq_f32(vmax);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  /* exp(x - max), accumulating the sum in the same pass. */
  float32x4_t vmaxv = vdupq_n_f32(maxv);
  float32x4_t vsum = vdupq_n_f32(0.0f);
  i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t ve = neon_fast_exp_f32(vsubq_f32(vld1q_f32(&in[i]), vmaxv));
    vst1q_f32(&out[i], ve);
    vsum = vaddq_f32(vsum, ve);
  }
  float sum = vaddvq_f32(vsum);
  for (; i < n; i++) {
    out[i] = fast_exp(in[i] - maxv);
    sum += out[i];
  }

  float inv_sum = 1.0f / sum;
  float32x4_t vinv = vdupq_n_f32(inv_sum);
  for (i = 0; i + 4 <= n; i += 4)
    vst1q_f32(&out[i], vmulq_f32(vld1q_f32(&out[i]), vinv));
  for (; i < n; i++)
    out[i] *= inv_sum;
}

static void simd_neon_log_softmax(float *restrict out,
                                         const float *restrict in,
                                         size_t n) {
  if (unlikely(n == 0))
    return;
  float maxv = in[0];
  size_t i = 1;
  if (n >= 4) {
    float32x4_t vmax = vld1q_f32(in);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = vmaxq_f32(vmax, vld1q_f32(&in[i]));
    maxv = vmaxvq_f32(vmax);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  /* Only the scalar sum of exps is needed — nothing is stored yet. */
  float32x4_t vmaxv = vdupq_n_f32(maxv);
  float32x4_t vsum = vdupq_n_f32(0.0f);
  i = 0;
  for (; i + 4 <= n; i += 4)
    vsum =
        vaddq_f32(vsum, neon_fast_exp_f32(vsubq_f32(vld1q_f32(&in[i]), vmaxv)));
  float sum = vaddvq_f32(vsum);
  for (; i < n; i++)
    sum += fast_exp(in[i] - maxv);

  float log_s = logf(sum);
  float32x4_t voff = vdupq_n_f32(maxv + log_s);
  for (i = 0; i + 4 <= n; i += 4)
    vst1q_f32(&out[i], vsubq_f32(vld1q_f32(&in[i]), voff));
  for (; i < n; i++)
    out[i] = in[i] - maxv - log_s;
}

static void simd_neon_vexp(float *restrict out,
                                  const float *restrict in, size_t n) {
  float32x4_t magic = vdupq_n_f32(12102203.0f);
  float32x4_t bias = vdupq_n_f32(1.0f);
  float32x4_t vzero = vdupq_n_f32(0.0f);
  uint32x4_t top = vdupq_n_u32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    int32x4_t bits = vcvtq_s32_f32(vmulq_f32(vi, magic));
    bits = vaddq_s32(bits, vreinterpretq_s32_f32(bias));
    bits = vmaxq_s32(bits, vreinterpretq_s32_u32(vdupq_n_u32(0)));
    bits = vminq_s32(bits, vreinterpretq_s32_u32(top));
    float32x4_t ve = vreinterpretq_f32_s32(bits);
    vst1q_f32(&out[i], vmaxq_f32(ve, vzero));
  }
  for (; i < n; i++)
    out[i] = fast_exp(in[i]);
}

static void simd_neon_vlog(float *restrict out,
                                  const float *restrict in, size_t n) {
  float32x4_t vzero = vdupq_n_f32(0.0f);
  float32x4_t vneg = vdupq_n_f32(-FLT_MAX);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    uint32x4_t mask = vcgtq_f32(vi, vzero);
    float32x4_t vln = vrecpeq_f32(vi); /* Approximate 1/x as placeholder */
    /* Accurate log requires scalar — use the real logf per-lane */
    float tmp[4];
    vst1q_f32(tmp, vi);
    for (int j = 0; j < 4; j++)
      tmp[j] = tmp[j] > 0.0f ? logf(tmp[j]) : -FLT_MAX;
    vln = vld1q_f32(tmp);
    vst1q_f32(&out[i], vbslq_f32(mask, vln, vneg));
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

static void simd_neon_vsqrt(float *restrict out,
                                   const float *restrict in, size_t n) {
  float32x4_t vzero = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    uint32x4_t mask = vcgeq_f32(vi, vzero);
    float32x4_t vs = vrsqrteq_f32(vi);
    /* Newton-Raphson refinement: 1/sqrt(x) */
    vs = vmulq_f32(vrsqrtsq_f32(vmulq_f32(vi, vs), vs), vs);
    vs = vmulq_f32(vrsqrtsq_f32(vmulq_f32(vi, vs), vs), vs);
    /* vs = 1/sqrt(x), so sqrt(x) = x * 1/sqrt(x) = x * vs */
    float32x4_t result = vmulq_f32(vi, vs);
    vst1q_f32(&out[i], vbslq_f32(mask, result, vzero));
  }
  for (; i < n; i++)
    out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

static void simd_neon_vrsqrt(float *restrict out,
                                    const float *restrict in, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    float32x4_t vs = vrsqrteq_f32(vi);
    vs = vmulq_f32(vrsqrtsq_f32(vmulq_f32(vi, vs), vs), vs);
    vs = vmulq_f32(vrsqrtsq_f32(vmulq_f32(vi, vs), vs), vs);
    vst1q_f32(&out[i], vs);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
  }
}

static void simd_neon_vinv(float *restrict out,
                                  const float *restrict in, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    float32x4_t vs = vrecpeq_f32(vi);
    vs = vmulq_f32(vrecpsq_f32(vi, vs), vs);
    vs = vmulq_f32(vrecpsq_f32(vi, vs), vs);
    vst1q_f32(&out[i], vs);
  }
  for (; i < n; i++)
    out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

/* ── Distance: vector–vector ─────────────────────────────────── */
static float simd_neon_dist_l2_sq(const float *restrict a,
                                         const float *restrict b,
                                         size_t d) {
  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    float32x4_t diff = vsubq_f32(va, vb);
    acc = vmlaq_f32(acc, diff, diff);
  }
  float result = vaddvq_f32(acc);
  for (; i < d; i++) {
    float df = a[i] - b[i];
    result += df * df;
  }
  return result;
}

static float simd_neon_dist_l1(const float *restrict a,
                                      const float *restrict b, size_t d) {
  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    float32x4_t diff = vsubq_f32(va, vb);
    acc = vaddq_f32(acc, vabsq_f32(diff));
  }
  float result = vaddvq_f32(acc);
  for (; i < d; i++)
    result += fabsf(a[i] - b[i]);
  return result;
}

static float simd_neon_dist_cos(const float *restrict a,
                                       const float *restrict b, size_t d) {
  float32x4_t vdot = vdupq_n_f32(0.0f);
  float32x4_t vna = vdupq_n_f32(0.0f);
  float32x4_t vnb = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    vdot = vmlaq_f32(vdot, va, vb);
    vna = vmlaq_f32(vna, va, va);
    vnb = vmlaq_f32(vnb, vb, vb);
  }
  float dot = vaddvq_f32(vdot);
  float na = vaddvq_f32(vna);
  float nb = vaddvq_f32(vnb);
  for (; i < d; i++) {
    dot += (double)a[i] * b[i];
    na += (double)a[i] * a[i];
    nb += (double)b[i] * b[i];
  }
  double denom = sqrt(na * nb);
  if (denom < FLT_MIN)
    return 1.0f;
  return (float)(1.0 - dot / denom);
}

static float simd_neon_dist_cheb(const float *restrict a,
                                        const float *restrict b, size_t d) {
  float32x4_t vmax = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    float32x4_t va = vld1q_f32(&a[i]);
    float32x4_t vb = vld1q_f32(&b[i]);
    float32x4_t adiff = vabsq_f32(vsubq_f32(va, vb));
    vmax = vmaxq_f32(vmax, adiff);
  }
  float result = vmaxvq_f32(vmax);
  for (; i < d; i++) {
    float df = fabsf(a[i] - b[i]);
    if (df > result)
      result = df;
  }
  return result;
}

static float simd_neon_dist_l2(const float *restrict a,
                                      const float *restrict b, size_t d) {
  return sqrtf(simd_neon_dist_l2_sq(a, b, d));
}

/* ── Distance matrix ──────────────────────────────────────────── */
static void simd_neon_dist_matrix_l2_sq(float *restrict out,
                                               const float *restrict a,
                                               const float *restrict b,
                                               size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_neon_dist_l2_sq(&a[i * d], &b[j * d], d);
}

static void simd_neon_dist_matrix_l1(float *restrict out,
                                            const float *restrict a,
                                            const float *restrict b,
                                            size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_neon_dist_l1(&a[i * d], &b[j * d], d);
}

static void simd_neon_dist_matrix_cos(float *restrict out,
                                             const float *restrict a,
                                             const float *restrict b,
                                             size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_neon_dist_cos(&a[i * d], &b[j * d], d);
}

/* ── BLAS Level 2: gemv ───────────────────────────────────────── */
static void simd_neon_gemv(float *restrict y,
                                  const float *restrict a,
                                  const float *restrict x, size_t m,
                                  size_t n, float beta) {
  for (size_t i = 0; i < m; i++) {
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    size_t j = 0;
    for (; j + 8 <= n; j += 8) {
      acc0 = vmlaq_f32(acc0, vld1q_f32(&a[i * n + j]), vld1q_f32(&x[j]));
      acc1 =
          vmlaq_f32(acc1, vld1q_f32(&a[i * n + j + 4]), vld1q_f32(&x[j + 4]));
    }
    for (; j + 4 <= n; j += 4)
      acc0 = vmlaq_f32(acc0, vld1q_f32(&a[i * n + j]), vld1q_f32(&x[j]));
    float result = vaddvq_f32(vaddq_f32(acc0, acc1));
    for (; j < n; j++)
      result += a[i * n + j] * x[j];
    y[i] = beta * y[i] + result;
  }
}

static void simd_neon_gemv_t(float *restrict y,
                                    const float *restrict a,
                                    const float *restrict x, size_t m,
                                    size_t n, float beta) {
  for (size_t j = 0; j < n; j++)
    y[j] *= beta;
  for (size_t i = 0; i < m; i++) {
    float32x4_t vxi = vdupq_n_f32(x[i]);
    const float *row = &a[i * n];
    size_t j = 0;
    for (; j + 4 <= n; j += 4) {
      float32x4_t vy = vld1q_f32(&y[j]);
      float32x4_t va = vld1q_f32(&row[j]);
      vst1q_f32(&y[j], vmlaq_f32(vy, vxi, va));
    }
    for (; j < n; j++)
      y[j] += x[i] * row[j];
  }
}

/* ── BLAS Level 3: gemm (cache-tiled) ─────────────────────────── */
static void simd_neon_gemm(float *restrict c,
                                  const float *restrict a,
                                  const float *restrict b, size_t m,
                                  size_t n, size_t k, float alpha, float beta) {
  const size_t T = 32;
  for (size_t i = 0; i < m; i++)
    for (size_t j = 0; j + 4 <= n; j += 4) {
      float32x4_t vc = vld1q_f32(&c[i * n + j]);
      vc = vmulq_f32(vc, vdupq_n_f32(beta));
      vst1q_f32(&c[i * n + j], vc);
    }
  for (size_t j = n - (n % 4); j < n; j++)
    for (size_t i = 0; i < m; i++)
      c[i * n + j] *= beta;

  for (size_t i0 = 0; i0 < m; i0 += T) {
    size_t imax = i0 + T < m ? i0 + T : m;
    for (size_t j0 = 0; j0 < n; j0 += T) {
      size_t jmax = j0 + T < n ? j0 + T : n;
      for (size_t k0 = 0; k0 < k; k0 += T) {
        size_t kmax = k0 + T < k ? k0 + T : k;
        for (size_t i = i0; i < imax; i++) {
          for (size_t kk = k0; kk < kmax; kk++) {
            float32x4_t vaik = vdupq_n_f32(alpha * a[i * k + kk]);
            size_t j = j0;
            for (; j + 4 <= jmax; j += 4) {
              float32x4_t vb = vld1q_f32(&b[kk * n + j]);
              float32x4_t vc = vld1q_f32(&c[i * n + j]);
              vst1q_f32(&c[i * n + j], vmlaq_f32(vc, vaik, vb));
            }
            for (; j < jmax; j++)
              c[i * n + j] += alpha * a[i * k + kk] * b[kk * n + j];
          }
        }
      }
    }
  }
}

/* ── Comparison / Selection ───────────────────────────────────── */
static void simd_neon_threshold(float *restrict out,
                                       const float *restrict in, float t,
                                       size_t n) {
  float32x4_t vt = vdupq_n_f32(t);
  float32x4_t one = vdupq_n_f32(1.0f);
  float32x4_t zero = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    uint32x4_t mask = vcgtq_f32(vi, vt);
    vst1q_f32(&out[i], vbslq_f32(mask, one, zero));
  }
  for (; i < n; i++)
    out[i] = in[i] > t ? 1.0f : 0.0f;
}

static void simd_neon_threshold_sign(float *restrict out,
                                            const float *restrict in,
                                            float t, size_t n) {
  float32x4_t vt = vdupq_n_f32(t);
  float32x4_t pos = vdupq_n_f32(1.0f);
  float32x4_t neg = vdupq_n_f32(-1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    uint32x4_t mask = vcgeq_f32(vi, vt);
    vst1q_f32(&out[i], vbslq_f32(mask, pos, neg));
  }
  for (; i < n; i++)
    out[i] = in[i] >= t ? 1.0f : -1.0f;
}

/* ── Hamming ──────────────────────────────────────────────────── */
static float simd_neon_hamming(const uint32_t *restrict a,
                                      const uint32_t *restrict b,
                                      size_t n_words) {
  uint32x4_t acc = vdupq_n_u32(0);
  size_t i = 0;
  for (; i + 4 <= n_words; i += 4) {
    uint32x4_t va = vld1q_u32(&a[i]);
    uint32x4_t vb = vld1q_u32(&b[i]);
    uint32x4_t xored = veorq_u32(va, vb);
    /* NEON has no popcount; use scalar for now.
     * On ARMv8.2-A with SHA3 extensions, there's cnt instruction per 8-bit
     * element */
    for (int j = 0; j < 4; j++)
      acc[j] += (uint32_t)__builtin_popcount(xored[j]);
  }
  uint32_t result = vaddvq_u32(acc);
  for (; i < n_words; i++)
    result += (uint32_t)__builtin_popcount(a[i] ^ b[i]);
  return (float)result;
}

/* ── Top-K (scalar heap) ──────────────────────────────────────── */
static void simd_neon_topk_indices(const float *restrict vals,
                                          uint32_t *restrict indices,
                                          size_t n, size_t k) {
  simd_scalar_topk_indices(vals, indices, n, k);
}

/* ── Clamp ──────────────────────────────────────────────────────── */
static void simd_neon_clamp(float *restrict out,
                                   const float *restrict in, float lo,
                                   float hi, size_t n) {
  float32x4_t vlo = vdupq_n_f32(lo);
  float32x4_t vhi = vdupq_n_f32(hi);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    float32x4_t vi = vld1q_f32(&in[i]);
    float32x4_t clamped = vminq_f32(vmaxq_f32(vi, vlo), vhi);
    vst1q_f32(&out[i], clamped);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < lo ? lo : (v > hi ? hi : v);
  }
}

/* ── forward value search (NEON: compare a lane-block, then locate the
 *    first hit scalar within the block; scalar tail). find_u8 stays memchr
 *    (already SIMD in libc), set by the scalar override. ─────────────── */
static size_t simd_neon_find_u16(const uint16_t *restrict p, uint16_t v,
                                 size_t n) {
  size_t i = 0;
  uint16x8_t vv = vdupq_n_u16(v);
  for (; i + 8 <= n; i += 8)
    if (vmaxvq_u16(vceqq_u16(vld1q_u16(p + i), vv)))
      for (size_t j = i; j < i + 8; j++)
        if (p[j] == v) return j;
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}
static size_t simd_neon_find_u32(const uint32_t *restrict p, uint32_t v,
                                 size_t n) {
  size_t i = 0;
  uint32x4_t vv = vdupq_n_u32(v);
  for (; i + 4 <= n; i += 4)
    if (vmaxvq_u32(vceqq_u32(vld1q_u32(p + i), vv)))
      for (size_t j = i; j < i + 4; j++)
        if (p[j] == v) return j;
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}
static size_t simd_neon_find_f32(const float *restrict p, float v, size_t n) {
  size_t i = 0;
  float32x4_t vv = vdupq_n_f32(v);
  for (; i + 4 <= n; i += 4)
    if (vmaxvq_u32(vceqq_f32(vld1q_f32(p + i), vv)))
      for (size_t j = i; j < i + 4; j++)
        if (p[j] == v) return j;
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}
static size_t simd_neon_find_f64(const double *restrict p, double v, size_t n) {
  size_t i = 0;
  float64x2_t vv = vdupq_n_f64(v);
  for (; i + 2 <= n; i += 2)
    if (vmaxvq_u32(vreinterpretq_u32_u64(vceqq_f64(vld1q_f64(p + i), vv)))) {
      if (p[i] == v) return i;
      if (p[i + 1] == v) return i + 1;
    }
  for (; i < n; i++)
    if (p[i] == v) return i;
  return SIZE_MAX;
}

/* NEON lacks a movemask; narrow each 0x00/0xFF lane to a 4-bit nibble via
 * vshrn, giving a 64-bit value with 4 bits per input byte. */
static inline uint64_t simd_neon_nibble_mask(uint8x16_t v) {
  uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(v), 4);
  return vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
}

/* Substring search, first+last algorithm over 16-byte blocks. */
static size_t simd_neon_strfind(const uint8_t *text, size_t n,
                                const uint8_t *pat, size_t m) {
  if (m == 0) return 0;
  if (m > n) return SIZE_MAX;
  if (m == 1) {
    const void *r = memchr(text, pat[0], n);
    return r ? (size_t)((const uint8_t *)r - text) : SIZE_MAX;
  }
  uint8x16_t vfirst = vdupq_n_u8(pat[0]);
  uint8x16_t vlast = vdupq_n_u8(pat[m - 1]);
  size_t i = 0;
  /* need text[i+m-1 .. i+m-1+15] in range: i + (m-1) + 16 <= n */
  while (i + 16 + (m - 1) <= n) {
    uint8x16_t bf = vld1q_u8(text + i);
    uint8x16_t bl = vld1q_u8(text + i + m - 1);
    uint8x16_t eq = vandq_u8(vceqq_u8(bf, vfirst), vceqq_u8(bl, vlast));
    uint64_t mask = simd_neon_nibble_mask(eq);
    while (mask) {
      int j = __builtin_ctzll(mask) >> 2; /* 4 bits per lane */
      if (m == 2 || memcmp(text + i + j + 1, pat + 1, m - 2) == 0)
        return i + j;
      mask &= ~((uint64_t)0xFULL << (j * 4));
    }
    i += 16;
  }
  size_t limit = n - m;
  while (i <= limit) {
    if (text[i] == pat[0] && memcmp(text + i + 1, pat + 1, m - 1) == 0)
      return i;
    i++;
  }
  return SIZE_MAX;
}

/* Count occurrences of byte `v`: sum per-block match lanes (0/1) with vaddvq. */
static size_t simd_neon_count_u8(const uint8_t *restrict p, uint8_t v,
                                 size_t n) {
  uint8x16_t vv = vdupq_n_u8(v);
  uint8x16_t one = vdupq_n_u8(1);
  size_t i = 0, total = 0;
  for (; i + 16 <= n; i += 16) {
    uint8x16_t cmp = vceqq_u8(vld1q_u8(p + i), vv); /* 0xFF per match */
    total += vaddvq_u8(vandq_u8(cmp, one));         /* <=16, fits u8 */
  }
  for (; i < n; i++)
    if (p[i] == v) total++;
  return total;
}

/* First index whose byte is in `set`: OR of per-byte compares for setlen<=8;
 * a 256-entry membership table drives the tail and larger sets. */
static size_t simd_neon_find_first_of(const uint8_t *restrict p, size_t n,
                                      const uint8_t *restrict set,
                                      size_t setlen) {
  uint8_t tbl[256];
  size_t i = 0, k;
  if (setlen == 0) return SIZE_MAX;
  memset(tbl, 0, sizeof(tbl));
  for (k = 0; k < setlen; k++) tbl[set[k]] = 1;
  if (setlen <= 8) {
    uint8x16_t vset[8];
    for (k = 0; k < setlen; k++) vset[k] = vdupq_n_u8(set[k]);
    for (; i + 16 <= n; i += 16) {
      uint8x16_t blk = vld1q_u8(p + i);
      uint8x16_t m = vceqq_u8(blk, vset[0]);
      for (k = 1; k < setlen; k++)
        m = vorrq_u8(m, vceqq_u8(blk, vset[k]));
      uint64_t mask = simd_neon_nibble_mask(m);
      if (mask) return i + (__builtin_ctzll(mask) >> 2);
    }
  }
  for (; i < n; i++)
    if (tbl[p[i]]) return i;
  return SIZE_MAX;
}

/* Validate UTF-8: SIMD-skip pure-ASCII 16-byte blocks, scalar-validate each
 * multibyte sequence exactly as the scalar oracle does. */
static size_t simd_neon_validate_utf8(const uint8_t *restrict p, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (i + 16 <= n) {
      uint8x16_t blk = vld1q_u8(p + i);
      if (vmaxvq_u8(vandq_u8(blk, vdupq_n_u8(0x80))) == 0) { i += 16; continue; }
    }
    uint8_t c = p[i];
    size_t len, j;
    uint32_t cp;
    if (c < 0x80) { i++; continue; }
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return i;
    if (i + len > n) return i;
    for (j = 1; j < len; j++) {
      uint8_t cc = p[i + j];
      if ((cc & 0xC0) != 0x80) return i;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return i;
    i += len;
  }
  return n;
}

/* ── Override table ─────────────────────────────────────────────── */
void simd_override_neon(simd_t *t) {
  t->find_u16 = simd_neon_find_u16;
  t->find_u32 = simd_neon_find_u32;
  t->find_f32 = simd_neon_find_f32;
  t->find_f64 = simd_neon_find_f64;
  t->strfind = simd_neon_strfind;
  t->count_u8 = simd_neon_count_u8;
  t->find_first_of = simd_neon_find_first_of;
  t->validate_utf8 = simd_neon_validate_utf8;
  t->dot = simd_neon_dot;
  t->dot_f = simd_neon_dot_f;
  t->norm_l2_sq = simd_neon_norm_l2_sq;
  t->norm_l2 = simd_neon_norm_l2;
  t->norm_l1 = simd_neon_norm_l1;
  t->sum = simd_neon_sum;
  t->max = simd_neon_max;
  t->min = simd_neon_min;
  t->argmax = simd_neon_argmax;
  t->argmin = simd_neon_argmin;
  t->argminmax = simd_neon_argminmax;
  t->add = simd_neon_add;
  t->sub = simd_neon_sub;
  t->mul = simd_neon_mul;
  t->div = simd_neon_div;
  t->abs = simd_neon_abs;
  t->fma = simd_neon_fma;
  t->add_s = simd_neon_add_s;
  t->mul_s = simd_neon_mul_s;
  t->scale_add_s = simd_neon_scale_add_s;
  t->sigmoid = simd_neon_sigmoid;
  t->relu = simd_neon_relu;
  t->relu6 = simd_neon_relu6;
  t->leaky_relu = simd_neon_leaky_relu;
  t->elu = simd_neon_elu;
  t->tanh_fast = simd_neon_tanh_fast;
  t->gelu = simd_neon_gelu;
  t->silu = simd_neon_silu;
  t->softmax = simd_neon_softmax;
  t->log_softmax = simd_neon_log_softmax;
  t->vexp = simd_neon_vexp;
  t->vlog = simd_neon_vlog;
  t->vsqrt = simd_neon_vsqrt;
  t->vrsqrt = simd_neon_vrsqrt;
  t->vinv = simd_neon_vinv;
  t->dist_l2_sq = simd_neon_dist_l2_sq;
  t->dist_l2 = simd_neon_dist_l2;
  t->dist_l1 = simd_neon_dist_l1;
  t->dist_cos = simd_neon_dist_cos;
  t->dist_cheb = simd_neon_dist_cheb;
  t->dist_matrix_l2_sq = simd_neon_dist_matrix_l2_sq;
  t->dist_matrix_cos = simd_neon_dist_matrix_cos;
  t->dist_matrix_l1 = simd_neon_dist_matrix_l1;
  t->gemv = simd_neon_gemv;
  t->gemv_t = simd_neon_gemv_t;
  t->gemm = simd_neon_gemm;
  t->threshold = simd_neon_threshold;
  t->threshold_sign = simd_neon_threshold_sign;
  t->hamming = simd_neon_hamming;
  t->topk_indices = simd_neon_topk_indices;
  t->clamp = simd_neon_clamp;
}

#else  /* !ARM64 */
void simd_override_neon(simd_t *t) { (void)t; }
#endif /* ARM64 */