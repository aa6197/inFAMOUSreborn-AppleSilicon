/* host_simd.h -- the host's SIMD intrinsics and cycle counter, in one place.
 *
 * x86 / x86-64: the compiler's own intrinsics headers, as before. Which
 * instructions a TU may use is still set by its -m flags (__SSE4_1__ etc.).
 *
 * arm64: sse2neon (SSE..SSE4.2 on NEON, third_party/sse2neon, MIT) with its
 * precise modes on, so float results match x86 instead of NEON's faster
 * approximations. _mm_getcsr/_mm_setcsr map MXCSR rounding and FTZ/DAZ onto
 * FPCR. __SSE4_1__ / __AVX2__ stay undefined, so code that checks them keeps
 * its portable path.
 *
 * __rdtsc() on arm64 reads the generic timer (CNTVCT_EL0). Its rate is not
 * the x86 TSC's (24 MHz on Apple silicon), so ticks are only meaningful after
 * calibration against a wall clock -- which every user in this tree already
 * does (QueryPerformanceCounter). Never compare raw ticks with a constant. */
#ifndef PS3EMU_HOST_SIMD_H
#define PS3EMU_HOST_SIMD_H

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <x86intrin.h>
#  endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#  ifndef SSE2NEON_PRECISE_MINMAX
#    define SSE2NEON_PRECISE_MINMAX 1
#  endif
#  ifndef SSE2NEON_PRECISE_DIV
#    define SSE2NEON_PRECISE_DIV 1
#  endif
#  ifndef SSE2NEON_PRECISE_SQRT
#    define SSE2NEON_PRECISE_SQRT 1
#  endif
#  ifndef SSE2NEON_PRECISE_DP
#    define SSE2NEON_PRECISE_DP 1
#  endif
#  include "../../third_party/sse2neon/sse2neon.h"
#  include <stdint.h>
static inline unsigned long long ps3_host_rdtsc(void)
{
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return (unsigned long long)v;
}
#  ifndef __rdtsc
#    define __rdtsc() ps3_host_rdtsc()
#  endif
#else
#  error "host_simd.h: unsupported host architecture (x86, x86-64 or arm64)"
#endif

#endif /* PS3EMU_HOST_SIMD_H */
