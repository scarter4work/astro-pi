#include "Debayer.h"

namespace planetary::ser {

namespace {
// Channel (0=R, 1=G, 2=B) at site (x,y), indexed by (y&1)*2 + (x&1).
const int* PatternTable(ColorId p) {
    static const int rggb[4] = {0, 1, 1, 2}, grbg[4] = {1, 0, 2, 1},
                     gbrg[4] = {1, 2, 0, 1}, bggr[4] = {2, 1, 1, 0};
    switch (p) {
        case ColorId::BayerRGGB: return rggb;
        case ColorId::BayerGRBG: return grbg;
        case ColorId::BayerGBRG: return gbrg;
        case ColorId::BayerBGGR: return bggr;
        default: throw SerError(std::string(ColorIdName(p)) + " is not a Bayer pattern");
    }
}

void CheckSize(const Frame& f) {
    if (f.width <= 0 || f.height <= 0 || f.channels <= 0 ||
        f.samples.size() != static_cast<size_t>(f.width) * f.height * f.channels)
        throw SerError("frame holds " + std::to_string(f.samples.size()) + " samples, expected " +
                       std::to_string(f.width) + "x" + std::to_string(f.height) + "x" +
                       std::to_string(f.channels));
}

// Reflection about the edge sample (-1 -> 1, n -> n-2): keeps the site parity.
int Mirror(int i, int n) {
    if (i < 0) return -i;
    if (i >= n) return 2 * (n - 1) - i;
    return i;
}
}  // namespace

FloatImage Normalize(const Frame& frame, int depth) {
    CheckSize(frame);
    FloatImage out;
    out.width = frame.width;
    out.height = frame.height;
    out.channels = frame.channels;
    out.data.resize(frame.samples.size());
    const float maxValue = static_cast<float>((1u << depth) - 1u);
    for (size_t i = 0; i < frame.samples.size(); ++i)
        out.data[i] = static_cast<float>(frame.samples[i]) / maxValue;
    return out;
}

FloatImage DebayerBilinear(const Frame& cfa, ColorId pattern, int depth) {
    if (!IsBayer(pattern))
        throw SerError(std::string("cannot debayer ") + ColorIdName(pattern) + ": not a Bayer pattern");
    CheckSize(cfa);
    if (cfa.channels != 1)
        throw SerError("debayer needs a 1-channel CFA frame, got " + std::to_string(cfa.channels) + " channels");
    if (cfa.width < 2 || cfa.height < 2)
        throw SerError("debayer needs at least a 2x2 frame");

    const int w = cfa.width, h = cfa.height;
    const int* table = PatternTable(pattern);
    const float maxValue = static_cast<float>((1u << depth) - 1u);
    auto site = [&](int x, int y) { return table[(y & 1) * 2 + (x & 1)]; };
    // Raw sample at a mirrored position; sums stay integral and divide once.
    auto val = [&](int x, int y) -> int {
        return cfa.samples[static_cast<size_t>(Mirror(y, h)) * w + Mirror(x, w)];
    };
    auto mean2 = [&](int a, int b) { return static_cast<float>(a + b) / (2.0f * maxValue); };
    auto mean4 = [&](int a, int b, int c, int d) { return static_cast<float>(a + b + c + d) / (4.0f * maxValue); };

    FloatImage out;
    out.width = w;
    out.height = h;
    out.channels = 3;
    out.data.resize(static_cast<size_t>(w) * h * 3);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int c = site(x, y);
            float* px = &out.data[(static_cast<size_t>(y) * w + x) * 3];
            const float here = static_cast<float>(val(x, y)) / maxValue;
            if (c == 1) {
                // Green: the channel on this row's horizontal neighbours is read
                // horizontally, the other channel vertically.
                const int horizChannel = site(x + 1, y);
                px[1] = here;
                px[horizChannel] = mean2(val(x - 1, y), val(x + 1, y));
                px[2 - horizChannel] = mean2(val(x, y - 1), val(x, y + 1));
            } else {
                px[c] = here;
                px[1] = mean4(val(x - 1, y), val(x + 1, y), val(x, y - 1), val(x, y + 1));
                px[2 - c] = mean4(val(x - 1, y - 1), val(x + 1, y - 1), val(x - 1, y + 1), val(x + 1, y + 1));
            }
        }
    }
    return out;
}

}  // namespace planetary::ser
