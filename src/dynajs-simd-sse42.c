#include "dynajs-simd-kernels.h"
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

/* SSE4.2 SIMD overrides — 128-bit XMM registers, 4 floats per vector.
 * Compiled only on x86-64 with -msse4.2 via target attribute.
 * Covers the most common BLAS-1 kernels as a bridge between scalar and AVX2. */

/* Whole-file guard: pairs with the #else/#endif stub at the bottom. A
 * stray early #endif here used to orphan that #else, so this file never
 * compiled on any host. */
#if defined(__x86_64__) || defined(_M_X64)
#include <smmintrin.h>
#include <math.h>
#include <string.h>
#include "dynajs-simd-kernels.h"
#include <math.h>

/* Horizontal sum of 4 lanes. */
__attribute__((target("sse4.2"))) static inline float
simd_hsum_128(__m128 v) {
  __m128 t = _mm_add_ps(v, _mm_movehl_ps(v, v));
  t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
  return _mm_cvtss_f32(t);
}

/* ── Dot product (float accumulator) ─────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_dot(const float *restrict a, const float *restrict b,
                      size_t n) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    acc = _mm_add_ps(acc, _mm_mul_ps(va, vb));
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += a[i] * b[i];
  return result;
}

/* ── Dot product (double accumulator for stability) ──────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_dot_f(const float *restrict a,
                        const float *restrict b, size_t n) {
  __m128d acc0 = _mm_setzero_pd();
  __m128d acc1 = _mm_setzero_pd();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 lo = _mm_mul_ps(va, vb);
    acc0 = _mm_add_pd(acc0, _mm_cvtps_pd(lo));
    __m128 hi = _mm_movehl_ps(lo, lo);
    acc1 = _mm_add_pd(acc1, _mm_cvtps_pd(hi));
  }
  double result = acc0[0] + acc0[1] + acc1[0] + acc1[1];
  for (; i < n; i++)
    result += (double)a[i] * b[i];
  return (float)result;
}

/* ── Norm L2 squared ────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_norm_l2_sq(const float *restrict x, size_t n) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vx = _mm_loadu_ps(&x[i]);
    acc = _mm_add_ps(acc, _mm_mul_ps(vx, vx));
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += x[i] * x[i];
  return result;
}

__attribute__((target("sse4.2"))) static float
simd_sse42_norm_l2(const float *restrict x, size_t n) {
  return sqrtf(simd_sse42_norm_l2_sq(x, n));
}

/* ── Norm L1 ─────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_norm_l1(const float *restrict x, size_t n) {
  __m128 acc = _mm_setzero_ps();
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vx = _mm_loadu_ps(&x[i]);
    acc = _mm_add_ps(acc, _mm_andnot_ps(sign_mask, vx));
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += fabsf(x[i]);
  return result;
}

/* ── Sum ─────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_sum(const float *restrict x, size_t n) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vx = _mm_loadu_ps(&x[i]);
    acc = _mm_add_ps(acc, vx);
  }
  float result = simd_hsum_128(acc);
  for (; i < n; i++)
    result += x[i];
  return result;
}

/* ── Max ─────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_max(const float *restrict x, size_t n) {
  if (n == 0)
    return -FLT_MAX;
  __m128 vmax = _mm_loadu_ps(x);
  size_t i = 4;
  for (; i + 4 <= n; i += 4)
    vmax = _mm_max_ps(vmax, _mm_loadu_ps(&x[i]));
  float result = simd_hsum_128(
      _mm_max_ps(vmax, _mm_shuffle_ps(vmax, vmax, _MM_SHUFFLE(2, 3, 0, 1))));
  result = fmaxf(result, _mm_cvtss_f32(_mm_movehl_ps(vmax, vmax)));
  for (; i < n; i++)
    if (x[i] > result)
      result = x[i];
  return result;
}

/* ── Min ─────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_min(const float *restrict x, size_t n) {
  if (n == 0)
    return FLT_MAX;
  __m128 vmin = _mm_loadu_ps(x);
  size_t i = 4;
  for (; i + 4 <= n; i += 4)
    vmin = _mm_min_ps(vmin, _mm_loadu_ps(&x[i]));
  float result = simd_hsum_128(
      _mm_min_ps(vmin, _mm_shuffle_ps(vmin, vmin, _MM_SHUFFLE(2, 3, 0, 1))));
  result = fminf(result, _mm_cvtss_f32(_mm_movehl_ps(vmin, vmin)));
  for (; i < n; i++)
    if (x[i] < result)
      result = x[i];
  return result;
}

/* ── Argmax ──────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_argmax(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  __m128 vmax = _mm_loadu_ps(x);
  __m128i vidx = _mm_setr_epi32(0, 1, 2, 3);
  __m128i vidx_max = vidx;
  size_t i = 4;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&x[i]);
    __m128i vidxi = _mm_set1_epi32((int)i);
    __m128i step = _mm_setr_epi32(0, 1, 2, 3);
    vidxi = _mm_add_epi32(vidxi, step);
    __m128 mask = _mm_cmpgt_ps(vi, vmax);
    vmax = _mm_max_ps(vmax, vi);
    vidx_max = _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(vidx_max),
                                              _mm_castsi128_ps(vidxi), mask));
  }
  float tmp[4];
  uint32_t idx_tmp[4];
  _mm_storeu_ps(tmp, vmax);
  _mm_storeu_si128((__m128i *)idx_tmp, vidx_max);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 4; k++)
    if (tmp[k] > best) {
      best = tmp[k];
      best_idx = idx_tmp[k];
    }
  for (; i < n; i++)
    if (x[i] > best) {
      best = x[i];
      best_idx = i;
    }
  return best_idx;
}

/* ── Argmin ──────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_argmin(const float *restrict x, size_t n) {
  if (n == 0)
    return 0;
  __m128 vmin = _mm_loadu_ps(x);
  __m128i vidx = _mm_setr_epi32(0, 1, 2, 3);
  __m128i vidx_min = vidx;
  size_t i = 4;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&x[i]);
    __m128i vidxi = _mm_set1_epi32((int)i);
    __m128i step = _mm_setr_epi32(0, 1, 2, 3);
    vidxi = _mm_add_epi32(vidxi, step);
    __m128 mask = _mm_cmplt_ps(vi, vmin);
    vmin = _mm_min_ps(vmin, vi);
    vidx_min = _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(vidx_min),
                                              _mm_castsi128_ps(vidxi), mask));
  }
  float tmp[4];
  uint32_t idx_tmp[4];
  _mm_storeu_ps(tmp, vmin);
  _mm_storeu_si128((__m128i *)idx_tmp, vidx_min);
  float best = tmp[0];
  size_t best_idx = idx_tmp[0];
  for (size_t k = 1; k < 4; k++)
    if (tmp[k] < best) {
      best = tmp[k];
      best_idx = idx_tmp[k];
    }
  for (; i < n; i++)
    if (x[i] < best) {
      best = x[i];
      best_idx = i;
    }
  return best_idx;
}

/* ── Argminmax ───────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_argminmax(const float *restrict x, size_t n,
                            size_t *argmin_out, size_t *argmax_out) {
  if (n == 0) {
    *argmin_out = *argmax_out = 0;
    return;
  }
  __m128 vmin = _mm_loadu_ps(x);
  __m128 vmax = vmin;
  __m128i vidx = _mm_setr_epi32(0, 1, 2, 3);
  __m128i vidx_min = vidx, vidx_max = vidx;
  size_t i = 4;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&x[i]);
    __m128i vidxi = _mm_set1_epi32((int)i);
    __m128i step = _mm_setr_epi32(0, 1, 2, 3);
    vidxi = _mm_add_epi32(vidxi, step);
    __m128 mask_lt = _mm_cmplt_ps(vi, vmin);
    __m128 mask_gt = _mm_cmpgt_ps(vi, vmax);
    vmin = _mm_min_ps(vmin, vi);
    vmax = _mm_max_ps(vmax, vi);
    vidx_min = _mm_castps_si128(_mm_blendv_ps(
        _mm_castsi128_ps(vidx_min), _mm_castsi128_ps(vidxi), mask_lt));
    vidx_max = _mm_castps_si128(_mm_blendv_ps(
        _mm_castsi128_ps(vidx_max), _mm_castsi128_ps(vidxi), mask_gt));
  }
  float tmp_min[4], tmp_max[4];
  uint32_t idx_min[4], idx_max[4];
  _mm_storeu_ps(tmp_min, vmin);
  _mm_storeu_ps(tmp_max, vmax);
  _mm_storeu_si128((__m128i *)idx_min, vidx_min);
  _mm_storeu_si128((__m128i *)idx_max, vidx_max);
  float best_min = tmp_min[0], best_max = tmp_max[0];
  size_t imin = idx_min[0], imax = idx_max[0];
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
__attribute__((target("sse4.2"))) static void
simd_sse42_add(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    _mm_storeu_ps(&z[i], _mm_add_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] + b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_sub(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    _mm_storeu_ps(&z[i], _mm_sub_ps(va, vb));
  }
  for (; i < n; i++)
    z[i] = a[i] - b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_mul(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_mul_ps(_mm_loadu_ps(&a[i]), _mm_loadu_ps(&b[i])));
  }
  for (; i < n; i++)
    z[i] = a[i] * b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_div(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_div_ps(_mm_loadu_ps(&a[i]), _mm_loadu_ps(&b[i])));
  }
  for (; i < n; i++)
    z[i] = a[i] / b[i];
}

__attribute__((target("sse4.2"))) static void
simd_sse42_abs(float *restrict out, const float *restrict in,
                      size_t n) {
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_andnot_ps(sign_mask, vi));
  }
  for (; i < n; i++)
    out[i] = fabsf(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_fma(float *restrict z, const float *restrict a,
                      const float *restrict b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vz = _mm_loadu_ps(&z[i]);
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    _mm_storeu_ps(&z[i], _mm_add_ps(_mm_mul_ps(va, vb), vz));
  }
  for (; i < n; i++)
    z[i] += a[i] * b[i];
}

/* ── Element-wise: scalar–vector ──────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_add_s(float *restrict z, const float *restrict x,
                        float s, size_t n) {
  __m128 vs = _mm_set1_ps(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_add_ps(_mm_loadu_ps(&x[i]), vs));
  }
  for (; i < n; i++)
    z[i] = x[i] + s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_mul_s(float *restrict z, const float *restrict x,
                        float s, size_t n) {
  __m128 vs = _mm_set1_ps(s);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_mul_ps(_mm_loadu_ps(&x[i]), vs));
  }
  for (; i < n; i++)
    z[i] = x[i] * s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_scale_add_s(float *restrict z, float alpha,
                              const float *restrict x, float beta,
                              size_t n) {
  __m128 va = _mm_set1_ps(alpha);
  __m128 vb = _mm_set1_ps(beta);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    _mm_storeu_ps(&z[i], _mm_add_ps(_mm_mul_ps(va, _mm_loadu_ps(&x[i])), vb));
  }
  for (; i < n; i++)
    z[i] = alpha * x[i] + beta;
}

/* ── Activations ──────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_sigmoid(float *restrict out, const float *restrict in,
                          size_t n) {
  __m128 one = _mm_set1_ps(1.0f);
  __m128 lo = _mm_set1_ps(-30.0f);
  __m128 hi = _mm_set1_ps(30.0f);
  __m128 magic = _mm_set1_ps(-12102203.0f);
  __m128 bias = _mm_set1_ps(1.0f);
  __m128i zero = _mm_setzero_si128();
  __m128i top = _mm_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    vi = _mm_max_ps(_mm_min_ps(vi, hi), lo);
    __m128i bits = _mm_cvtps_epi32(_mm_mul_ps(vi, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(bias));
    bits = _mm_max_epi32(bits, zero);
    bits = _mm_min_epi32(bits, top);
    __m128 ve = _mm_add_ps(one, _mm_castsi128_ps(bits));
    _mm_storeu_ps(&out[i], _mm_div_ps(one, ve));
  }
  for (; i < n; i++)
    out[i] = fast_sigmoid(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_tanh(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 two = _mm_set1_ps(2.0f);
  __m128 one = _mm_set1_ps(1.0f);
  __m128 lo = _mm_set1_ps(-10.0f);
  __m128 hi = _mm_set1_ps(10.0f);
  __m128 magic = _mm_set1_ps(-12102203.0f * 2.0f);
  __m128 bias = _mm_set1_ps(1.0f);
  __m128i zero = _mm_setzero_si128();
  __m128i top = _mm_set1_epi32(0x7F800000);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    vi = _mm_max_ps(_mm_min_ps(vi, hi), lo);
    __m128i bits = _mm_cvtps_epi32(_mm_mul_ps(vi, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(bias));
    bits = _mm_max_epi32(bits, zero);
    bits = _mm_min_epi32(bits, top);
    __m128 ve = _mm_sub_ps(
        _mm_div_ps(two, _mm_add_ps(one, _mm_castsi128_ps(bits))), one);
    _mm_storeu_ps(&out[i], ve);
  }
  for (; i < n; i++)
    out[i] = fast_tanh(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_gelu(float *restrict out, const float *restrict in,
                       size_t n) {
  const __m128 sqrt_2_over_pi = _mm_set1_ps(0.7978845608028654f);
  const __m128 c = _mm_set1_ps(0.044715f);
  const __m128 half = _mm_set1_ps(0.5f);
  const __m128 one = _mm_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 x = _mm_loadu_ps(&in[i]);
    __m128 x3 = _mm_mul_ps(_mm_mul_ps(x, x), x);
    __m128 inner = _mm_mul_ps(sqrt_2_over_pi, _mm_add_ps(x, _mm_mul_ps(c, x3)));
    /* SSE4.2 has no tanh instruction (_mm_tan_ps is SVML-only, and is
     * tan, not tanh): tanh(y) = 2*sigmoid(2y) - 1 via the bit-exp trick. */
    {
      __m128 y = inner;
      __m128 y2 = _mm_add_ps(y, y);
      __m128 y2c =
          _mm_max_ps(_mm_min_ps(y2, _mm_set1_ps(10.0f)), _mm_set1_ps(-10.0f));
      __m128i bits2 =
          _mm_cvtps_epi32(_mm_mul_ps(y2c, _mm_set1_ps(-12102203.0f)));
      bits2 = _mm_add_epi32(bits2, _mm_castps_si128(_mm_set1_ps(1.0f)));
      bits2 = _mm_max_epi32(bits2, _mm_setzero_si128());
      bits2 = _mm_min_epi32(bits2, _mm_set1_epi32(0x7F800000));
      __m128 ve2 = _mm_add_ps(one, _mm_castsi128_ps(bits2));
      __m128 sig2 = _mm_div_ps(one, ve2);
      inner = _mm_sub_ps(_mm_add_ps(sig2, sig2), one);
    }
    _mm_storeu_ps(&out[i],
                  _mm_mul_ps(_mm_mul_ps(half, x), _mm_add_ps(one, inner)));
  }
  for (; i < n; i++) {
    float x = in[i];
    float x3 = x * x * x;
    float inner = tanhf(0.7978845608028654f * (x + 0.044715f * x3));
    out[i] = 0.5f * x * (1.0f + inner);
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_relu(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 zero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_max_ps(vi, zero));
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_relu6(float *restrict out, const float *restrict in,
                        size_t n) {
  __m128 zero = _mm_setzero_ps();
  __m128 six = _mm_set1_ps(6.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_min_ps(_mm_max_ps(vi, zero), six));
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_leaky_relu(float *restrict out,
                             const float *restrict in, float slope,
                             size_t n) {
  __m128 vzero = _mm_setzero_ps();
  __m128 vslope = _mm_set1_ps(slope);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpgt_ps(vi, vzero);
    __m128 res = _mm_add_ps(_mm_and_ps(mask, vi),
                            _mm_andnot_ps(mask, _mm_mul_ps(vi, vslope)));
    _mm_storeu_ps(&out[i], res);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : v * slope;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_elu(float *restrict out, const float *restrict in,
                      float alpha, size_t n) {
  __m128 vzero = _mm_setzero_ps();
  __m128 valpha = _mm_set1_ps(alpha);
  __m128 vone = _mm_set1_ps(1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpgt_ps(vi, vzero);
    /* _mm_exp_ps is SVML-only; per-lane libm exp matches the tail. */
    __m128 vexp;
    {
      float tmp[4];
      _mm_storeu_ps(tmp, vi);
      for (int j = 0; j < 4; j++)
        tmp[j] = expf(tmp[j]);
      vexp = _mm_loadu_ps(tmp);
    }
    __m128 exp_part = _mm_sub_ps(vexp, vone);
    __m128 res = _mm_add_ps(_mm_and_ps(mask, vi),
                            _mm_andnot_ps(mask, _mm_mul_ps(valpha, exp_part)));
    _mm_storeu_ps(&out[i], res);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
  }
}

/* Vectorized Schraudolph exp, matching fast_exp bit-for-bit:
 * bits = (int)(12102203·x + 0x3F800000), clamped to [0, +inf bits]. */
__attribute__((target("sse4.2"))) static inline __m128
sse42_fast_exp_ps(__m128 x) {
  __m128i bits = _mm_cvtps_epi32(
      _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(12102203.0f)),
                 _mm_set1_ps(1065353216.0f)));
  bits = _mm_max_epi32(bits, _mm_setzero_si128());
  bits = _mm_min_epi32(bits, _mm_set1_epi32(0x7F800000));
  return _mm_castsi128_ps(bits);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_softmax(float *restrict out, const float *restrict in,
                          size_t n) {
  if (unlikely(n == 0))
    return;
  /* Max reduction. The 4-wide head load is only safe when n >= 4 —
   * softmax is routinely called with tiny n (class counts). */
  float maxv = in[0];
  size_t i = 1;
  if (n >= 4) {
    __m128 vmax = _mm_loadu_ps(in);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm_max_ps(vmax, _mm_loadu_ps(&in[i]));
    __m128 shuf = _mm_max_ps(vmax, _mm_movehl_ps(vmax, vmax));
    shuf = _mm_max_ss(shuf, _mm_shuffle_ps(shuf, shuf, 1));
    maxv = _mm_cvtss_f32(shuf);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  __m128 vmaxv = _mm_set1_ps(maxv);
  i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i],
                  sse42_fast_exp_ps(_mm_sub_ps(_mm_loadu_ps(&in[i]), vmaxv)));
  for (; i < n; i++)
    out[i] = fast_exp(in[i] - maxv);

  float sum = simd_hsum_f32(out, n);
  float inv_sum = 1.0f / sum;
  __m128 vinv = _mm_set1_ps(inv_sum);
  for (i = 0; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i], _mm_mul_ps(_mm_loadu_ps(&out[i]), vinv));
  for (; i < n; i++)
    out[i] *= inv_sum;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_log_softmax(float *restrict out,
                              const float *restrict in, size_t n) {
  if (unlikely(n == 0))
    return;
  float maxv = in[0];
  size_t i = 1;
  if (n >= 4) {
    __m128 vmax = _mm_loadu_ps(in);
    for (i = 4; i + 4 <= n; i += 4)
      vmax = _mm_max_ps(vmax, _mm_loadu_ps(&in[i]));
    __m128 shuf = _mm_max_ps(vmax, _mm_movehl_ps(vmax, vmax));
    shuf = _mm_max_ss(shuf, _mm_shuffle_ps(shuf, shuf, 1));
    maxv = _mm_cvtss_f32(shuf);
  }
  for (; i < n; i++)
    if (in[i] > maxv)
      maxv = in[i];

  /* exps land in out[] so simd_hsum_f32 can sum every element —
   * the old code only summed the scalar tail. */
  __m128 vmaxv = _mm_set1_ps(maxv);
  i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i],
                  sse42_fast_exp_ps(_mm_sub_ps(_mm_loadu_ps(&in[i]), vmaxv)));
  for (; i < n; i++)
    out[i] = fast_exp(in[i] - maxv);
  float sum = simd_hsum_f32(out, n);

  float log_s = logf(sum);
  __m128 voff = _mm_set1_ps(maxv + log_s);
  for (i = 0; i + 4 <= n; i += 4)
    _mm_storeu_ps(&out[i], _mm_sub_ps(_mm_loadu_ps(&in[i]), voff));
  for (; i < n; i++)
    out[i] = in[i] - maxv - log_s;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vexp(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 magic = _mm_set1_ps(12102203.0f);
  __m128 bias = _mm_set1_ps(1.0f);
  __m128 vzero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128i bits = _mm_cvtps_epi32(_mm_mul_ps(vi, magic));
    bits = _mm_add_epi32(bits, _mm_castps_si128(bias));
    bits = _mm_max_epi32(bits, _mm_setzero_si128());
    bits = _mm_min_epi32(bits, _mm_set1_epi32(0x7F800000));
    __m128 ve = _mm_castsi128_ps(bits);
    _mm_storeu_ps(&out[i], _mm_max_ps(ve, vzero));
  }
  for (; i < n; i++)
    out[i] = fast_exp(in[i]);
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vlog(float *restrict out, const float *restrict in,
                       size_t n) {
  __m128 vzero = _mm_setzero_ps();
  size_t i = 0;
  (void)vzero;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    /* _mm_log_ps is SVML-only; compute per lane with libm logf. */
    __m128 vln = _mm_set1_ps(-FLT_MAX);
    /* Scalar fallback embedded: only compute log for positive lanes */
    for (size_t j = 0; j < 4; j++) {
      float vf = ((float *)&vi)[j];
      ((float *)&vln)[j] = vf > 0.0f ? logf(vf) : -FLT_MAX;
    }
    _mm_storeu_ps(&out[i], vln);
  }
  for (; i < n; i++)
    out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vsqrt(float *restrict out, const float *restrict in,
                        size_t n) {
  __m128 vzero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpge_ps(vi, vzero);
    __m128 vs = _mm_sqrt_ps(vi);
    vs = _mm_and_ps(mask, vs);
    _mm_storeu_ps(&out[i], vs);
  }
  for (; i < n; i++)
    out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vrsqrt(float *restrict out, const float *restrict in,
                         size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 vs = _mm_rsqrt_ps(vi);
    _mm_storeu_ps(&out[i], vs);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_vinv(float *restrict out, const float *restrict in,
                       size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    _mm_storeu_ps(&out[i], _mm_rcp_ps(vi));
  }
  for (; i < n; i++)
    out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

/* ── Distance: vector–vector ─────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_dist_l2_sq(const float *restrict a,
                             const float *restrict b, size_t d) {
  __m128 acc = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 diff = _mm_sub_ps(va, vb);
    acc = _mm_add_ps(acc, _mm_mul_ps(diff, diff));
  }
  float result = simd_hsum_128(acc);
  for (; i < d; i++) {
    float df = a[i] - b[i];
    result += df * df;
  }
  return result;
}

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_l1(const float *restrict a,
                          const float *restrict b, size_t d) {
  __m128 acc = _mm_setzero_ps();
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 diff = _mm_sub_ps(va, vb);
    acc = _mm_add_ps(acc, _mm_andnot_ps(sign_mask, diff));
  }
  float result = simd_hsum_128(acc);
  for (; i < d; i++)
    result += fabsf(a[i] - b[i]);
  return result;
}

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_cos(const float *restrict a,
                           const float *restrict b, size_t d) {
  __m128 vdot = _mm_setzero_ps();
  __m128 vna = _mm_setzero_ps();
  __m128 vnb = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    vdot = _mm_add_ps(vdot, _mm_mul_ps(va, vb));
    vna = _mm_add_ps(vna, _mm_mul_ps(va, va));
    vnb = _mm_add_ps(vnb, _mm_mul_ps(vb, vb));
  }
  float dot = simd_hsum_128(vdot);
  float na = simd_hsum_128(vna);
  float nb = simd_hsum_128(vnb);
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

__attribute__((target("sse4.2"))) static float
simd_sse42_dist_cheb(const float *restrict a,
                            const float *restrict b, size_t d) {
  __m128 vmax = _mm_setzero_ps();
  __m128 sign_mask = _mm_set1_ps(-0.0f);
  size_t i = 0;
  for (; i + 4 <= d; i += 4) {
    __m128 va = _mm_loadu_ps(&a[i]);
    __m128 vb = _mm_loadu_ps(&b[i]);
    __m128 adiff = _mm_andnot_ps(sign_mask, _mm_sub_ps(va, vb));
    vmax = _mm_max_ps(vmax, adiff);
  }
  float result = simd_hsum_128(
      _mm_max_ps(vmax, _mm_shuffle_ps(vmax, vmax, _MM_SHUFFLE(2, 3, 0, 1))));
  result = fmaxf(result, _mm_cvtss_f32(_mm_movehl_ps(vmax, vmax)));
  for (; i < d; i++) {
    float df = fabsf(a[i] - b[i]);
    if (df > result)
      result = df;
  }
  return result;
}

/* ── Distance matrix ──────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_dist_matrix_l2_sq(float *restrict out,
                                    const float *restrict a,
                                    const float *restrict b, size_t n,
                                    size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_sse42_dist_l2_sq(&a[i * d], &b[j * d], d);
}

__attribute__((target("sse4.2"))) static void simd_sse42_dist_matrix_cos(
    float *restrict out, const float *restrict a,
    const float *restrict b, size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_sse42_dist_cos(&a[i * d], &b[j * d], d);
}

__attribute__((target("sse4.2"))) static void simd_sse42_dist_matrix_l1(
    float *restrict out, const float *restrict a,
    const float *restrict b, size_t n, size_t m, size_t d) {
  for (size_t i = 0; i < n; i++)
    for (size_t j = 0; j < m; j++)
      out[i * m + j] = simd_sse42_dist_l1(&a[i * d], &b[j * d], d);
}

/* ── BLAS Level 2: gemv ───────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_gemv(float *restrict y, const float *restrict a,
                       const float *restrict x, size_t m, size_t n,
                       float beta) {
  for (size_t i = 0; i < m; i++) {
    __m128 acc0 = _mm_setzero_ps();
    __m128 acc1 = _mm_setzero_ps();
    size_t j = 0;
    for (; j + 8 <= n; j += 8) {
      acc0 = _mm_add_ps(
          acc0, _mm_mul_ps(_mm_loadu_ps(&a[i * n + j]), _mm_loadu_ps(&x[j])));
      acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(&a[i * n + j + 4]),
                                         _mm_loadu_ps(&x[j + 4])));
    }
    for (; j + 4 <= n; j += 4)
      acc0 = _mm_add_ps(
          acc0, _mm_mul_ps(_mm_loadu_ps(&a[i * n + j]), _mm_loadu_ps(&x[j])));
    __m128 sum = _mm_add_ps(acc0, acc1);
    float result = simd_hsum_128(sum);
    for (; j < n; j++)
      result += a[i * n + j] * x[j];
    y[i] = beta * y[i] + result;
  }
}

__attribute__((target("sse4.2"))) static void
simd_sse42_gemv_t(float *restrict y, const float *restrict a,
                         const float *restrict x, size_t m, size_t n,
                         float beta) {
  for (size_t j = 0; j < n; j++)
    y[j] *= beta;
  for (size_t i = 0; i < m; i++) {
    __m128 vxi = _mm_set1_ps(x[i]);
    const float *row = &a[i * n];
    size_t j = 0;
    for (; j + 4 <= n; j += 4) {
      __m128 vy = _mm_loadu_ps(&y[j]);
      __m128 va = _mm_loadu_ps(&row[j]);
      _mm_storeu_ps(&y[j], _mm_add_ps(vy, _mm_mul_ps(vxi, va)));
    }
    for (; j < n; j++)
      y[j] += x[i] * row[j];
  }
}

/* ── BLAS Level 3: gemm (tiled) ───────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_gemm(float *restrict c, const float *restrict a,
                       const float *restrict b, size_t m, size_t n,
                       size_t k, float alpha, float beta) {
  const size_t T = 32;
  for (size_t i = 0; i < m; i++)
    for (size_t j = 0; j + 4 <= n; j += 4) {
      __m128 vc = _mm_loadu_ps(&c[i * n + j]);
      vc = _mm_mul_ps(vc, _mm_set1_ps(beta));
      _mm_storeu_ps(&c[i * n + j], vc);
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
            __m128 vaik = _mm_set1_ps(alpha * a[i * k + kk]);
            size_t j = j0;
            for (; j + 4 <= jmax; j += 4) {
              __m128 vb = _mm_loadu_ps(&b[kk * n + j]);
              __m128 vc = _mm_loadu_ps(&c[i * n + j]);
              _mm_storeu_ps(&c[i * n + j],
                            _mm_add_ps(_mm_mul_ps(vaik, vb), vc));
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
__attribute__((target("sse4.2"))) static void
simd_sse42_threshold(float *restrict out,
                            const float *restrict in, float t, size_t n) {
  __m128 vt = _mm_set1_ps(t);
  __m128 one = _mm_set1_ps(1.0f);
  __m128 zero = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpgt_ps(vi, vt);
    _mm_storeu_ps(&out[i], _mm_blendv_ps(zero, one, mask));
  }
  for (; i < n; i++)
    out[i] = in[i] > t ? 1.0f : 0.0f;
}

__attribute__((target("sse4.2"))) static void simd_sse42_threshold_sign(
    float *restrict out, const float *restrict in, float t, size_t n) {
  __m128 vt = _mm_set1_ps(t);
  __m128 pos = _mm_set1_ps(1.0f);
  __m128 neg = _mm_set1_ps(-1.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 mask = _mm_cmpge_ps(vi, vt);
    _mm_storeu_ps(&out[i], _mm_blendv_ps(neg, pos, mask));
  }
  for (; i < n; i++)
    out[i] = in[i] >= t ? 1.0f : -1.0f;
}

/* ── Hamming ──────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static float
simd_sse42_hamming(const uint32_t *restrict a,
                          const uint32_t *restrict b, size_t n_words) {
  /* There is no _mm_popcnt_epi32 (vector popcount needs AVX-512
   * VPOPCNTDQ); SSE4.2 provides scalar POPCNT, which the compiler emits
   * for __builtin_popcount under this target. */
  uint32_t result = 0;
  for (size_t i = 0; i < n_words; i++)
    result += (uint32_t)__builtin_popcount(a[i] ^ b[i]);
  return (float)result;
}

