#include <catch_amalgamated.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "Engine.h"
#include "Regions.h"
#include "Select.h"

// PlanRegions' heap per worker, measured (final-fix re-review Minor 2: planScratchBytes counted six arrays of
// 8 bytes per frame; the ranking holds more). A counting allocator replaces this binary's global operator
// new / delete -- its own executable, so no other test runs under it: every allocation carries its size in a
// header, the live total and its peak are kept. PlanRegions on one worker ranks one region at a time, so the
// peak over a call, less what the returned plan still holds, is one worker's scratch.

namespace {

std::atomic<size_t> gLive{0}, gPeak{0};
constexpr size_t kHeader = alignof(std::max_align_t);

void* Allocate(size_t n) {
    void* p = std::malloc(n + kHeader);
    if (!p) throw std::bad_alloc();
    *static_cast<size_t*>(p) = n;
    const size_t live = gLive.fetch_add(n) + n;
    size_t peak = gPeak.load();
    while (live > peak && !gPeak.compare_exchange_weak(peak, live)) {
    }
    return static_cast<char*>(p) + kHeader;
}

void Release(void* q) noexcept {
    if (!q) return;
    void* p = static_cast<char*>(q) - kHeader;
    gLive.fetch_sub(*static_cast<size_t*>(p));
    std::free(p);
}

}  // namespace

void* operator new(size_t n) { return Allocate(n); }
void* operator new[](size_t n) { return Allocate(n); }
void operator delete(void* p) noexcept { Release(p); }
void operator delete[](void* p) noexcept { Release(p); }
void operator delete(void* p, size_t) noexcept { Release(p); }
void operator delete[](void* p, size_t) noexcept { Release(p); }

using namespace planetary;

TEST_CASE("PlanRegionsWorkerBytes is what one PlanRegions worker allocates at its peak", "[engine][memory]") {
    // One region, every frame a candidate with detail over the noise (the gate passes) and a noisy quality (the
    // prior is fitted: its likelihood table is the largest array): the worst case per frame.
    stack::RegionLayout lay;
    lay.width = lay.height = 64;
    lay.size = 64;
    lay.step = 32;
    lay.nx = lay.ny = 1;
    lay.regions.push_back({0, 0, 64, 64, 0.01, 0, 0});
    lay.node.push_back(0);
    for (size_t frames : {1000u, 3000u, 4096u}) {
        std::mt19937 rng(5);
        std::normal_distribution<double> n(0, 1);
        std::vector<stack::FrameRegions> measured(frames);
        for (size_t k = 0; k < frames; ++k) {
            measured[k].frameIndex = static_cast<int64_t>(k);
            stack::RegionMeasure q;
            q.aligned = true;
            q.confidence = 0.95f;
            q.quality = static_cast<float>(0.3 * std::exp(0.3 * n(rng)));
            q.qualitySigma = 0.01f;
            q.quality2Sigma = 2 * q.quality * q.qualitySigma;
            measured[k].regions.push_back(q);
        }
        const size_t before = gLive.load();
        gPeak.store(before);
        stack::RegionPlan plan = stack::PlanRegions(lay, measured, 1);
        const size_t peak = gPeak.load(), after = gLive.load();
        REQUIRE(plan.detail.size() == 1);
        REQUIRE(plan.detail[0].candidates == static_cast<int64_t>(frames));
        REQUIRE(plan.detail[0].kept > 0);
        // The peak, less what the plan holds at it (all of it but `stacked`, filled after the ranking).
        const size_t scratch = peak - (after - plan.stacked.capacity());
        // One worker's scratch, and the frames' order PlanRegions holds once beside it.
        const size_t counted = stack::PlanRegionsWorkerBytes(frames) + stack::PlanRegionsOrderBytes(frames);
        INFO(frames << " frames: measured " << scratch << " bytes (" << static_cast<double>(scratch) / frames
                    << " per frame), counted " << counted << " (" << counted / frames << " per frame)");
        CHECK(stack::PlanRegionsWorkerBytes(frames) / frames == 664);
        CHECK(stack::PlanRegionsOrderBytes(frames) / frames == 8);
        // Counted, not guessed: what is left over does not grow with the frames (the worker thread's start-up and the
        // pool's own: 1113 bytes on the development machine at every count here).
        CHECK(scratch >= counted);
        CHECK(scratch - counted <= 2048);
        // PlanMemory plans with it.
        ser::FileInfo info;
        info.header.width = info.header.height = 64;
        info.colorId = ser::ColorId::Mono;
        info.normDepth = 16;
        info.planes = 1;
        info.bytesPerSample = 2;
        info.frameCount = static_cast<int64_t>(frames);
        CHECK(stack::PlanMemory(info, 1, 1).planScratchBytes == stack::PlanRegionsWorkerBytes(frames));
    }
}
