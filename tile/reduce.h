#pragma once

#include "../intrinsics.h"
#include "../utils.h"
#include "../types.h"

namespace intgemm {

struct Sum32Op {
  INTGEMM_SSE2 static inline __m128i Run(__m128i first, __m128i second) {
    return add_epi32(first, second);
  }

  INTGEMM_AVX2 static inline __m256i Run(__m256i first, __m256i second) {
    return add_epi32(first, second);
  }

#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
  INTGEMM_AVX512BW static inline __m512i Run(__m512i first, __m512i second) {
    return add_epi32(first, second);
  }
#endif
};

} // namespace intgemm

#ifdef INTGEMM_COMPILER_SUPPORTS_AVX512BW
#define INTGEMM_THIS_IS_AVX512BW
#include "reduce.inl"
#undef INTGEMM_THIS_IS_AVX512BW
#endif

#define INTGEMM_THIS_IS_AVX2
#include "reduce.inl"
#undef INTGEMM_THIS_IS_AVX2

#define INTGEMM_THIS_IS_SSE2
#include "reduce.inl"
#undef INTGEMM_THIS_IS_SSE2