/* ── Top-K (uses scalar heap — the vectorized partition is in AVX2) ─ */
__attribute__((target("sse4.2"))) static void
simd_sse42_topk_indices(const float *restrict vals,
                               uint32_t *restrict indices, size_t n,
                               size_t k) {
  /* Fall through to scalar topk */
  simd_scalar_topk_indices(vals, indices, n, k);
}

/* ── Clamp ──────────────────────────────────────────────────────── */
__attribute__((target("sse4.2"))) static void
simd_sse42_clamp(float *restrict out, const float *restrict in,
                        float lo, float hi, size_t n) {
  __m128 vlo = _mm_set1_ps(lo);
  __m128 vhi = _mm_set1_ps(hi);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    __m128 vi = _mm_loadu_ps(&in[i]);
    __m128 clamped = _mm_min_ps(_mm_max_ps(vi, vlo), vhi);
    _mm_storeu_ps(&out[i], clamped);
  }
  for (; i < n; i++) {
    float v = in[i];
    out[i] = v < lo ? lo : (v > hi ? hi : v);
  }
}

/* Substring search, first+last algorithm over 16-byte blocks (SSE). */
__attribute__((target("sse4.2"))) static size_t
simd_sse42_strfind(const uint8_t *text, size_t n, const uint8_t *pat,
                   size_t m) {
  if (m == 0) return 0;
  if (m > n) return SIZE_MAX;
  if (m == 1) {
    const void *r = memchr(text, pat[0], n);
    return r ? (size_t)((const uint8_t *)r - text) : SIZE_MAX;
  }
  __m128i vfirst = _mm_set1_epi8((char)pat[0]);
  __m128i vlast = _mm_set1_epi8((char)pat[m - 1]);
  size_t i = 0;
  while (i + 16 + (m - 1) <= n) {
    __m128i bf = _mm_loadu_si128((const __m128i *)(text + i));
    __m128i bl = _mm_loadu_si128((const __m128i *)(text + i + m - 1));
    unsigned mask = (unsigned)_mm_movemask_epi8(
        _mm_and_si128(_mm_cmpeq_epi8(bf, vfirst), _mm_cmpeq_epi8(bl, vlast)));
    while (mask) {
      int j = __builtin_ctz(mask);
      if (m == 2 || memcmp(text + i + j + 1, pat + 1, m - 2) == 0)
        return i + j;
      mask &= mask - 1;
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

/* ── Override table ─────────────────────────────────────────────── */
void simd_override_sse42(simd_t *t) {
  t->strfind = simd_sse42_strfind;
  t->dot = simd_sse42_dot;
  t->dot_f = simd_sse42_dot_f;
  t->norm_l2_sq = simd_sse42_norm_l2_sq;
  t->norm_l2 = simd_sse42_norm_l2;
  t->norm_l1 = simd_sse42_norm_l1;
  t->sum = simd_sse42_sum;
  t->max = simd_sse42_max;
  t->min = simd_sse42_min;
  t->argmax = simd_sse42_argmax;
  t->argmin = simd_sse42_argmin;
  t->argminmax = simd_sse42_argminmax;
  t->add = simd_sse42_add;
  t->sub = simd_sse42_sub;
  t->mul = simd_sse42_mul;
  t->div = simd_sse42_div;
  t->abs = simd_sse42_abs;
  t->fma = simd_sse42_fma;
  t->add_s = simd_sse42_add_s;
  t->mul_s = simd_sse42_mul_s;
  t->scale_add_s = simd_sse42_scale_add_s;
  t->sigmoid = simd_sse42_sigmoid;
  t->relu = simd_sse42_relu;
  t->relu6 = simd_sse42_relu6;
  t->leaky_relu = simd_sse42_leaky_relu;
  t->elu = simd_sse42_elu;
  t->tanh_fast = simd_sse42_tanh;
  t->gelu = simd_sse42_gelu;
  t->softmax = simd_sse42_softmax;
  t->log_softmax = simd_sse42_log_softmax;
  t->vexp = simd_sse42_vexp;
  t->vlog = simd_sse42_vlog;
  t->vsqrt = simd_sse42_vsqrt;
  t->vrsqrt = simd_sse42_vrsqrt;
  t->vinv = simd_sse42_vinv;
  t->dist_l2_sq = simd_sse42_dist_l2_sq;
  t->dist_l1 = simd_sse42_dist_l1;
  t->dist_cos = simd_sse42_dist_cos;
  t->dist_cheb = simd_sse42_dist_cheb;
  t->dist_matrix_l2_sq = simd_sse42_dist_matrix_l2_sq;
  t->dist_matrix_cos = simd_sse42_dist_matrix_cos;
  t->dist_matrix_l1 = simd_sse42_dist_matrix_l1;
  t->gemv = simd_sse42_gemv;
  t->gemv_t = simd_sse42_gemv_t;
  t->gemm = simd_sse42_gemm;
  t->threshold = simd_sse42_threshold;
  t->threshold_sign = simd_sse42_threshold_sign;
  t->hamming = simd_sse42_hamming;
  t->topk_indices = simd_sse42_topk_indices;
  t->clamp = simd_sse42_clamp;
}

#else /* !x86_64 */
void simd_override_sse42(simd_t *t) { (void)t; }
#endif /* x86_64 */