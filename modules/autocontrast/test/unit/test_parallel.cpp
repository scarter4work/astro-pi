// AutoContrast: ac::parallel_for and ac::deterministic_sum.
#include <catch_amalgamated.hpp>

#include "core/Parallel.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
// Terms whose floating-point sum is order-sensitive: wide dynamic range, mixed sign.
double term(size_t i)
{
   const double x = static_cast<double>(i) + 1.0;
   return std::sin(x) * std::pow(10.0, static_cast<double>(i % 17) - 8.0) + 1.0 / x;
}
uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; }
}

TEST_CASE("deterministic_sum is bit-identical with 1, 3 and 32 threads", "[parallel]")
{
   const size_t n = 1000000;
   const double s1  = ac::deterministic_sum(n, term, 1);
   const double s3  = ac::deterministic_sum(n, term, 3);
   const double s32 = ac::deterministic_sum(n, term, 32);
   const double sd  = ac::deterministic_sum(n, term);           // default thread count
   CHECK(bits(s1) == bits(s3));
   CHECK(bits(s1) == bits(s32));
   CHECK(bits(s1) == bits(sd));
   // Chunk order is fixed: the sum equals the serial chunked sum written out by hand.
   double ref = 0.0;
   for (size_t c0 = 0; c0 < n; c0 += 4096) {
      double part = 0.0;
      for (size_t i = c0; i < std::min(n, c0 + 4096); ++i) part += term(i);
      ref += part;
   }
   CHECK(bits(s1) == bits(ref));
}

TEST_CASE("deterministic_sum edge sizes", "[parallel]")
{
   CHECK(ac::deterministic_sum(0, term, 4) == 0.0);
   CHECK(bits(ac::deterministic_sum(1, term, 4)) == bits(term(0)));
   CHECK(bits(ac::deterministic_sum(4096, term, 5)) == bits(ac::deterministic_sum(4096, term, 1)));
   CHECK(bits(ac::deterministic_sum(4097, term, 5)) == bits(ac::deterministic_sum(4097, term, 1)));
}

TEST_CASE("parallel_for visits every index exactly once", "[parallel]")
{
   const size_t n = 100003;
   std::vector<std::atomic<int>> hits(n);
   for (auto& h : hits) h = 0;
   ac::parallel_for(n, [&](size_t i) { hits[i].fetch_add(1); }, 7);
   size_t bad = 0;
   for (auto& h : hits) bad += (h.load() != 1);
   CHECK(bad == 0);
   ac::parallel_for(0, [&](size_t) { FAIL("body called for n == 0"); });
}

TEST_CASE("parallel_for propagates an exception to the caller", "[parallel]")
{
   for (unsigned threads : {1u, 4u}) {
      CHECK_THROWS_AS(ac::parallel_for(50000, [](size_t i) {
         if (i == 31337) throw std::runtime_error("boom");
      }, threads), std::runtime_error);
   }
   try {
      ac::parallel_for(1000, [](size_t i) { if (i == 5) throw std::runtime_error("boom 5"); }, 3);
      FAIL("no exception");
   } catch (const std::runtime_error& e) {
      CHECK(std::string(e.what()) == "boom 5");
   }
}
