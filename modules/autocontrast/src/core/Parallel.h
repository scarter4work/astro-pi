// AutoContrast core (PCL-free): threading primitives.
// No OpenMP (libgomp is not on stock distributions): std::thread with dynamic chunk claiming.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ac {

// Worker count used when a caller passes 0.
inline unsigned default_threads()
{
   const unsigned n = std::thread::hardware_concurrency();
   return n == 0 ? 1u : n;
}

// Runs body(i) for every i in [0, n), exactly once, on up to `threads` threads (0 = default_threads()).
// Indices are claimed in small dynamic chunks. The first exception thrown by any body is rethrown on
// the calling thread after every worker has stopped; remaining chunks are abandoned.
template <class F>
void parallel_for(size_t n, F&& body, unsigned threads = 0)
{
   if (n == 0)
      return;
   if (threads == 0)
      threads = default_threads();
   const size_t chunk = std::max<size_t>(1, std::min<size_t>(256, n / (static_cast<size_t>(threads) * 8 + 1)));
   const size_t n_chunks = (n + chunk - 1) / chunk;
   const unsigned workers = static_cast<unsigned>(std::min<size_t>(threads, n_chunks));

   std::atomic<size_t> next{0};
   std::atomic<bool> failed{false};
   std::exception_ptr error;
   std::mutex error_mutex;

   auto work = [&]() {
      for (;;) {
         if (failed.load(std::memory_order_relaxed))
            return;
         const size_t c = next.fetch_add(1, std::memory_order_relaxed);
         if (c >= n_chunks)
            return;
         const size_t lo = c * chunk, hi = std::min(n, lo + chunk);
         try {
            for (size_t i = lo; i < hi; ++i)
               body(i);
         } catch (...) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!error)
               error = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
            return;
         }
      }
   };

   if (workers <= 1) {
      work();
   } else {
      std::vector<std::thread> pool;
      pool.reserve(workers - 1);
      try {
         for (unsigned t = 1; t < workers; ++t)
            pool.emplace_back(work);
      } catch (...) {                       // could not start a thread: finish on the threads we have
         failed.store(false);
      }
      work();
      for (auto& th : pool)
         th.join();
   }
   if (error)
      std::rethrow_exception(error);
}

// Sum of term(0..n-1), bit-identical for every thread count: the terms are summed serially in fixed
// 4096-element chunks (each chunk from 0.0, in index order) and the chunk partials are then combined
// in chunk order. `threads` (0 = default) only changes who computes which chunk.
template <class F>
double deterministic_sum(size_t n, F&& term, unsigned threads = 0)
{
   constexpr size_t kChunk = 4096;
   if (n == 0)
      return 0.0;
   const size_t n_chunks = (n + kChunk - 1) / kChunk;
   std::vector<double> partial(n_chunks, 0.0);
   parallel_for(n_chunks, [&](size_t c) {
      const size_t lo = c * kChunk, hi = std::min(n, lo + kChunk);
      double s = 0.0;
      for (size_t i = lo; i < hi; ++i)
         s += term(i);
      partial[c] = s;
   }, threads);
   double total = 0.0;
   for (size_t c = 0; c < n_chunks; ++c)
      total += partial[c];
   return total;
}

} // namespace ac
