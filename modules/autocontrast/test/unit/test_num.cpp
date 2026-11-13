// AutoContrast: numpy/scipy-exact primitives vs the Python-generated goldens (synthetic/prim).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Image.h"
#include "core/num/Convolve.h"
#include "core/num/Gaussian.h"
#include "core/num/Histogram.h"
#include "core/num/Interp.h"
#include "core/num/Label.h"
#include "core/num/Percentile.h"
#include "core/num/Sum.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string prim(const std::string& name) { return std::string(AC_TEST_DIR) + "/golden/synthetic/prim/" + name; }
std::vector<double> npy(const std::string& name) { return ac::test::load_npy(prim(name)).data; }

uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; }

double max_abs_diff(const std::vector<double>& a, const std::vector<double>& b)
{
   REQUIRE(a.size() == b.size());
   double m = 0.0;
   for (size_t i = 0; i < a.size(); ++i)
      m = std::max(m, std::fabs(a[i] - b[i]));
   return m;
}

double max_rel_diff(const std::vector<double>& a, const std::vector<double>& b)
{
   REQUIRE(a.size() == b.size());
   double m = 0.0;
   for (size_t i = 0; i < a.size(); ++i)
      m = std::max(m, std::fabs(a[i] - b[i]) / std::max(1.0, std::fabs(b[i])));
   return m;
}

void report(const char* what, double d) { std::printf("[max|d|] %-44s %.3g\n", what, d); }

ac::Plane plane_from(const ac::test::NpyArray& a)
{
   REQUIRE(a.shape.size() == 2);
   ac::Plane p(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]));
   p.data = a.data;
   return p;
}

} // namespace

TEST_CASE("percentile_matches_numpy_bitwise")
{
   const auto levels = npy("percentile_levels.npy");
   for (const char* name : {"p1001", "p1000", "pties", "pn2", "pn5", "pn1", "img"}) {
      const auto in = npy(std::string("percentile_") + name + "_in.npy");
      const auto want = npy(std::string("percentile_") + name + "_out.npy");
      REQUIRE(want.size() == levels.size());
      const auto got = ac::percentiles(in, levels);
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < want.size(); ++i) {
         INFO(name << " q=" << levels[i]);
         REQUIRE(bits(got[i]) == bits(want[i]));
         REQUIRE(bits(ac::percentile(in, levels[i])) == bits(want[i]));
      }
   }
   const auto img = npy("percentile_img_in.npy");
   REQUIRE(bits(ac::percentile(img, 25.0)) == bits(npy("percentile_img25_out.npy")[0]));
}

TEST_CASE("percentile_edge_cases")
{
   CHECK_THROWS(ac::percentile({}, 50.0));
   CHECK_THROWS(ac::percentile({1.0}, -1.0));
   CHECK_THROWS(ac::percentile({1.0}, 100.5));
   CHECK(std::isnan(ac::percentile({1.0, std::numeric_limits<double>::quiet_NaN(), 3.0}, 50.0)));
}

TEST_CASE("median_even_odd")
{
   for (const char* name : {"p1001", "p1000", "pties", "pn2", "pn5", "pn1"}) {
      const auto in = npy(std::string("percentile_") + name + "_in.npy");
      const double want = npy(std::string("median_") + name + "_out.npy")[0];
      INFO(name);
      REQUIRE(bits(ac::median(in)) == bits(want));
   }
   CHECK(ac::median({3.0, 1.0, 2.0}) == 2.0);
   CHECK(ac::median({4.0, 1.0, 2.0, 3.0}) == 2.5);
   CHECK_THROWS(ac::median({}));
}

