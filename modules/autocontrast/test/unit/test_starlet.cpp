// AutoContrast: starlet (à trous B3) transform and energy spectrum vs the Python goldens
// (synthetic/starlet, synthetic/energy) — fingerprint/starlet.py and fingerprint/energy.py.
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Image.h"
#include "core/Parallel.h"
#include "core/fingerprint/Energy.h"
#include "core/fingerprint/Starlet.h"
#include "core/num/Convolve.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; }

bool same_bits(const std::vector<double>& a, const std::vector<double>& b)
{
   return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

void report(const char* what, double d) { std::printf("[max|d|] %-44s %.3g\n", what, d); }

ac::Plane plane_from(const ac::test::NpyArray& a)
{
   REQUIRE(a.shape.size() == 2);
   ac::Plane p(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]));
   p.data = a.data;
   return p;
}

// plane i of an (n, H, W) array
std::vector<double> slice(const ac::test::NpyArray& a, size_t i)
{
   REQUIRE(a.shape.size() == 3);
   const size_t hw = a.shape[1] * a.shape[2];
   return std::vector<double>(a.data.begin() + static_cast<long>(i * hw), a.data.begin() + static_cast<long>((i + 1) * hw));
}

double max_abs_diff(const std::vector<double>& a, const std::vector<double>& b)
{
   REQUIRE(a.size() == b.size());
   double m = 0.0;
   for (size_t i = 0; i < a.size(); ++i)
      m = std::max(m, std::fabs(a[i] - b[i]));
   return m;
}

struct Case { int h, w, n; };

std::vector<Case> golden_cases()
{
   const auto idx = ac::test::load_json(golden("starlet/index.json"));
   std::vector<Case> cs;
   for (const auto& c : idx["cases"])
      cs.push_back({c["h"].get<int>(), c["w"].get<int>(), c["n"].get<int>()});
   return cs;
}

std::string tag(int h, int w) { return std::to_string(h) + "x" + std::to_string(w); }

ac::Plane random_plane(int w, int h, uint64_t seed)
{
   std::mt19937_64 g(seed);
   std::normal_distribution<double> nd(0.0, 1.0);
   ac::Plane p(w, h);
   for (double& v : p.data)
      v = nd(g);
   return p;
}

} // namespace

TEST_CASE("starlet_planes_match_scipy")
{
   const auto cases = golden_cases();
   REQUIRE(cases.size() >= 9 + 14);           // 3 sizes x {1,5,7} + degenerate shapes + 33x17 at 1..8
   double worst = 0.0;
   size_t bit_exact = 0, total = 0;
   for (const auto& c : cases) {
      const std::string t = tag(c.h, c.w);
      const auto img = plane_from(ac::test::load_npy(golden("starlet/img_" + t + ".npy")));
      const auto want_p = ac::test::load_npy(golden("starlet/planes_" + t + "_n" + std::to_string(c.n) + ".npy"));
      const auto want_r = ac::test::load_npy(golden("starlet/residual_" + t + "_n" + std::to_string(c.n) + ".npy"));
      const auto got = ac::starlet_transform(img, c.n);
      INFO(t << " n" << c.n);
      REQUIRE(got.planes.size() == static_cast<size_t>(c.n));
      REQUIRE(want_p.shape == std::vector<size_t>{static_cast<size_t>(c.n), static_cast<size_t>(c.h), static_cast<size_t>(c.w)});
      for (int s = 0; s < c.n; ++s) {
         REQUIRE(got.planes[static_cast<size_t>(s)].width == c.w);
         REQUIRE(got.planes[static_cast<size_t>(s)].height == c.h);
         const auto want = slice(want_p, static_cast<size_t>(s));
         const double d = max_abs_diff(got.planes[static_cast<size_t>(s)].data, want);
         worst = std::max(worst, d);
         REQUIRE(d <= 1e-12);
         bit_exact += same_bits(got.planes[static_cast<size_t>(s)].data, want);
         ++total;
      }
      const double dr = max_abs_diff(got.residual.data, want_r.data);
      worst = std::max(worst, dr);
      REQUIRE(dr <= 1e-12);
      bit_exact += same_bits(got.residual.data, want_r.data);
      ++total;
   }
   report("starlet planes+residual vs scipy (worst)", worst);
   std::printf("[bits] starlet planes+residual bit-identical to scipy: %zu / %zu\n", bit_exact, total);
}

