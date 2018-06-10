#include "AVX_Matrix_Mult.h"

#include <cassert>
#include <emmintrin.h>
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tmmintrin.h>
#include <xmmintrin.h>

namespace intgemm {

#ifdef __AVX512F__
namespace AVX512 {

namespace {

union FloatAccess {
  float as_f[4];
  __m128 as_n;
};
union IntAccess {
  int32_t as_i[4];
  __m128i as_n;
};

/* Convert 16-bit to 32-bit and add, not caring what parts are added.
 * Implementations:
 * 1. https://github.com/tesseract-ocr/tesseract/blob/master/src/arch/intsimdmatrixavx2.cpp#L67 under Apache license:
 *   This does a multiply by 1 and horizontal add:
 *    _mm512_madd_epi16(sum, _mm512_set1_epi16(1))
 *   Current fastest.
 *
 * 2. Signed extension and fold halves:
 *    sum = _mm512_add_epi32(
 *      _mm512_cvtepi16_epi32(_mm512_castsi512_si256(sum)),
 *      _mm512_cvtepi16_epi32(_mm512_extracti64x4_epi64(sum, 1)));
 *
 * 3. Sign extend by abuse of bitshift, then add.
 * sum = _mm512_add_epi32(
 *      _mm512_srai_epi32(_mm512_slli_epi32(sum, 16), 16),
 *      _mm512_srai_epi32(sum, 16));
 */
inline void Convert32Sum(__m512i &sum) {
  sum = _mm512_madd_epi16(sum, _mm512_set1_epi16(1));
}

// Two sum version.
struct ReducedPair {
  int32_t result[2];
};
inline ReducedPair Reduce16to32(__m512i sum1, __m512i sum2) {
  Convert32Sum(sum1);
  Convert32Sum(sum2);
  // 1 2 1 2 1 2 1 2 1 2 1 2 1 2 1 2
  __m512i pack12 = _mm512_add_epi32(_mm512_unpackhi_epi32(sum1, sum2), _mm512_unpacklo_epi32(sum1, sum2));
  // 1 2 1 2 1 2 1 2
  __m256i halves = _mm256_add_epi32(_mm512_castsi512_si256(pack12), _mm512_extracti64x4_epi64(pack12, 1));
  // 1 2 1 2
  IntAccess a;
  a.as_n = _mm_add_epi32(_mm256_castsi256_si128(halves), _mm256_extracti128_si256(halves, 1));
  ReducedPair ret;
  ret.result[0] = a.as_i[0] + a.as_i[2];
  ret.result[1] = a.as_i[1] + a.as_i[3];
  return ret;
}

// Assuming sum1, sum2, sum3, and sum4 are arrays 32-bit signed integers,
// reduce within each.
// Returns [sum(sum1), sum(sum2), sum(sum3), sum(sum4)]
// TODO: consider doing in 64-bit, allowing 4 more bits of quantization?
inline __m128i Reduce32(__m512i sum1, __m512i sum2, __m512i sum3, __m512i sum4) {
  // 1 2 1 2 1 2 1 2 1 2 1 2 1 2 1 2
  __m512i pack12 = _mm512_add_epi32(_mm512_unpackhi_epi32(sum1, sum2), _mm512_unpacklo_epi32(sum1, sum2));
  // 3 4 3 4 3 4 3 4 3 4 3 4 3 4 3 4
  __m512i pack34 = _mm512_add_epi32(_mm512_unpackhi_epi32(sum3, sum4), _mm512_unpacklo_epi32(sum3, sum4));
  // 1 2 3 4 1 2 3 4 1 2 3 4 1 2 3 4
  __m512i pack1234 = _mm512_add_epi32(_mm512_unpackhi_epi64(pack12, pack34), _mm512_unpacklo_epi64(pack12, pack34));
  // Cut the register into halves and sum those.  1 2 3 4 1 2 3 4
  __m256i halves = _mm256_add_epi32(_mm512_castsi512_si256(pack1234), _mm512_extracti64x4_epi64(pack1234, 1));
  // Again: cut the register into halves and sum those. 1 2 3 4
  return _mm_add_epi32(_mm256_castsi256_si128(halves), _mm256_extracti128_si256(halves, 1));
}

// Four sum version
inline __m128i Reduce16to32(__m512i sum1, __m512i sum2, __m512i sum3, __m512i sum4) {
  Convert32Sum(sum1);
  Convert32Sum(sum2);
  Convert32Sum(sum3);
  Convert32Sum(sum4);
  return Reduce32(sum1, sum2, sum3, sum4);
}

// Somewhat inefficient reduce for single __m256i containing int32_t
inline int32_t Reduce32(__m256i halves) {
  IntAccess a;
  a.as_n = _mm_add_epi32(_mm256_castsi256_si128(halves), _mm256_extracti128_si256(halves, 1));
  // TODO is there a more efficient way?
  return a.as_i[0] + a.as_i[1] + a.as_i[2] + a.as_i[3];
}

// Somewhat inefficient reduce for single __m512i containing int32_t
inline int32_t Reduce32(__m512i sum1) {
  // Fold register over itself.
  return Reduce32(_mm256_add_epi32(_mm512_castsi512_si256(sum1), _mm512_extracti64x4_epi64(sum1, 1)));
}

inline int32_t Reduce16to32(__m512i sum1) {
  Convert32Sum(sum1);
  // Fold register over itself.
  return Reduce32(_mm256_add_epi32(_mm512_castsi512_si256(sum1), _mm512_extracti64x4_epi64(sum1, 1)));
}

class ScatterPut {
  public:
    explicit ScatterPut(float unquant_mult, int num_B_rows)
      : unquant_mult_(unquant_mult),
        unquant_mult_sse_(_mm_set1_ps(unquant_mult)),
       num_b_rows_scatter_(_mm_set_epi32(num_B_rows * 3 * sizeof(float), num_B_rows * 2 * sizeof(float), num_B_rows * 1 * sizeof(float), num_B_rows * 0 * sizeof(float))),
       num_B_rows_(num_B_rows) {}

