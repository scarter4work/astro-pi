// AutoContrast: exact optimal-transport EMD (network simplex). Correctness is proven by a dual certificate on
// every solve (primal feasibility, dual feasibility, complementary slackness, zero duality gap), and parity
// with scipy.stats.wasserstein_distance_nd (HiGHS) on the 20 golden chroma pairs (synthetic/distance).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/fingerprint/Emd.h"
#include "core/num/Histogram.h"
#include "core/num/Sum.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

// The 16x16 chroma grid of fingerprint/distance.py: bin centres of np.linspace(-100, 100, 17), meshgrid
// indexing="ij" raveled (row-major a-bin * 16 + b-bin), Euclidean ground cost (scipy cdist).
std::vector<double> grid_cost(int bins = 16, double extent = 100.0)
{
   const auto e = ac::linspace(-extent, extent, bins + 1);
   std::vector<double> c(bins);
   for (int k = 0; k < bins; ++k)
      c[k] = 0.5 * (e[k] + e[k + 1]);
   const int n = bins * bins;
   std::vector<double> cost(size_t(n) * n);
   for (int p = 0; p < n; ++p)
      for (int q = 0; q < n; ++q) {
         const double d0 = c[p / bins] - c[q / bins], d1 = c[p % bins] - c[q % bins];
         double s = 0.0;
         s += d0 * d0;
         s += d1 * d1;
         cost[size_t(p) * n + q] = std::sqrt(s);
      }
   return cost;
}

std::vector<double> normalised(const std::vector<double>& w)
{
   const double s = ac::pairwise_sum(w.data(), w.size());
   std::vector<double> out(w.size());
   for (size_t i = 0; i < w.size(); ++i)
      out[i] = w[i] / s;
   return out;
}

struct CertStats {
   double worst_primal = 0.0, worst_reduced = 0.0, worst_basic = 0.0, worst_gap = 0.0;
   long problems = 0, pivots = 0, degenerate = 0;
};
CertStats g_cert;

// The dual certificate: with a = supply / sum, b = demand / sum (scipy's normalisation),
//   primal:  x >= 0 on the reported arcs, row sums = a and column sums = b             (<= 1e-12)
//   dual:    c_ij - u_i - v_j >= -1e-12 on EVERY arc
//   slack:   c_ij - u_i - v_j = 0 +- 1e-12 on every basic arc
//   gap:     cost = sum u_i a_i + sum v_j b_j                                         (<= 1e-12)
// Together these prove `cost` is the optimum of the transportation LP (weak duality), independently of scipy.
void check_certificate(const std::vector<double>& supply, const std::vector<double>& demand,
                       const std::vector<double>& cost, const ac::EmdResult& r)
{
   const size_t n = supply.size(), m = demand.size();
   const auto a = normalised(supply), b = normalised(demand);
   REQUIRE(r.u.size() == n);
   REQUIRE(r.v.size() == m);
   REQUIRE(std::isfinite(r.cost));

   std::vector<double> row(n, 0.0), col(m, 0.0);
   double primal_cost = 0.0, worst_basic = 0.0;
   for (const auto& [i, j, x] : r.flows) {
      REQUIRE(i >= 0);
      REQUIRE(size_t(i) < n);
      REQUIRE(j >= 0);
      REQUIRE(size_t(j) < m);
      REQUIRE(x >= -1e-12);
      row[i] += x;
      col[j] += x;
      primal_cost += cost[size_t(i) * m + j] * x;
      const double rc = cost[size_t(i) * m + j] - r.u[i] - r.v[j];
      worst_basic = std::max(worst_basic, std::abs(rc));
   }
   double worst_primal = 0.0;
   for (size_t i = 0; i < n; ++i)
      worst_primal = std::max(worst_primal, std::abs(row[i] - a[i]));
   for (size_t j = 0; j < m; ++j)
      worst_primal = std::max(worst_primal, std::abs(col[j] - b[j]));

   double worst_reduced = 0.0;   // most negative reduced cost, as a positive number
   for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < m; ++j)
         worst_reduced = std::max(worst_reduced, -(cost[i * m + j] - r.u[i] - r.v[j]));

   double dual = 0.0;
   for (size_t i = 0; i < n; ++i)
      dual += r.u[i] * a[i];
   for (size_t j = 0; j < m; ++j)
      dual += r.v[j] * b[j];
   const double gap = std::abs(r.cost - dual);

   INFO("n=" << n << " m=" << m << " primal=" << worst_primal << " reduced=" << worst_reduced
             << " basic=" << worst_basic << " gap=" << gap << " cost=" << r.cost << " dual=" << dual);
   REQUIRE(worst_primal <= 1e-12);
   REQUIRE(worst_reduced <= 1e-12);
   REQUIRE(worst_basic <= 1e-12);
   REQUIRE(gap <= 1e-12);
   REQUIRE(std::abs(primal_cost - r.cost) <= 1e-12);

   g_cert.worst_primal = std::max(g_cert.worst_primal, worst_primal);
   g_cert.worst_reduced = std::max(g_cert.worst_reduced, worst_reduced);
   g_cert.worst_basic = std::max(g_cert.worst_basic, worst_basic);
   g_cert.worst_gap = std::max(g_cert.worst_gap, gap);
   ++g_cert.problems;
   g_cert.pivots += r.pivots;
   g_cert.degenerate += r.degenerate_pivots;
}

