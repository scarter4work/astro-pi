#include "Demosaic.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "BayerPattern.h"
#include "SerFile.h"

namespace planetary::stack {

using ser::ColorId;
using ser::FloatImage;
using ser::Frame;
using ser::SerError;

namespace {
// Throws SerError for every case LumaForMeasure documents (depth, size, channels, Bayer
// shape); otherwise returns 2^depth - 1, the value samples are divided by.
float CheckFrame(const Frame& f, ColorId color, int depth) {
    const float maxValue = ser::MaxSampleValue(depth);
    ser::CheckFrameSize(f);
    if (ser::IsBayer(color)) {
        if (f.channels != 1)
            throw SerError(std::string(ser::ColorIdName(color)) + " needs a 1-channel CFA frame, got " +
                           std::to_string(f.channels) + " channels");
        if (f.width < 2 || f.height < 2)
            throw SerError("a Bayer frame needs at least 2x2 samples");
    } else if (f.channels != 1 && f.channels != 3) {
        throw SerError("expected a 1- or 3-channel frame, got " + std::to_string(f.channels) + " channels");
    }
    return maxValue;
}

// A float plane with a mirrored border of `pad` samples on every side.
struct Padded {
    int w = 0, h = 0, pad = 0, stride = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[static_cast<size_t>(y + pad) * stride + (x + pad)]; }
    float& at(int x, int y) { return v[static_cast<size_t>(y + pad) * stride + (x + pad)]; }
    Padded(int width, int height, int border)
        : w(width), h(height), pad(border), stride(width + 2 * border),
          v(static_cast<size_t>(width + 2 * border) * (height + 2 * border)) {}
    // Fills the border from the interior by parity-preserving reflection.
    void MirrorBorder() {
        for (int y = -pad; y < h + pad; ++y)
            for (int x = -pad; x < w + pad; ++x)
                if (x < 0 || y < 0 || x >= w || y >= h) at(x, y) = at(ser::MirrorIndex(x, w), ser::MirrorIndex(y, h));
    }
};

FloatImage HamiltonAdams(const Frame& cfa, ColorId pattern, float maxValue) {
    const int w = cfa.width, h = cfa.height;
    const int* table = ser::PatternTable(pattern);
    auto site = [&](int x, int y) { return table[(y & 1) * 2 + (x & 1)]; };

    // Normalised CFA with a 2-sample mirrored border (the green step reads +-2).
    Padded c(w, h, 2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = static_cast<float>(cfa.samples[static_cast<size_t>(y) * w + x]) / maxValue;
    c.MirrorBorder();

    // Step 1: full green plane. At red/blue sites interpolate along the direction with the
    // smaller gradient (green difference + the site channel's second difference), with the
    // site channel's Laplacian as a correction term. The result is held inside the range of
    // the four green neighbours: the correction cannot overshoot at a limb (measured: it cuts
    // the worst error on an anti-aliased disc from 0.18 to 0.12), and stays exact on flat
    // fields and, away from the border, on linear ramps.
    Padded g(w, h, 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float here = c.at(x, y);
            if (site(x, y) == 1) {
                g.at(x, y) = here;
                continue;
            }
            const float gw = c.at(x - 1, y), ge = c.at(x + 1, y), gn = c.at(x, y - 1), gs = c.at(x, y + 1);
            const float lapH = 2.0f * here - c.at(x - 2, y) - c.at(x + 2, y);
            const float lapV = 2.0f * here - c.at(x, y - 2) - c.at(x, y + 2);
            const float dH = std::fabs(gw - ge) + std::fabs(lapH);
            const float dV = std::fabs(gn - gs) + std::fabs(lapV);
            const float estH = 0.5f * (gw + ge) + 0.25f * lapH;
            const float estV = 0.5f * (gn + gs) + 0.25f * lapV;
            const float est = dH < dV ? estH : dV < dH ? estV : 0.5f * (estH + estV);
            const float lo = std::min({gw, ge, gn, gs}), hi = std::max({gw, ge, gn, gs});
            g.at(x, y) = std::clamp(est, lo, hi);
        }
    g.MirrorBorder();

    // Step 2: red and blue from colour differences (channel - green), which are smooth
    // even where the luminance is not. Where they are not smooth (a coloured limb on black
    // sky) the sum can leave the recorded range -- measured -0.14 and 1.06 on a saturated
    // disc -- so red and blue are held to [0, brightest sample of the frame]. That bound is at
    // most 1 for in-range data and keeps samples above 2^normDepth - 1 (FrameRangeWarning) as
    // they are. Green and the site channel are already inside it.
    const float top = static_cast<float>(*std::max_element(cfa.samples.begin(), cfa.samples.end())) / maxValue;
    auto bound = [top](float v) { return std::clamp(v, 0.0f, top); };
    auto diff = [&](int x, int y) { return c.at(x, y) - g.at(x, y); };
    FloatImage out;
    out.width = w;
    out.height = h;
    out.channels = 3;
    out.data.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* px = &out.data[(static_cast<size_t>(y) * w + x) * 3];
            const int s = site(x, y);
            const float gHere = g.at(x, y);
            px[1] = gHere;
            if (s == 1) {
                // The channel on this row's horizontal neighbours, then the other one vertically.
                const int hc = site(x + 1, y);
                px[hc] = bound(gHere + 0.5f * (diff(x - 1, y) + diff(x + 1, y)));
                px[2 - hc] = bound(gHere + 0.5f * (diff(x, y - 1) + diff(x, y + 1)));
            } else {
                px[s] = c.at(x, y);
                // The opposite channel sits on the diagonals: use the smoother diagonal.
                const float d1 = std::fabs(c.at(x - 1, y - 1) - c.at(x + 1, y + 1)) +
                                 std::fabs(2.0f * gHere - g.at(x - 1, y - 1) - g.at(x + 1, y + 1));
                const float d2 = std::fabs(c.at(x + 1, y - 1) - c.at(x - 1, y + 1)) +
                                 std::fabs(2.0f * gHere - g.at(x + 1, y - 1) - g.at(x - 1, y + 1));
                const float e1 = 0.5f * (diff(x - 1, y - 1) + diff(x + 1, y + 1));
                const float e2 = 0.5f * (diff(x + 1, y - 1) + diff(x - 1, y + 1));
                px[2 - s] = bound(gHere + (d1 < d2 ? e1 : d2 < d1 ? e2 : 0.5f * (e1 + e2)));
            }
        }
    return out;
}
}  // namespace