TEST_CASE("starlet_reconstructs")
{
   double worst = 0.0;
   for (const auto& c : golden_cases()) {
      const auto img = plane_from(ac::test::load_npy(golden("starlet/img_" + tag(c.h, c.w) + ".npy")));
      const auto st = ac::starlet_transform(img, c.n);
      for (size_t i = 0; i < img.data.size(); ++i) {
         double sum = 0.0;
         for (const auto& p : st.planes)
            sum += p.data[i];
         worst = std::max(worst, std::fabs(sum + st.residual.data[i] - img.data[i]));
      }
   }
   report("starlet reconstruction (worst)", worst);
   REQUIRE(worst <= 1e-12);
}

TEST_CASE("starlet_kernel_wider_than_image")
{
   // 7x5 at 7 scales: the scale-6 kernel is 257 taps on axes of 7 and 5 (mirror bounces many times).
   const auto img = plane_from(ac::test::load_npy(golden("starlet/img_7x5.npy")));
   const auto want_p = ac::test::load_npy(golden("starlet/planes_7x5_n7.npy"));
   const auto want_r = ac::test::load_npy(golden("starlet/residual_7x5_n7.npy"));
   const auto got = ac::starlet_transform(img, 7);
   REQUIRE(got.planes.size() == 7);
   for (size_t s = 0; s < 7; ++s) {
      INFO("scale " << s);
      REQUIRE(max_abs_diff(got.planes[s].data, slice(want_p, s)) <= 1e-12);
   }
   REQUIRE(max_abs_diff(got.residual.data, want_r.data) <= 1e-12);
}

TEST_CASE("starlet_rejects_bad_scale_count")
{
   const ac::Plane p(4, 4);
   // the upper bound is explicit and says why (Python has none; see kMaxStarletScales)
   try {
      (void)ac::starlet_transform(p, ac::kMaxStarletScales + 1);
      FAIL("no throw above kMaxStarletScales");
   } catch (const std::invalid_argument& e) {
      CHECK(std::string(e.what()).rfind("n_scales must be <= 25, got 26: ", 0) == 0);
   }
   // and the bound itself runs, without buffers that grow with the kernel, and still reconstructs
   const auto small = random_plane(3, 2, 5);
   const auto st = ac::starlet_transform(small, ac::kMaxStarletScales);
   REQUIRE(st.planes.size() == static_cast<size_t>(ac::kMaxStarletScales));
   for (size_t i = 0; i < small.data.size(); ++i) {
      double sum = 0.0;
      for (const auto& pl : st.planes)
         sum += pl.data[i];
      CHECK(std::fabs(sum + st.residual.data[i] - small.data[i]) <= 1e-12);
   }
   for (int n : {0, -3}) {
      try {
         (void)ac::starlet_transform(p, n);
         FAIL("no throw for n_scales " << n);
      } catch (const std::invalid_argument& e) {
         CHECK(std::string(e.what()) == "n_scales must be >= 1, got " + std::to_string(n));
      }
   }
}

TEST_CASE("starlet_thread_invariant")
{
   const auto in = random_plane(257, 190, 7);
   const auto ref = ac::starlet_transform(in, 7, 1);
   for (unsigned t : {2u, 5u, 32u}) {
      const auto got = ac::starlet_transform(in, 7, t);
      INFO("threads " << t);
      for (size_t s = 0; s < 7; ++s)
         REQUIRE(same_bits(got.planes[s].data, ref.planes[s].data));
      REQUIRE(same_bits(got.residual.data, ref.residual.data));
   }
}

