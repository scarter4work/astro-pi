#include "core/fingerprint/Starlet.h"

#include "core/Parallel.h"
#include "core/num/Convolve.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ac {

// ---------------------------------------------------------------------------------------------------------
// Why the 5-tap pass is bit-identical to scipy's convolve1d with the explicit dilated kernel.
//
// The dilated kernel is symmetric, so scipy (NI_Correlate1D, symmetric branch; convolve1d's reversal is a
// no-op here) evaluates, for step s = 2**scale and half-width r = 2s, in this order:
//     acc = x[0] * 6/16
//     for jj = -r .. -1:  acc += (x[jj] + x[-jj]) * w[jj]
// where w[-2s] = 1/16, w[-s] = 4/16 and every other w[jj] is exactly 0.0. The fast pass performs only the
// three non-zero steps, in the same order, with the same operands.
//
// A zero tap adds z = (x[jj] + x[-jj]) * 0.0. When |x| <= kSafe = 1e300 everywhere, x[jj] + x[-jj] is finite
// (|.| <= 2e300), so z is exactly +0.0 or -0.0, and every partial sum stays finite (|acc| <= kSafe, the
// weights sum to 1). Adding a signed zero to a finite acc changes nothing, except -0.0 + +0.0 = +0.0 under
// round-to-nearest. So after any run of zero taps the scipy accumulator either equals the fast one or is
// +0.0 where the fast one is -0.0; and adding the same next term p to (+0.0, -0.0) gives identical results
// unless p is itself a zero, in which case the pair is again (+0.0 or equal, -0.0). By induction the two
// final values are bit-identical unless the fast result is -0.0. Those (rare) samples are recomputed with
// the full scipy tap loop (exact_tap_loop). Inputs outside the bound (NaN, +/-Inf, |x| > 1e300, where
// Inf*0 = NaN or an overflowing sum would poison the zero taps) send the whole pass to convolve1d_mirror()
// with the explicit kernel, which is scipy's arithmetic by construction (Task 3).
// test_starlet.cpp:b3_fast_pass_matches_convolve1d_bitwise checks all of this bitwise.
// ---------------------------------------------------------------------------------------------------------

namespace {

constexpr double kW0 = 6.0 / 16.0;   // centre
constexpr double kW1 = 4.0 / 16.0;   // at +/- s
constexpr double kW2 = 1.0 / 16.0;   // at +/- 2s
constexpr double kSafe = 1e300;

bool is_negative_zero(double v)
{
   uint64_t u;
   std::memcpy(&u, &v, sizeof u);
   return u == 0x8000000000000000ull;
}

bool all_within_safe_bound(const Plane& in)
{
   bool bad = false;
   for (double v : in.data)
      bad |= !(std::fabs(v) <= kSafe);   // NaN compares false: caught
   return !bad;
}

// scipy's symmetric tap loop over the explicit dilated kernel, for one output sample. get(k) is the
// (mirror-extended) input at offset k from the centre.
template <class Get>
double exact_tap_loop(long s, const Get& get)
{
   double acc = get(0) * kW0;
   for (long jj = -2 * s; jj < 0; ++jj) {
      const double w = jj == -2 * s ? kW2 : (jj == -s ? kW1 : 0.0);
      acc += (get(jj) + get(-jj)) * w;
   }
   return acc;
}

constexpr size_t kRowsPerTask = 8;

// axis 1: along x, one row at a time. Interior samples (all taps inside the row) read the row directly;
// the border samples resolve each tap through extend_index, so no buffer grows with the kernel width.
void smooth_rows(const Plane& in, Plane& out, long s, unsigned threads)
{
   const long W = in.width, r = 2 * s;
   const size_t H = static_cast<size_t>(in.height);
   const size_t tasks = (H + kRowsPerTask - 1) / kRowsPerTask;
   const long inner0 = std::min(r, W), inner1 = std::max(inner0, W - r);   // [inner0, inner1): no tap leaves the row
   parallel_for(tasks, [&](size_t t) {
      const size_t y1 = std::min(H, (t + 1) * kRowsPerTask);
      for (size_t y = t * kRowsPerTask; y < y1; ++y) {
         const double* row = in.data.data() + y * static_cast<size_t>(W);
         double* o = out.data.data() + y * static_cast<size_t>(W);
         auto at = [&](long i) { return row[(i >= 0 && i < W) ? i : extend_index(i, W, EdgeMode::Mirror)]; };
         auto border = [&](long p) {
            double acc = row[p] * kW0;
            acc += (at(p - r) + at(p + r)) * kW2;
            acc += (at(p - s) + at(p + s)) * kW1;
            o[p] = acc;
         };
         for (long p = 0; p < inner0; ++p)
            border(p);
         for (long p = inner0; p < inner1; ++p) {
            double acc = row[p] * kW0;
            acc += (row[p - r] + row[p + r]) * kW2;
            acc += (row[p - s] + row[p + s]) * kW1;
            o[p] = acc;
         }
         for (long p = inner1; p < W; ++p)
            border(p);
         for (long p = 0; p < W; ++p)
            if (is_negative_zero(o[p]))
               o[p] = exact_tap_loop(s, [&](long k) { return at(p + k); });
      }
   }, threads);
}

// axis 0: down the columns, computed a whole output row at a time from five (mirrored) input rows, so
// every access is a contiguous row stream (no transpose needed; per-sample arithmetic is unchanged).
void smooth_cols(const Plane& in, Plane& out, long s, unsigned threads)
{
   const long H = in.height;
   const size_t W = static_cast<size_t>(in.width);
   const size_t tasks = (static_cast<size_t>(H) + kRowsPerTask - 1) / kRowsPerTask;
   auto mirror = [H](long i) { return (i >= 0 && i < H) ? i : extend_index(i, H, EdgeMode::Mirror); };
   parallel_for(tasks, [&](size_t t) {
      const long y1 = std::min<long>(H, static_cast<long>((t + 1) * kRowsPerTask));
      for (long y = static_cast<long>(t * kRowsPerTask); y < y1; ++y) {
         const double* c = in.data.data() + static_cast<size_t>(y) * W;
         const double* m2 = in.data.data() + static_cast<size_t>(mirror(y - 2 * s)) * W;
         const double* p2 = in.data.data() + static_cast<size_t>(mirror(y + 2 * s)) * W;
         const double* m1 = in.data.data() + static_cast<size_t>(mirror(y - s)) * W;
         const double* p1 = in.data.data() + static_cast<size_t>(mirror(y + s)) * W;
         double* o = out.data.data() + static_cast<size_t>(y) * W;
         for (size_t x = 0; x < W; ++x) {
            double acc = c[x] * kW0;
            acc += (m2[x] + p2[x]) * kW2;
            acc += (m1[x] + p1[x]) * kW1;
            o[x] = acc;
         }
         for (size_t x = 0; x < W; ++x)
            if (is_negative_zero(o[x]))
               o[x] = exact_tap_loop(s, [&](long k) { return in.data[static_cast<size_t>(mirror(y + k)) * W + x]; });
      }
   }, threads);
}

void check_scale_axis(int scale, int axis)
{
   if (scale < 0 || scale > 24)
      throw std::invalid_argument("b3: scale must be in [0, 24], got " + std::to_string(scale));
   if (axis != 0 && axis != 1)
      throw std::invalid_argument("b3_smooth_axis: axis must be 0 or 1");
}

} // namespace