TEST_CASE("interp_clamps")
{
   const auto xp = npy("interp_xp.npy"), fp = npy("interp_fp.npy"), x = npy("interp_x.npy"),
              want = npy("interp_out.npy");
   REQUIRE(x.size() == want.size());
   for (size_t i = 0; i < x.size(); ++i) {
      INFO("i=" << i << " x=" << x[i]);
      REQUIRE(bits(ac::interp(x[i], xp, fp)) == bits(want[i]));
   }
   const auto lxp = npy("interp_log_xp.npy"), lfp = npy("interp_log_fp.npy"), lx = npy("interp_log_x.npy"),
              lwant = npy("interp_log_out.npy");
   for (size_t i = 0; i < lx.size(); ++i)
      REQUIRE(bits(ac::interp(lx[i], lxp, lfp)) == bits(lwant[i]));
   // explicit clamping
   CHECK(ac::interp(-1e9, xp, fp) == fp.front());
   CHECK(ac::interp(1e9, xp, fp) == fp.back());
   CHECK(std::isnan(ac::interp(std::numeric_limits<double>::quiet_NaN(), xp, fp)));
}

TEST_CASE("histogram_edge_values_match_numpy")
{
   const auto vals = npy("hist_vals.npy"), w = npy("hist_weights.npy");
   const auto c = npy("hist_int_counts.npy"), cw = npy("hist_int_weighted.npy");
   const auto c_edges = npy("hist_edges_arr_counts.npy");
   REQUIRE(c.size() == 360);
   const auto got = ac::histogram_uniform(vals.data(), nullptr, vals.size(), 360, 0.0, 360.0);
   REQUIRE(got.size() == 360);
   for (size_t i = 0; i < 360; ++i) { INFO("bin " << i); REQUIRE(got[i] == c[i]); }
   // numpy's explicit-edges (searchsorted) path agrees with the uniform path on these edge probes
   for (size_t i = 0; i < 360; ++i) { INFO("bin " << i); REQUIRE(got[i] == c_edges[i]); }
   const auto gw = ac::histogram_uniform(vals.data(), w.data(), vals.size(), 360, 0.0, 360.0);
   for (size_t i = 0; i < 360; ++i) { INFO("wbin " << i); REQUIRE(bits(gw[i]) == bits(cw[i])); }

   const auto v16 = npy("hist16_vals.npy");
   const auto g16 = ac::histogram_uniform(v16.data(), nullptr, v16.size(), 16, -100.0, 100.0);
   const auto w16 = npy("hist16_int_counts.npy");
   for (size_t i = 0; i < 16; ++i) { INFO("bin16 " << i); REQUIRE(g16[i] == w16[i]); }
   const auto e16 = npy("hist16_edges_arr_counts.npy");
   for (size_t i = 0; i < 16; ++i) { INFO("bin16e " << i); REQUIRE(g16[i] == e16[i]); }
}

TEST_CASE("histogram_drops_nan_and_out_of_range")
{
   const double nan = std::numeric_limits<double>::quiet_NaN();
   const std::vector<double> v{nan, -0.1, 0.0, 0.5, 1.0, 1.1, std::numeric_limits<double>::infinity()};
   const auto h = ac::histogram_uniform(v.data(), nullptr, v.size(), 2, 0.0, 1.0);
   CHECK(h[0] == 1.0); // 0.0
   CHECK(h[1] == 2.0); // 0.5 and 1.0 (last edge inclusive)
}