std::vector<double> random_masses(std::mt19937_64& rng, size_t n, double zero_fraction, bool integral)
{
   std::uniform_real_distribution<double> U(0.0, 1.0);
   std::uniform_int_distribution<int> K(1, 5);
   std::vector<double> w(n);
   for (auto& x : w)
      x = U(rng) < zero_fraction ? 0.0 : (integral ? double(K(rng)) : U(rng));
   if (ac::pairwise_sum(w.data(), w.size()) == 0.0)
      w[std::uniform_int_distribution<size_t>(0, n - 1)(rng)] = 1.0;
   return w;
}

} // namespace

TEST_CASE("emd_dual_certificate: 200 random sparse/dense problems", "[emd]")
{
   std::mt19937_64 rng(20261112);
   std::uniform_real_distribution<double> U(0.0, 1.0);
   const auto grid = grid_cost();
   for (int t = 0; t < 200; ++t) {
      const int kind = t % 5;
      const double zf = (t / 5) % 2 ? 0.9 : (t % 3 == 0 ? 0.0 : 0.4);   // sparse, dense, mixed
      std::vector<double> supply, demand, cost;
      if (kind <= 1) {
         // the production shape: 16x16 chroma grid, 256 + 256 nodes including zero-mass bins
         supply = random_masses(rng, 256, zf, kind == 1);
         demand = random_masses(rng, 256, zf, kind == 1);
         cost = grid;
      } else {
         const size_t n = 1 + rng() % 40, m = 1 + rng() % 40;
         supply = random_masses(rng, n, zf, kind == 3);
         demand = random_masses(rng, m, zf, kind == 3);
         cost.resize(n * m);
         for (auto& c : cost)
            // kind 2: continuous costs; 3: small integer costs (ties everywhere, maximal degeneracy);
            // 4: continuous costs with an occasional zero
            c = kind == 3 ? double(rng() % 4) : (kind == 4 && U(rng) < 0.2 ? 0.0 : 100.0 * U(rng));
      }
      INFO("problem " << t << " kind " << kind);
      const auto r = ac::emd_exact(supply, demand, cost);
      check_certificate(supply, demand, cost, r);
   }
   std::printf("[emd] certificate over %ld problems: worst primal %.3g, worst negative reduced cost %.3g, "
               "worst basic |rc| %.3g, worst duality gap %.3g; %ld pivots (%ld degenerate)\n",
               g_cert.problems, g_cert.worst_primal, g_cert.worst_reduced, g_cert.worst_basic, g_cert.worst_gap,
               g_cert.pivots, g_cert.degenerate);
}