// The 5-tap fast pass must give the bits convolve1d_mirror gives with the explicit dilated kernel
// (2**scale - 1 zero taps between the B3 taps), including signed zeros, non-finite and huge inputs.
TEST_CASE("b3_fast_pass_matches_convolve1d_bitwise")
{
   std::mt19937_64 g(20261112);
   std::normal_distribution<double> nd(0.0, 1.0);
   std::uniform_int_distribution<int> pick(0, 9);
   const double inf = std::numeric_limits<double>::infinity();
   const double nan = std::numeric_limits<double>::quiet_NaN();

   struct Fill { const char* name; int kind; };
   const Fill fills[] = {{"normal", 0}, {"signed-zeros", 1}, {"zeros+cancelling", 2}, {"with-inf", 3},
                         {"with-nan", 4}, {"huge", 5}, {"subnormal", 6}, {"near-bound", 7}};
   const std::pair<int, int> shapes[] = {{1, 1}, {1, 9}, {9, 1}, {2, 3}, {3, 2}, {5, 7}, {17, 33}, {64, 3}, {40, 129}};
   size_t compared = 0, neg_zero_outputs = 0;
   for (const auto& f : fills)
      for (const auto& wh : shapes) {
         ac::Plane in(wh.first, wh.second);
         for (double& v : in.data) {
            const int r = pick(g);
            switch (f.kind) {
            case 0: v = nd(g); break;
            case 1: v = (r & 1) ? -0.0 : 0.0; break;
            case 2: v = r < 6 ? ((r & 1) ? -0.0 : 0.0) : (r < 8 ? 0.5 : -0.5); break;
            case 3: v = r == 0 ? ((g() & 1) ? inf : -inf) : nd(g); break;
            case 4: v = r == 0 ? nan : nd(g); break;
            case 5: v = r < 3 ? std::numeric_limits<double>::max() * (r == 1 ? -1.0 : 1.0) : nd(g) * 1e307; break;
            case 6: v = nd(g) * 1e-310; break;
            default:   // just inside the fast path's |x| <= 1e300 bound: the fast pass, not the fallback
               v = r == 0 ? ((g() & 1) ? 1e300 : -1e300) : std::clamp(nd(g) * 4e299, -1e300, 1e300);
               break;
            }
         }
         for (int scale = 0; scale <= 7; ++scale)
            for (int axis : {0, 1}) {
               const auto want = ac::convolve1d_mirror(in, ac::b3_dilated_kernel(scale), axis, 1);
               const auto got = ac::b3_smooth_axis(in, scale, axis, 1);
               INFO(f.name << " " << wh.first << "x" << wh.second << " scale " << scale << " axis " << axis);
               REQUIRE(got.width == in.width);
               REQUIRE(got.height == in.height);
               for (size_t i = 0; i < want.data.size(); ++i) {
                  INFO("i=" << i << " want " << want.data[i] << " got " << got.data[i]);
                  REQUIRE(bits(got.data[i]) == bits(want.data[i]));
                  neg_zero_outputs += want.data[i] == 0.0 && std::signbit(want.data[i]);
               }
               ++compared;
            }
      }
   std::printf("[bits] b3 fast pass == convolve1d_mirror on %zu plane/scale/axis combos (%zu -0.0 outputs)\n",
               compared, neg_zero_outputs);
   CHECK(neg_zero_outputs > 0);   // the signed-zero repair path really was exercised

   // very wide kernels (2**20 * 4 + 1 taps) on tiny planes: border-only, every tap bounces
   for (const auto& wh : {std::pair<int, int>{1, 9}, {2, 3}, {5, 7}}) {
      ac::Plane in(wh.first, wh.second);
      for (double& v : in.data)
         v = nd(g);
      for (int scale : {9, 12, 20})
         for (int axis : {0, 1}) {
            INFO("wide " << wh.first << "x" << wh.second << " scale " << scale << " axis " << axis);
            const auto want = ac::convolve1d_mirror(in, ac::b3_dilated_kernel(scale), axis, 1);
            const auto got = ac::b3_smooth_axis(in, scale, axis, 1);
            REQUIRE(same_bits(got.data, want.data));
         }
   }
}

TEST_CASE("b3_dilated_kernel_is_pythons")
{
   // _dilate(_B3_KERNEL, scale): zeros((5 - 1) * 2**scale + 1), dilated[::step] = [1,4,6,4,1]/16
   for (int scale = 0; scale <= 6; ++scale) {
      const auto k = ac::b3_dilated_kernel(scale);
      const size_t step = size_t(1) << scale;
      REQUIRE(k.size() == 4 * step + 1);
      const double taps[5] = {1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0};
      for (size_t i = 0; i < k.size(); ++i)
         REQUIRE(bits(k[i]) == bits(i % step == 0 ? taps[i / step] : 0.0));
   }
}