TEST_CASE("histogram_multi_block_matches_numpy_block_order")
{
   // > 65536 elements: numpy accumulates bincount per 65536-block; the weighted sums must follow that order.
   // Reference computed independently here with the same block structure semantics (self-consistency vs a
   // straightforward serial block loop using exact-edge membership).
   const size_t n = 200000;
   std::vector<double> x(n), w(n);
   for (size_t i = 0; i < n; ++i) {
      x[i] = std::fmod(static_cast<double>(i) * 0.6180339887498949, 1.0);
      w[i] = 1e-3 * static_cast<double>((i * 2654435761u) % 1000) + 1e-9 * static_cast<double>(i % 7);
   }
   const int bins = 37;
   const auto got = ac::histogram_uniform(x.data(), w.data(), n, bins, 0.0, 1.0);
   std::vector<double> total(bins, 0.0);
   for (size_t b0 = 0; b0 < n; b0 += 65536) {
      std::vector<double> blk(bins, 0.0);
      for (size_t i = b0; i < std::min(n, b0 + 65536); ++i) {
         int k = std::min(bins - 1, static_cast<int>(x[i] * bins));
         // exact-edge correction against linspace edges
         auto edge = [&](int j) { return j == bins ? 1.0 : static_cast<double>(j) * (1.0 / bins); };
         while (k > 0 && x[i] < edge(k)) --k;
         while (k < bins - 1 && x[i] >= edge(k + 1)) ++k;
         blk[k] += w[i];
      }
      for (int k = 0; k < bins; ++k) total[k] += blk[k];
   }
   report("histogram multi-block weighted", max_abs_diff(got, total));
   REQUIRE(max_abs_diff(got, total) <= 1e-9);
}

TEST_CASE("histogram2d_matches_numpy")
{
   const auto a = npy("hist2d_a.npy"), b = npy("hist2d_b.npy");
   const auto want_w = npy("hist2d_weighted.npy"), want_u = npy("hist2d_unweighted.npy");
   ac::Plane pa(static_cast<int>(a.size()), 1), pb(static_cast<int>(a.size()), 1), pw(static_cast<int>(a.size()), 1);
   pa.data = a; pb.data = b;
   for (size_t i = 0; i < a.size(); ++i) pw.data[i] = std::hypot(a[i], b[i]);
   const auto gw = ac::histogram2d(pa, pb, pw, 16, 100.0);
   REQUIRE(gw.size() == 256);
   const double rel = max_rel_diff(gw, want_w);
   report("histogram2d weighted (rel)", rel);
   REQUIRE(rel <= 1e-12);
   const auto gu = ac::histogram2d(pa, pb, ac::Plane(), 16, 100.0);   // empty weight plane: counts
   REQUIRE(max_abs_diff(gu, want_u) == 0.0);
   // bit-exact: same sequential bincount order as numpy
   for (size_t i = 0; i < 256; ++i) REQUIRE(bits(gw[i]) == bits(want_w[i]));
}

TEST_CASE("convolve1d_mirror_kernel_wider_than_axis")
{
   double worst = 0.0;
   for (auto hw : {std::pair<int, int>{7, 5}, std::pair<int, int>{64, 48}}) {
      const int h = hw.first, w = hw.second;
      const std::string tag = std::to_string(h) + "x" + std::to_string(w);
      const auto in = plane_from(ac::test::load_npy(prim("conv_in_" + tag + ".npy")));
      for (const char* k : {"5", "9", "17", "257", "asym9"}) {
         const auto kernel = npy(std::string("conv_kernel_") + k + ".npy");
         for (int axis : {0, 1}) {
            const auto want = npy("conv_" + tag + "_k" + k + "_axis" + std::to_string(axis) + ".npy");
            const auto got = ac::convolve1d_mirror(in, kernel, axis);
            REQUIRE(got.width == w);
            REQUIRE(got.height == h);
            const double d = max_abs_diff(got.data, want);
            worst = std::max(worst, d);
            INFO(tag << " k" << k << " axis" << axis);
            REQUIRE(d <= 1e-14);
         }
      }
   }
   report("convolve1d_mirror (worst)", worst);
}

TEST_CASE("gaussian_matches_scipy")
{
   double worst = 0.0;
   for (auto hw : {std::pair<int, int>{7, 5}, std::pair<int, int>{64, 48}}) {
      const std::string tag = std::to_string(hw.first) + "x" + std::to_string(hw.second);
      const auto in = plane_from(ac::test::load_npy(prim("conv_in_" + tag + ".npy")));
      for (const char* s : {"0.5", "1.0", "2.355", "3.0", "8.0"}) {
         const auto want = npy("gauss_" + tag + "_s" + s + ".npy");
         const auto got = ac::gaussian_filter(in, std::stod(s));
         const double d = max_abs_diff(got.data, want);
         worst = std::max(worst, d);
         INFO(tag << " sigma " << s);
         REQUIRE(d <= 1e-12);
      }
   }
   report("gaussian_filter (worst)", worst);
}

