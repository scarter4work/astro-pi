// AutoContrast core (PCL-free): numpy's float64 add-reduction order.
#pragma once

#include <cstddef>

namespace ac {

namespace detail {

// numpy/_core/src/umath/loops_utils.h.src::DOUBLE_pairwise_sum over term(off .. off+n-1): fewer than 8 terms
// summed in order from 0.0; up to 128 terms in 8 interleaved accumulators combined
// ((r0+r1)+(r2+r3))+((r4+r5)+(r6+r7)) plus a serial tail; beyond that split at n/2 rounded down to a multiple
// of 8 and recurse.
template <class F>
double pairwise_sum_rec(size_t off, size_t n, const F& term)
{
   if (n < 8) {
      double res = 0.0;
      for (size_t i = 0; i < n; ++i)
         res += term(off + i);
      return res;
   }
   if (n <= 128) {
      double r[8];
      for (size_t j = 0; j < 8; ++j)
         r[j] = term(off + j);
      size_t i;
      for (i = 8; i < n - (n % 8); i += 8)
         for (size_t j = 0; j < 8; ++j)
            r[j] += term(off + i + j);
      double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
      for (; i < n; ++i)
         res += term(off + i);
      return res;
   }
   size_t n2 = n / 2;
   n2 -= n2 % 8;
   return pairwise_sum_rec(off, n2, term) + pairwise_sum_rec(off + n2, n - n2, term);
}

} // namespace detail

// np.sum / np.add.reduce of term(off .. off+n-1) under numpy 2.5.3: the output starts at add's identity
// +0.0 and the pairwise sum of ALL n elements is added to it (no element of the input seeds the
// accumulator). Over a whole contiguous array -- including axis=(1, 2) of an (n, H, W) C-contiguous array,
// one call per plane over H*W -- there is no 8192-element buffer blocking (verified bitwise on 96x80 ..
// 1600x1067 planes; older numpy blocked by np.BUFSIZE). The identity matters only for signed zeros: an
// all -0.0 input gives +0.0, as numpy does. `term(i)` lets the caller fuse an elementwise map (e.g. x*x for
// np.sum(x**2)) without materialising it; the arithmetic is unchanged because the map's result is rounded
// before it is summed. (No -ffast-math anywhere in ac_core, so 0.0 + s is never folded to s.)
template <class F>
double pairwise_sum(size_t off, size_t n, const F& term)
{
   return 0.0 + detail::pairwise_sum_rec(off, n, term);
}

inline double pairwise_sum(const double* a, size_t n)
{
   return pairwise_sum(0, n, [a](size_t i) { return a[i]; });
}

} // namespace ac