std::vector<double> b3_dilated_kernel(int scale)
{
   check_scale_axis(scale, 0);
   const size_t step = size_t(1) << scale;
   std::vector<double> k(4 * step + 1, 0.0);
   const double taps[5] = {kW2, kW1, kW0, kW1, kW2};   // np.array([1, 4, 6, 4, 1]) / 16.0 (all exact)
   for (size_t i = 0; i < 5; ++i)
      k[i * step] = taps[i];
   return k;
}

Plane b3_smooth_axis(const Plane& in, int scale, int axis, unsigned threads)
{
   check_scale_axis(scale, axis);
   if (in.data.empty())
      return Plane(in.width, in.height);
   if (!all_within_safe_bound(in))
      return convolve1d_mirror(in, b3_dilated_kernel(scale), axis, threads);
   Plane out(in.width, in.height);
   const long s = 1L << scale;
   if (axis == 1)
      smooth_rows(in, out, s, threads);
   else
      smooth_cols(in, out, s, threads);
   return out;
}

Starlet starlet_transform(const Plane& image, int n_scales, unsigned threads)
{
   if (n_scales < 1)
      throw std::invalid_argument("n_scales must be >= 1, got " + std::to_string(n_scales));
   if (n_scales > kMaxStarletScales)
      throw std::invalid_argument(
         "n_scales must be <= " + std::to_string(kMaxStarletScales) + ", got " + std::to_string(n_scales) +
         ": the last scale's B3 kernel would be 4 * 2^" + std::to_string(n_scales - 1) +
         " + 1 px wide, and one wider than 2^26 px only mirror-bounces any image PixInsight can hold "
         "(Python accepts it but builds that kernel explicitly and runs out of memory a few scales later)");
   Starlet st;
   st.planes.reserve(static_cast<size_t>(n_scales));
   Plane current = image;
   const size_t n = current.data.size();
   for (int scale = 0; scale < n_scales; ++scale) {
      Plane smoothed = b3_smooth_axis(b3_smooth_axis(current, scale, 0, threads), scale, 1, threads);
      Plane plane(current.width, current.height);
      constexpr size_t kBlock = 1 << 14;
      parallel_for((n + kBlock - 1) / kBlock, [&](size_t b) {
         const size_t i1 = std::min(n, (b + 1) * kBlock);
         for (size_t i = b * kBlock; i < i1; ++i)
            plane.data[i] = current.data[i] - smoothed.data[i];
      }, threads);
      st.planes.push_back(std::move(plane));
      current = std::move(smoothed);
   }
   st.residual = std::move(current);
   return st;
}

} // namespace ac
