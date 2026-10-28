#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Debayer.h"
#include "StackTypes.h"

namespace planetary::stack {

// ---- 1. Lunar disk and sky (spec Part B, step 1) ----
struct LunarMasks {
    int width = 0, height = 0;
    std::vector<uint8_t> disk, sky;   // 1 = member; disjoint; neither = limb transition / unsure
    size_t diskCount = 0, skyCount = 0;
};
LunarMasks SegmentLunar(const ser::FloatImage& rgb, int threads);

}  // namespace planetary::stack