    inline void Write(float *base, __m128i reduced) {
      __m128 float_sums = _mm_cvtepi32_ps(reduced);
      float_sums = _mm_mul_ps(float_sums, unquant_mult_sse_);
      // The scatter instruction requires avx512vl
      _mm_i32scatter_ps(base, num_b_rows_scatter_, float_sums, 1);
    }

    inline void Write(float *base, ReducedPair reduced) {
      base[0] = unquant_mult_ * static_cast<float>(reduced.result[0]);
      base[num_B_rows_] = unquant_mult_ * static_cast<float>(reduced.result[1]);
    }

    inline void Write(float *base, int32_t reduced) {
      base[0] = unquant_mult_ * static_cast<float>(reduced);
    }

  private:
    const float unquant_mult_;
    const __m128 unquant_mult_sse_;
    const __m128i num_b_rows_scatter_;
    const int num_B_rows_;
};

} // namespace


// This is an AVX512F implementation of int16_t multiply based on Jacob
// Devlin's SSE code.  The original SSE code was:

// Copyright (c) 2017 Microsoft Corporation

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.


// We are multiplying A * B^T, as opposed to A * B. This is important because it means we can do consecutive memory access on A * B^T which allows to to take the most
// advantage of L1 cache.
// 
// B is typically a weight matrix, so it can be pre-processed offline, and therefore this transpose does not cost anything.
// A is typically an activation minibatch matrix.
// A and B must be 64-byte aligned.
// C should be the usual 4-byte alignment.
void MatrixMult16(const __m512i * A, const __m512i * B, float * C, float unquant_mult, int num_A_rows, int num_B_rows, int width) {
    assert(width % 32 == 0);
    assert(reinterpret_cast<uintptr_t>(A) % 64 == 0);
    assert(reinterpret_cast<uintptr_t>(B) % 64 == 0);

    ScatterPut put(unquant_mult, num_B_rows);

    const int sse_width = width/32;

    // We do loop unrolling over A. This is *significantly* faster
    // since B can live in the registers. We are assuming that
    // A is a multiple of 4, but we can add extra code to handle values of 1, 2, 3.
    //
    // We could also do loop unrolling over B, which adds some additional speedup.
    // We don't do that for the sake of clarity.
    // 
    // There are other memory access patterns we could do, e.g., put B on the outer loop.
    // The justification is that A is typically small enough that it can live in L1 cache.
    // B is usually a larger weight matrix, so it might not be able to. However, we are using
    // each element of B four times while it's still in a register, so caching is not as important.

    // Round down to a multiple of 4.
    int num_unroll_rows = num_A_rows & ~3;
    for (int i = 0; i < num_unroll_rows; i += 4) {
        const __m512i * A1_row = A + (i+0)*sse_width;
        const __m512i * A2_row = A + (i+1)*sse_width;
        const __m512i * A3_row = A + (i+2)*sse_width;
        const __m512i * A4_row = A + (i+3)*sse_width;

        for (int j = 0; j < num_B_rows; j++) {
            const __m512i * B_row = B + j*sse_width;

            __m512i sum1 = _mm512_setzero_si512();
            __m512i sum2 = _mm512_setzero_si512();
            __m512i sum3 = _mm512_setzero_si512();
            __m512i sum4 = _mm512_setzero_si512();

            // This is just a simple dot product, unrolled four ways.
            for (int k = 0; k < sse_width; k++) {
                __m512i b = *(B_row + k);
                
                __m512i a1 = *(A1_row + k);
                __m512i a2 = *(A2_row + k);
                __m512i a3 = *(A3_row + k);
                __m512i a4 = *(A4_row + k);

                // madd_epi16 does multiply add on 8 16-bit integers and accumulates into a four 32-bit register.
                // E.g.,
                // a1 = [f1, f2, f3, f4, f5, f6, f7, h8] (16-bit ints)
                // b1 = [h1, h2, h3, h4, h5, h6, h7, h8] (16-bit ints)
                // result = [f1*h1 + f2*h2, f3*h3 + f4*h4, f5*h5 + f6*h6, f7*h7 + f8*h8] (32-bit ints)
                // Then add_epi32 just effectively does a += on these 32-bit integers.
                sum1 = _mm512_add_epi32(sum1, _mm512_madd_epi16(b, a1));
                sum2 = _mm512_add_epi32(sum2, _mm512_madd_epi16(b, a2));
                sum3 = _mm512_add_epi32(sum3, _mm512_madd_epi16(b, a3));
                sum4 = _mm512_add_epi32(sum4, _mm512_madd_epi16(b, a4));
            }
            put.Write(C + i * num_B_rows + j, Reduce32(sum1, sum2, sum3, sum4));
        }
    }
    // Handle the non-multiples of 4 rows.
    // TODO: efficient version for 3 rows, 2 rows, etc.
    for (int i = num_unroll_rows; i < num_A_rows; ++i) {
      const __m512i * A1_row = A + i * sse_width;
      for (int j = 0; j < num_B_rows; j++) {
        __m512i sum1 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          const __m512i * B_row = B + j*sse_width;
          __m512i b = *(B_row + k);
          __m512i a1 = *(A1_row + k);
          sum1 = _mm512_add_epi32(sum1, _mm512_madd_epi16(b, a1));
        }
        // TODO is there a more efficient way?
        *(C + (i)*num_B_rows + j) = unquant_mult * static_cast<float>(Reduce32(sum1));
      }
    }
}

namespace {

/* Three ways considered to apply sign bits:
 * 1. Use 256-bit sign instruction:
 *  __m256i a_first = _mm256_sign_epi8(_mm512_castsi512_si256(a), _mm512_castsi512_si256(b));
 *  __m256i a_second = _mm256_sign_epi8(_mm512_extracti64x4_epi64(a, 1), b_second);
 *  a = _mm512_inserti64x4(_mm512_castsi256_si512(a_first), a_second, 1);
 *  a = Concat(a_first, a_second);
 *
 * 2. Extract a mask and xor + 1
 *   __mmask64 neg_mask  _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
 *  Use set1 to to build to_xor
 *  a = _mm512_xor_si512(a, to_xor)
 *  And add one:
 *  const __m512i ones8 = _mm512_set1_epi8(1);
 *  a = _mm512_mask_add_epi8(a, neg_mask, a, ones8);
 *
 * 3. Extract a mask and subtract from 0
 * In the outer loop on b:
 *  __mmask64 neg_mask  _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128))
 * For each a:
 *  a = _mm512_mask_sub_epi8(a, neg_mask, _mm512_setzero_si512(), a);
 *
 * Finally, subtraction won the benchmark
 */
inline void Accum(const __m512i zeros, __m512i a, const __m512i b, const __m512i b_positive, const __mmask64 neg_mask, __m512i &sum) {
  // Apply sign bits.
  a = _mm512_mask_sub_epi8(a, neg_mask, zeros, a);
  // The magic 8-bit multiply then horizontal sum into 16-bit.
  __m512i multiplied = _mm512_maddubs_epi16(b_positive, a);
  // Now we have 16-bit results that are the sum of two multiplies.
  // Choosing to approximate and do adds.
  // Perhaps every so often we could accumulate by Convert32Sum
  sum = _mm512_adds_epi16(sum, multiplied);
}

} // namespace

/*void MatrixMult8(const __m512i *A, const __m512i *B, float *C, float unquant_mult, int num_A_rows, int num_B_rows, int width) {
  assert(width % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(A) % 64 == 0);
  assert(reinterpret_cast<uintptr_t>(B) % 64 == 0);
  assert(num_B_rows % 8 == 0);
  __m128 unquant_reg = _mm_set1_ps(unquant_mult);
  const __m512i zeros = _mm512_setzero_si512();
  const int simd_width = width / 64;
  const __m512i *const B_end = B + num_B_rows * simd_width;
  int B0_rowidx = 0;
  for (const __m512i *B0_row = B; B0_row != B_end; B0_row += 8 * simd_width, B0_rowidx += 8) {
    const __m512i *B1_row = B0_row + simd_width * 1;
    const __m512i *B2_row = B0_row + simd_width * 2;
    const __m512i *B3_row = B0_row + simd_width * 3;
    const __m512i *B4_row = B0_row + simd_width * 4;
    const __m512i *B5_row = B0_row + simd_width * 5;
    const __m512i *B6_row = B0_row + simd_width * 6;
    const __m512i *B7_row = B0_row + simd_width * 7;
    for (int A_rowidx = 0; A_rowidx < num_A_rows; ++A_rowidx) {
      const __m512i *A_row = A + A_rowidx * simd_width;
      __m512i sum0 = _mm512_setzero_si512();
      __m512i sum1 = _mm512_setzero_si512();
      __m512i sum2 = _mm512_setzero_si512();
      __m512i sum3 = _mm512_setzero_si512();
      __m512i sum4 = _mm512_setzero_si512();
      __m512i sum5 = _mm512_setzero_si512();
      __m512i sum6 = _mm512_setzero_si512();
      __m512i sum7 = _mm512_setzero_si512();
      for (int k = 0; k < simd_width; ++k) {
        __m512i a = *(A_row + k);
        __m512i a_positive = _mm512_abs_epi8(a);
        __mmask64 neg_mask = _mm512_test_epi8_mask(a, _mm512_set1_epi8(-128));
        Accum(zeros, *(B0_row + k), a, a_positive, neg_mask, sum0);
        Accum(zeros, *(B1_row + k), a, a_positive, neg_mask, sum1);
        Accum(zeros, *(B2_row + k), a, a_positive, neg_mask, sum2);
        Accum(zeros, *(B3_row + k), a, a_positive, neg_mask, sum3);
        Accum(zeros, *(B4_row + k), a, a_positive, neg_mask, sum4);
        Accum(zeros, *(B5_row + k), a, a_positive, neg_mask, sum5);
        Accum(zeros, *(B6_row + k), a, a_positive, neg_mask, sum6);
        Accum(zeros, *(B7_row + k), a, a_positive, neg_mask, sum7);
      }
      *reinterpret_cast<__m128*>(C + A_rowidx * num_B_rows + B0_rowidx) = _mm_mul_ps(_mm_cvtepi32_ps(Reduce16to32(sum0, sum1, sum2, sum3)), unquant_reg);
      *reinterpret_cast<__m128*>(C + A_rowidx * num_B_rows + B0_rowidx + 4) = _mm_mul_ps(_mm_cvtepi32_ps(Reduce16to32(sum4, sum5, sum6, sum7)), unquant_reg);
    }
  }
}*/

void MatrixMult8Contrast(const __m512i *A, const __m512i *B, float *C, float unquant_mult, int num_A_rows, int num_B_rows, int width) {
  assert(width % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(A) % 64 == 0);
  assert(reinterpret_cast<uintptr_t>(B) % 64 == 0);
  assert(num_B_rows % 8 == 0);
  __m128 unquant_reg = _mm_set1_ps(unquant_mult);
  const __m512i zeros = _mm512_setzero_si512();
  const int simd_width = width / 64;
  const __m512i *const B_end = B + num_B_rows * simd_width;
  int B0_rowidx = 0;
  for (const __m512i *B0_row = B; B0_row != B_end; B0_row += 8 * simd_width, B0_rowidx += 8) {
    const __m512i *B1_row = B0_row + simd_width * 1;
    const __m512i *B2_row = B0_row + simd_width * 2;
    const __m512i *B3_row = B0_row + simd_width * 3;
    const __m512i *B4_row = B0_row + simd_width * 4;
    const __m512i *B5_row = B0_row + simd_width * 5;
    const __m512i *B6_row = B0_row + simd_width * 6;
    const __m512i *B7_row = B0_row + simd_width * 7;
    for (int A_rowidx = 0; A_rowidx < num_A_rows; ++A_rowidx) {
      const __m512i *A_row = A + A_rowidx * simd_width;
      __m512i sum0 = _mm512_setzero_si512();
      __m512i sum1 = _mm512_setzero_si512();
      __m512i sum2 = _mm512_setzero_si512();
      __m512i sum3 = _mm512_setzero_si512();
      __m512i sum4 = _mm512_setzero_si512();
      __m512i sum5 = _mm512_setzero_si512();
      __m512i sum6 = _mm512_setzero_si512();
      __m512i sum7 = _mm512_setzero_si512();
      for (int k = 0; k < simd_width; ++k) {
        __m512i a = *(A_row + k);
        __mmask64 neg_mask = _mm512_test_epi8_mask(a, _mm512_set1_epi8(-128));
        __m512i a_positive = _mm512_abs_epi8(a);
        __m512i b0 = *(B0_row + k);
        __m512i b1 = *(B1_row + k);
        __m512i b2 = *(B2_row + k);
        __m512i b3 = *(B3_row + k);
        __m512i b4 = *(B4_row + k);
        __m512i b5 = *(B5_row + k);
        __m512i b6 = *(B6_row + k);
        __m512i b7 = *(B7_row + k);
        b0 = _mm512_mask_sub_epi8(b0, neg_mask, zeros, b0);
        b1 = _mm512_mask_sub_epi8(b1, neg_mask, zeros, b1);
        b2 = _mm512_mask_sub_epi8(b2, neg_mask, zeros, b2);
        b3 = _mm512_mask_sub_epi8(b3, neg_mask, zeros, b3);
        b4 = _mm512_mask_sub_epi8(b4, neg_mask, zeros, b4);
        b5 = _mm512_mask_sub_epi8(b5, neg_mask, zeros, b5);
        b6 = _mm512_mask_sub_epi8(b6, neg_mask, zeros, b6);
        b7 = _mm512_mask_sub_epi8(b7, neg_mask, zeros, b7);
        __m512i mult0 = _mm512_maddubs_epi16(a_positive, b0);
        __m512i mult1 = _mm512_maddubs_epi16(a_positive, b1);
        __m512i mult2 = _mm512_maddubs_epi16(a_positive, b2);
        __m512i mult3 = _mm512_maddubs_epi16(a_positive, b3);
        __m512i mult4 = _mm512_maddubs_epi16(a_positive, b4);
        __m512i mult5 = _mm512_maddubs_epi16(a_positive, b5);
        __m512i mult6 = _mm512_maddubs_epi16(a_positive, b6);
        __m512i mult7 = _mm512_maddubs_epi16(a_positive, b7);
        sum0 = _mm512_adds_epi16(mult0, sum0);
        sum1 = _mm512_adds_epi16(mult1, sum1);
        sum2 = _mm512_adds_epi16(mult2, sum2);
        sum3 = _mm512_adds_epi16(mult3, sum3);
        sum4 = _mm512_adds_epi16(mult4, sum4);
        sum5 = _mm512_adds_epi16(mult5, sum5);
        sum6 = _mm512_adds_epi16(mult6, sum6);
        sum7 = _mm512_adds_epi16(mult7, sum7);
      }
      *reinterpret_cast<__m128*>(C + A_rowidx * num_B_rows + B0_rowidx) = _mm_mul_ps(_mm_cvtepi32_ps(Reduce16to32(sum0, sum1, sum2, sum3)), unquant_reg);
      *reinterpret_cast<__m128*>(C + A_rowidx * num_B_rows + B0_rowidx + 4) = _mm_mul_ps(_mm_cvtepi32_ps(Reduce16to32(sum4, sum5, sum6, sum7)), unquant_reg);
    }
  }
}


void MatrixMult8(const __m512i * A, const __m512i * B, float * C, float unquant_mult, int num_A_rows, int num_B_rows, int width) {
  assert(width % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(A) % 64 == 0);
  assert(reinterpret_cast<uintptr_t>(B) % 64 == 0);
  ScatterPut put(unquant_mult, num_B_rows);
  const __m512i zeros = _mm512_setzero_si512();

  const int sse_width = width/64;
  int i = 0;
  int mult8rows = num_A_rows & (~7);

  for (; i < mult8rows; i += 8) {
    const __m512i *A1_row = A + (i+0)*sse_width;
    const __m512i *A2_row = A + (i+1)*sse_width;
    const __m512i *A3_row = A + (i+2)*sse_width;
    const __m512i *A4_row = A + (i+3)*sse_width;
    const __m512i *A5_row = A + (i+4)*sse_width;
    const __m512i *A6_row = A + (i+5)*sse_width;
    const __m512i *A7_row = A + (i+6)*sse_width;
    const __m512i *A8_row = A + (i+7)*sse_width;
    for (int j = 0; j < num_B_rows; j++) {
      const __m512i *B_row = B + j*sse_width;
      __m512i sum1 = _mm512_setzero_si512();
      __m512i sum2 = _mm512_setzero_si512();
      __m512i sum3 = _mm512_setzero_si512();
      __m512i sum4 = _mm512_setzero_si512();
      __m512i sum5 = _mm512_setzero_si512();
      __m512i sum6 = _mm512_setzero_si512();
      __m512i sum7 = _mm512_setzero_si512();
      __m512i sum8 = _mm512_setzero_si512();
      for (int k = 0; k < sse_width; k++) {
        __m512i b = *(B_row + k);
        __m512i b_positive = _mm512_abs_epi8(b);
        // Didn't seem to make a difference definining sign bits here vs at top
        __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
        Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
        Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
        Accum(zeros, *(A3_row + k), b, b_positive, neg_mask, sum3);
        Accum(zeros, *(A4_row + k), b, b_positive, neg_mask, sum4);
        Accum(zeros, *(A5_row + k), b, b_positive, neg_mask, sum5);
        Accum(zeros, *(A6_row + k), b, b_positive, neg_mask, sum6);
        Accum(zeros, *(A7_row + k), b, b_positive, neg_mask, sum7);
        Accum(zeros, *(A8_row + k), b, b_positive, neg_mask, sum8);
      }
      put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2, sum3, sum4));
      put.Write(C + (i+4) *num_B_rows + j, Reduce16to32(sum5, sum6, sum7, sum8));
    }
  }

  const __m512i *A1_row = A + (i+0)*sse_width;
  const __m512i *A2_row = A + (i+1)*sse_width;
  const __m512i *A3_row = A + (i+2)*sse_width;
  const __m512i *A4_row = A + (i+3)*sse_width;
  const __m512i *A5_row = A + (i+4)*sse_width;
  const __m512i *A6_row = A + (i+5)*sse_width;
  const __m512i *A7_row = A + (i+6)*sse_width;
  switch (num_A_rows & 7) {
    case 7:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        __m512i sum2 = _mm512_setzero_si512();
        __m512i sum3 = _mm512_setzero_si512();
        __m512i sum4 = _mm512_setzero_si512();
        __m512i sum5 = _mm512_setzero_si512();
        __m512i sum6 = _mm512_setzero_si512();
        __m512i sum7 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
          Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
          Accum(zeros, *(A3_row + k), b, b_positive, neg_mask, sum3);
          Accum(zeros, *(A4_row + k), b, b_positive, neg_mask, sum4);
          Accum(zeros, *(A5_row + k), b, b_positive, neg_mask, sum5);
          Accum(zeros, *(A6_row + k), b, b_positive, neg_mask, sum6);
          Accum(zeros, *(A7_row + k), b, b_positive, neg_mask, sum7);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2, sum3, sum4));
        put.Write(C + (i+4) *num_B_rows + j, Reduce16to32(sum5, sum6));
        put.Write(C + (i+6) *num_B_rows + j, Reduce16to32(sum7));
      }
    case 6:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        __m512i sum2 = _mm512_setzero_si512();
        __m512i sum3 = _mm512_setzero_si512();
        __m512i sum4 = _mm512_setzero_si512();
        __m512i sum5 = _mm512_setzero_si512();
        __m512i sum6 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
          Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
          Accum(zeros, *(A3_row + k), b, b_positive, neg_mask, sum3);
          Accum(zeros, *(A4_row + k), b, b_positive, neg_mask, sum4);
          Accum(zeros, *(A5_row + k), b, b_positive, neg_mask, sum5);
          Accum(zeros, *(A6_row + k), b, b_positive, neg_mask, sum6);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2, sum3, sum4));
        put.Write(C + (i+4) *num_B_rows + j, Reduce16to32(sum5, sum6));
      }
    case 5:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        __m512i sum2 = _mm512_setzero_si512();
        __m512i sum3 = _mm512_setzero_si512();
        __m512i sum4 = _mm512_setzero_si512();
        __m512i sum5 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
          Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
          Accum(zeros, *(A3_row + k), b, b_positive, neg_mask, sum3);
          Accum(zeros, *(A4_row + k), b, b_positive, neg_mask, sum4);
          Accum(zeros, *(A5_row + k), b, b_positive, neg_mask, sum5);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2, sum3, sum4));
        put.Write(C + (i+4) *num_B_rows + j, Reduce16to32(sum5));
      }
    case 4:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        __m512i sum2 = _mm512_setzero_si512();
        __m512i sum3 = _mm512_setzero_si512();
        __m512i sum4 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
          Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
          Accum(zeros, *(A3_row + k), b, b_positive, neg_mask, sum3);
          Accum(zeros, *(A4_row + k), b, b_positive, neg_mask, sum4);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2, sum3, sum4));
      }
    case 3:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        __m512i sum2 = _mm512_setzero_si512();
        __m512i sum3 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
          Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
          Accum(zeros, *(A3_row + k), b, b_positive, neg_mask, sum3);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2));
        put.Write(C + (i+2) *num_B_rows + j, Reduce16to32(sum3));
      }
    case 2:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        __m512i sum2 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
          Accum(zeros, *(A2_row + k), b, b_positive, neg_mask, sum2);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1, sum2));
      }
    case 1:
      for (int j = 0; j < num_B_rows; j++) {
        const __m512i *B_row = B + j*sse_width;
        __m512i sum1 = _mm512_setzero_si512();
        for (int k = 0; k < sse_width; k++) {
          __m512i b = *(B_row + k);
          __m512i b_positive = _mm512_abs_epi8(b);
          __mmask64 neg_mask = _mm512_test_epi8_mask(b, _mm512_set1_epi8(-128));
          Accum(zeros, *(A1_row + k), b, b_positive, neg_mask, sum1);
        }
        put.Write(C + i *num_B_rows + j, Reduce16to32(sum1));
      }
  }
}

} // namespace AVX512
#endif // __AVX512__
} // namespace intgemm