TEST_CASE("energy_matches_numpy")
{
   const auto ec = ac::test::load_json(golden("energy/cases.json"));
   REQUIRE(ec["cases"].size() == 5);
   bool beyond_bufsize = false;   // a plane larger than np.BUFSIZE pins "no 8192-element blocking"
   for (const auto& c : ec["cases"]) {
      const int h = c["h"].get<int>(), w = c["w"].get<int>(), n = c["n"].get<int>();
      const double px = c["pixel_scale"].get<double>();
      beyond_bufsize |= h * w > 8192;
      const auto planes_npy = ac::test::load_npy(golden("starlet/planes_" + tag(h, w) + "_n" + std::to_string(n) + ".npy"));
      std::vector<ac::Plane> planes;
      for (int s = 0; s < n; ++s) {
         ac::Plane p(w, h);
         p.data = slice(planes_npy, static_cast<size_t>(s));
         planes.push_back(std::move(p));
      }
      const auto spec = ac::energy_spectrum_from_planes(planes, px);
      const auto want_e = c["energy"].get<std::vector<double>>();
      const auto want_c = c["centers"].get<std::vector<double>>();
      INFO(tag(h, w) << " n" << n << " px " << px);
      REQUIRE(spec.energy.size() == want_e.size());
      for (size_t i = 0; i < want_e.size(); ++i) {
         INFO("i=" << i << " got " << spec.energy[i] << " want " << want_e[i]);
         REQUIRE(bits(spec.energy[i]) == bits(want_e[i]));   // np.sum's pairwise order, reproduced
         REQUIRE(bits(spec.centers_arcsec[i]) == bits(want_c[i]));
      }
      // energy_spectrum(image, ...) is the same numbers from one decomposition
      const auto img = plane_from(ac::test::load_npy(golden("starlet/img_" + tag(h, w) + ".npy")));
      const auto direct = ac::energy_spectrum(img, px, n);
      REQUIRE(direct.energy.size() == spec.energy.size());
      for (size_t i = 0; i < want_e.size(); ++i)
         REQUIRE(std::fabs(direct.energy[i] - want_e[i]) <= 1e-9 * want_e[i]);
   }
   REQUIRE(beyond_bufsize);
}

TEST_CASE("energy_centers_are_2pow_i_times_scale")
{
   std::vector<ac::Plane> planes(9, ac::Plane(3, 2));
   for (double px : {1.0, 0.05, 2.5, 1.01, 0.123456789, 7.3e-3}) {
      const auto spec = ac::energy_spectrum_from_planes(planes, px);
      REQUIRE(spec.centers_arcsec.size() == 9);
      for (size_t i = 0; i < 9; ++i) {
         CHECK(bits(spec.centers_arcsec[i]) == bits(std::ldexp(1.0, static_cast<int>(i)) * px));
         CHECK(spec.energy[i] == 0.0);
      }
   }
   CHECK(ac::energy_spectrum_from_planes({}, 1.0).centers_arcsec.empty());
   std::vector<ac::Plane> ragged{ac::Plane(3, 2), ac::Plane(2, 3)};
   CHECK_THROWS_AS(ac::energy_spectrum_from_planes(ragged, 1.0), std::invalid_argument);
}

TEST_CASE("energy_thread_invariant")
{
   const auto st = ac::starlet_transform(random_plane(301, 211, 3), 6, 1);
   const auto ref = ac::energy_spectrum_from_planes(st.planes, 1.0, 1);
   for (unsigned t : {2u, 7u, 32u}) {
      const auto got = ac::energy_spectrum_from_planes(st.planes, 1.0, t);
      REQUIRE(same_bits(got.energy, ref.energy));
   }
}

TEST_CASE("starlet_1600px_7scales_under_300ms", "[perf]")
{
#ifndef NDEBUG
   SKIP("perf budget applies to Release builds only");
#else
   // the real proxy shape: a 1600 px long edge at 3:2
   const auto in = random_plane(1600, 1067, 11);
   double best = 1e300;
   for (int rep = 0; rep < 3; ++rep) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto st = ac::starlet_transform(in, 7, 1);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      REQUIRE(st.planes.size() == 7);
      best = std::min(best, ms);
   }
   std::printf("[perf] starlet 1600x1067, 7 scales, 1 thread: best of 3 = %.1f ms\n", best);
   const auto t0 = std::chrono::steady_clock::now();
   const auto st = ac::starlet_transform(in, 7, 0);
   std::printf("[perf] starlet 1600x1067, 7 scales, %u threads: %.1f ms\n", ac::default_threads(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
   REQUIRE(best < 300.0);
#endif
}