TEST_CASE("label4_matches_scipy")
{
   for (const char* name : {"dense", "sparse", "tiny"}) {
      const auto m = ac::test::load_npy(prim(std::string("label_") + name + "_mask.npy"));
      const int h = static_cast<int>(m.shape[0]), w = static_cast<int>(m.shape[1]);
      std::vector<uint8_t> mask(m.data.size());
      for (size_t i = 0; i < mask.size(); ++i) mask[i] = m.data[i] != 0.0 ? 1 : 0;
      const auto want_lab = npy(std::string("label_") + name + "_labels.npy");
      const auto want_box = npy(std::string("label_") + name + "_boxes.npy");
      const auto meta = ac::test::load_json(prim(std::string("label_") + name + ".json"));
      const auto got = ac::label4(mask, w, h);
      INFO(name);
      REQUIRE(got.count == meta["n"].get<int>());
      REQUIRE(got.label.size() == want_lab.size());
      for (size_t i = 0; i < want_lab.size(); ++i) REQUIRE(static_cast<double>(got.label[i]) == want_lab[i]);
      REQUIRE(got.boxes.size() * 4 == want_box.size());
      for (size_t i = 0; i < got.boxes.size(); ++i) {
         REQUIRE(got.boxes[i].y0 == want_box[4 * i + 0]);
         REQUIRE(got.boxes[i].y1 == want_box[4 * i + 1]);
         REQUIRE(got.boxes[i].x0 == want_box[4 * i + 2]);
         REQUIRE(got.boxes[i].x1 == want_box[4 * i + 3]);
      }
   }
   const auto none = ac::label4(std::vector<uint8_t>(6, 0), 3, 2);
   CHECK(none.count == 0);
   CHECK(none.boxes.empty());
}

TEST_CASE("reductions_thread_invariant")
{
   // a larger plane so several chunks exist; compare bitwise across thread counts
   ac::Plane in(300, 200);
   for (size_t i = 0; i < in.data.size(); ++i)
      in.data[i] = std::sin(0.37 * static_cast<double>(i)) + 1e-3 * static_cast<double>(i % 11);
   const auto kernel = npy("conv_kernel_17.npy");
   for (int axis : {0, 1}) {
      const auto ref = ac::convolve1d_mirror(in, kernel, axis, 1);
      for (unsigned t : {3u, 32u}) {
         const auto got = ac::convolve1d_mirror(in, kernel, axis, t);
         REQUIRE(got.data.size() == ref.data.size());
         REQUIRE(std::memcmp(got.data.data(), ref.data.data(), ref.data.size() * sizeof(double)) == 0);
      }
   }
   const auto gref = ac::gaussian_filter(in, 2.355, 1);
   for (unsigned t : {3u, 32u}) {
      const auto got = ac::gaussian_filter(in, 2.355, t);
      REQUIRE(std::memcmp(got.data.data(), gref.data.data(), gref.data.size() * sizeof(double)) == 0);
   }
}

TEST_CASE("pairwise_sum_matches_np_sum_bitwise")
{
   // every length 1..300 (all pairwise branch edges: <8, 8..128, recursion) plus long ones around 8192
   const auto vals = npy("../np_sum/values.npy"), offs = npy("../np_sum/offsets.npy"), sums = npy("../np_sum/sums.npy");
   REQUIRE(offs.size() == sums.size() + 1);
   for (size_t k = 0; k < sums.size(); ++k) {
      const size_t lo = static_cast<size_t>(offs[k]), n = static_cast<size_t>(offs[k + 1]) - lo;
      INFO("n=" << n);
      REQUIRE(bits(ac::pairwise_sum(vals.data() + lo, n)) == bits(sums[k]));
   }
}