FloatImage LumaForMeasure(const Frame& frame, ColorId color, int normDepth) {
    const float maxValue = CheckFrame(frame, color, normDepth);
    FloatImage out;
    out.channels = 1;
    if (ser::IsBayer(color)) {
        const int w = frame.width;
        out.width = frame.width / 2;
        out.height = frame.height / 2;
        out.data.resize(static_cast<size_t>(out.width) * out.height);
        const float scale = 1.0f / (4.0f * maxValue);
        for (int y = 0; y < out.height; ++y) {
            const uint16_t* r0 = &frame.samples[static_cast<size_t>(2 * y) * w];
            const uint16_t* r1 = r0 + w;
            for (int x = 0; x < out.width; ++x) {
                const uint32_t sum = uint32_t(r0[2 * x]) + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1];
                out.data[static_cast<size_t>(y) * out.width + x] = static_cast<float>(sum) * scale;
            }
        }
        return out;
    }
    out.width = frame.width;
    out.height = frame.height;
    const size_t n = static_cast<size_t>(frame.width) * frame.height;
    out.data.resize(n);
    if (frame.channels == 1) {
        for (size_t i = 0; i < n; ++i) out.data[i] = static_cast<float>(frame.samples[i]) / maxValue;
    } else {
        const float scale = 1.0f / (3.0f * maxValue);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t sum = uint32_t(frame.samples[3 * i]) + frame.samples[3 * i + 1] + frame.samples[3 * i + 2];
            out.data[i] = static_cast<float>(sum) * scale;
        }
    }
    return out;
}

FloatImage DemosaicHQ(const Frame& frame, ColorId color, int normDepth) {
    const float maxValue = CheckFrame(frame, color, normDepth);
    if (ser::IsBayer(color)) return HamiltonAdams(frame, color, maxValue);
    return ser::Normalize(frame, normDepth);
}

}  // namespace planetary::stack