namespace {

// scipy.stats.wasserstein_distance_nd solves the DUAL transportation LP through scipy.optimize.milp at HiGHS's
// default 1e-7 tolerances; its value can sit below the optimum. Measured on the 20 golden pairs (scipy 1.18.1):
// pair 2 is 5.59e-7 low (1.97e-9 after /diagonal), pair 3 2.61e-6 low (9.24e-9 after /diagonal), pair 12
// 1.56e-8; the other 15 agree to <= 1e-12. The exact value is proven by the dual certificate and matches HiGHS
// on the PRIMAL LP at 1e-10 tolerances (emd_exact_raw.npy) to ~1e-14, so the golden, not the solver, is inexact.
constexpr double kScipyRawTol = 5e-6;   // ~2x the worst measured gap (pair 3)

struct GoldenPair {
   std::vector<double> h0, h1;
   bool neutral = false;
};

std::vector<GoldenPair> golden_pairs()
{
   const auto pairs = ac::test::load_npy(golden("distance/emd_pairs.npy"));
   REQUIRE(pairs.shape == std::vector<size_t>{20, 2, 16, 16});
   std::vector<GoldenPair> out(20);
   for (size_t p = 0; p < 20; ++p) {
      out[p].h0.assign(pairs.data.begin() + p * 512, pairs.data.begin() + p * 512 + 256);
      out[p].h1.assign(pairs.data.begin() + p * 512 + 256, pairs.data.begin() + p * 512 + 512);
      out[p].neutral = ac::pairwise_sum(out[p].h0.data(), 256) == 0.0 || ac::pairwise_sum(out[p].h1.data(), 256) == 0.0;
   }
   return out;
}

} // namespace

TEST_CASE("emd_matches_exact_reference: 20 golden pairs vs HiGHS primal at 1e-10", "[emd]")
{
   const auto gp = golden_pairs();
   const auto raw = ac::test::load_npy(golden("distance/emd_exact_raw.npy"));
   const auto norm = ac::test::load_npy(golden("distance/emd_exact_normalized.npy"));
   REQUIRE(raw.size() == 20);
   REQUIRE(norm.size() == 20);
   const auto cost = grid_cost();
   const double diagonal = std::hypot(200.0, 200.0);
   double worst_raw = 0.0, worst_norm = 0.0;
   for (size_t p = 0; p < 20; ++p) {
      INFO("pair " << p);
      if (gp[p].neutral) {
         REQUIRE(raw.data[p] == 0.0);   // the neutral pairs: Python never solves them
         REQUIRE(norm.data[p] == 0.0);
         REQUIRE_THROWS_AS(ac::emd_exact(gp[p].h0, gp[p].h1, cost), std::invalid_argument);
         continue;
      }
      const auto r = ac::emd_exact(gp[p].h0, gp[p].h1, cost);
      check_certificate(gp[p].h0, gp[p].h1, cost, r);
      const double dr = std::abs(r.cost - raw.data[p]), dn = std::abs(r.cost / diagonal - norm.data[p]);
      INFO("C++ " << r.cost << " reference " << raw.data[p]);
      REQUIRE(dr <= 1e-12);
      REQUIRE(dn <= 1e-12);
      worst_raw = std::max(worst_raw, dr);
      worst_norm = std::max(worst_norm, dn);
   }
   std::printf("[emd] vs exact reference: max |delta| raw %.3g, normalised %.3g\n", worst_raw, worst_norm);
}

TEST_CASE("emd_matches_scipy: 20 golden pairs at scipy's own tolerance", "[emd]")
{
   const auto gp = golden_pairs();
   const auto raw = ac::test::load_npy(golden("distance/emd_raw.npy"));
   REQUIRE(raw.size() == 20);
   const auto cost = grid_cost();
   const double diagonal = std::hypot(200.0, 200.0);
   double worst = 0.0;
   for (size_t p = 0; p < 20; ++p) {
      if (gp[p].neutral) {
         REQUIRE(raw.data[p] == 0.0);
         continue;
      }
      const double c = ac::emd_exact(gp[p].h0, gp[p].h1, cost).cost;
      const double d = std::abs(c - raw.data[p]);
      if (d > 1e-12)
         std::printf("[emd] pair %zu: exact %.17g, scipy %.17g, |delta| raw %.3g, after /diagonal %.3g\n", p, c,
                     raw.data[p], d, d / diagonal);
      INFO("pair " << p << " exact " << c << " scipy " << raw.data[p]);
      REQUIRE(d <= kScipyRawTol);
      worst = std::max(worst, d);
   }
   std::printf("[emd] vs scipy (HiGHS dual, milp defaults): max |delta| raw %.3g\n", worst);
}


