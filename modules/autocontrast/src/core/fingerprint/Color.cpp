#include "core/fingerprint/Color.h"

#include "core/Parallel.h"

#include <cmath>
#include <stdexcept>

namespace ac {

namespace {

// Python: _DELTA = 6.0 / 29.0; _DELTA**3 and 3 * _DELTA**2 are float ** int -> libm pow().
struct LabConstants {
   double delta = 6.0 / 29.0;
   double delta3 = std::pow(6.0 / 29.0, 3.0);
   double slope = 3 * std::pow(6.0 / 29.0, 2.0);
   double offset = 4.0 / 29.0;
};

inline double srgb_to_linear(double c)
{
   // np.where evaluates both branches; selecting by the predicate gives the same value (NaN falls to pow).
   return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

inline double lab_f(double t, const LabConstants& k)
{
   return t > k.delta3 ? std::cbrt(t) : t / k.slope + k.offset;
}

} // namespace

void rgb_to_lab(const Image& rgb, Plane& L, Plane& a, Plane& b, unsigned threads)
{
   if (rgb.channels != 3)
      throw std::invalid_argument("rgb_to_lab expects an (H, W, 3) sRGB image, got " + std::to_string(rgb.height) +
                                  "x" + std::to_string(rgb.width) + "x" + std::to_string(rgb.channels));
   static const double M[3][3] = {{0.4124564, 0.3575761, 0.1804375},
                                  {0.2126729, 0.7151522, 0.0721750},
                                  {0.0193339, 0.1191920, 0.9503041}};
   static const double W[3] = {0.95047, 1.0, 1.08883};
   const LabConstants k;

   L = Plane(rgb.width, rgb.height);
   a = Plane(rgb.width, rgb.height);
   b = Plane(rgb.width, rgb.height);
   const size_t w = static_cast<size_t>(rgb.width);
   parallel_for(static_cast<size_t>(rgb.height), [&](size_t y) {
      for (size_t x = 0; x < w; ++x) {
         const size_t i = y * w + x;
         const double* p = &rgb.data[i * 3];
         const double l0 = srgb_to_linear(p[0]), l1 = srgb_to_linear(p[1]), l2 = srgb_to_linear(p[2]);
         double f[3];
         for (int j = 0; j < 3; ++j) {
            const double xyz = ((l0 * M[j][0] + l1 * M[j][1]) + l2 * M[j][2]);   // -ffp-contract=off: no FMA
            f[j] = lab_f(xyz / W[j], k);
         }
         L.data[i] = 116.0 * f[1] - 16.0;
         a.data[i] = 500.0 * (f[0] - f[1]);
         b.data[i] = 200.0 * (f[1] - f[2]);
      }
   }, threads);
}

} // namespace ac