TEST_CASE("emd_degenerate_identical_histograms_zero", "[emd]")
{
   const auto cost = grid_cost();
   std::mt19937_64 rng(7);
   for (double zf : {0.0, 0.5, 0.95}) {
      const auto h = random_masses(rng, 256, zf, false);
      const auto r = ac::emd_exact(h, h, cost);
      check_certificate(h, h, cost, r);
      REQUIRE(std::abs(r.cost) <= 1e-12);
   }
   // every bin equal: the most degenerate case of all (every basic arc of a perfect matching)
   const std::vector<double> flat(256, 1.0);
   const auto r = ac::emd_exact(flat, flat, cost);
   check_certificate(flat, flat, cost, r);
   REQUIRE(std::abs(r.cost) <= 1e-12);
}

TEST_CASE("emd_single_bin_masses", "[emd]")
{
   const auto cost = grid_cost();
   for (const auto& [p, q] : std::vector<std::pair<int, int>>{{0, 255}, {17, 17}, {3, 200}, {255, 0}}) {
      std::vector<double> s(256, 0.0), d(256, 0.0);
      s[p] = 0.25;   // any positive mass: normalisation makes it 1
      d[q] = 7.0;
      const auto r = ac::emd_exact(s, d, cost);
      check_certificate(s, d, cost, r);
      REQUIRE(r.cost == Catch::Approx(cost[size_t(p) * 256 + q]).margin(1e-12));
   }
   // 1 x 1, 1 x m and n x 1 problems
   const auto r11 = ac::emd_exact({2.0}, {5.0}, {3.5});
   REQUIRE(r11.cost == 3.5);
   const std::vector<double> c13 = {4.0, 1.0, 2.0};
   const auto r13 = ac::emd_exact({1.0}, {1.0, 1.0, 2.0}, c13);
   check_certificate({1.0}, {1.0, 1.0, 2.0}, c13, r13);
   REQUIRE(r13.cost == Catch::Approx(0.25 * 4.0 + 0.25 * 1.0 + 0.5 * 2.0).margin(1e-12));
   const auto r31 = ac::emd_exact({1.0, 0.0, 3.0}, {1.0}, c13);
   check_certificate({1.0, 0.0, 3.0}, {1.0}, c13, r31);
   REQUIRE(r31.cost == Catch::Approx(0.25 * 4.0 + 0.75 * 2.0).margin(1e-12));
}

TEST_CASE("emd_exact rejects invalid input loudly", "[emd]")
{
   const std::vector<double> c4 = {0.0, 1.0, 1.0, 0.0};
   REQUIRE_THROWS_AS(ac::emd_exact({1.0, 1.0}, {1.0, 1.0}, {0.0, 1.0, 1.0}), std::invalid_argument);   // shape
   REQUIRE_THROWS_AS(ac::emd_exact({}, {1.0}, {}), std::invalid_argument);                             // empty
   REQUIRE_THROWS_AS(ac::emd_exact({0.0, 0.0}, {1.0, 1.0}, c4), std::invalid_argument);                // sum 0
   REQUIRE_THROWS_AS(ac::emd_exact({-1.0, 2.0}, {1.0, 1.0}, c4), std::invalid_argument);               // negative
   REQUIRE_THROWS_AS(ac::emd_exact({NAN, 2.0}, {1.0, 1.0}, c4), std::invalid_argument);                // NaN
   REQUIRE_THROWS_AS(ac::emd_exact({1.0, 1.0}, {1.0, 1.0}, {0.0, INFINITY, 1.0, 0.0}),
                     std::invalid_argument);                                                            // inf cost
}

TEST_CASE("emd_256x256_under_20ms", "[perf]")
{
#ifndef NDEBUG
   SKIP("perf budget applies to Release builds only");
#else
   const auto cost = grid_cost();
   std::mt19937_64 rng(99);
   double worst = 0.0, total = 0.0;
   long pivots = 0;
   const int reps = 10;
   for (int rep = 0; rep < reps; ++rep) {
      const auto s = random_masses(rng, 256, rep % 2 ? 0.0 : 0.3, false);
      const auto d = random_masses(rng, 256, rep % 2 ? 0.0 : 0.3, false);
      const auto t0 = std::chrono::steady_clock::now();
      const auto r = ac::emd_exact(s, d, cost);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      worst = std::max(worst, ms);
      total += ms;
      pivots += r.pivots;
   }
   std::printf("[perf] emd 256x256 (dense and 30%% zero bins): mean %.2f ms, worst %.2f ms, mean %ld pivots\n",
               total / reps, worst, pivots / reps);
   REQUIRE(worst < 20.0);
#endif
}
