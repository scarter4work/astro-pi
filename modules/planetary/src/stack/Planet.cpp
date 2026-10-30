#include "Planet.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "Measure.h"
#include "MineralInternal.h"

namespace planetary::stack {

using namespace mineral_internal;

// ---- Sharpening for small discs ----

const SharpenProfile kPlanetSharpen = {{1.2, 1.0, 1.0, 1.0}, {0.6, 0.0, 0.0, 0.0}, 0.15};  // Planet.h: the measurements

// ---- The planet's disc ----

// The sky model (Planet.h, SegmentPlanet step 1): blocks of kPlaneBlock px, each split into kPlaneTile px tiles in a
// checkerboard for its noise; a block is sky while its mean lies within kSkySigmas of that noise of the plane. Under
// kPlaneMinBlocks blocks the frame is not judged; under kPlaneMinBlocks or kMinSkyBlockShare of them sky, it has no sky.
constexpr int kPlaneBlock = 16, kPlaneTile = 4;
constexpr double kSkySigmas = 3;
constexpr size_t kPlaneMinBlocks = 8;
constexpr double kMinSkyBlockShare = 0.10;
// The sky fit's second phase: at most kSkyRounds rounds.
constexpr int kSkyRounds = 50;
// The first window SegmentPlanet tries around the candidate (px a side; it doubles).
constexpr int kPlanetWindow = 64;
// The planet: SegmentLunar's piece over the sky by kEdgeSigmas of the sky's own scatter, at least kMinPiece px, its
// mean kDiscSigmas of that scatter over the sky; the sky, within kSkyPixelSigmas of its level.
constexpr double kEdgeSigmas = 3, kDiscSigmas = 5, kSkyPixelSigmas = 5;
constexpr size_t kMinPiece = 16;

namespace {

// Each channel's least-squares plane (SkyPlane: three rounds of 3-sigma rejection) over the finite pixels `use` holds.
void FitPlanes(const ser::FloatImage& rgb, std::vector<uint8_t> use, Plane p[3]) {
    const size_t n = Pixels(rgb);
    for (size_t i = 0; i < n; ++i)
        if (!(std::isfinite(rgb.data[i * 3]) && std::isfinite(rgb.data[i * 3 + 1]) && std::isfinite(rgb.data[i * 3 + 2]))) use[i] = 0;
    LunarMasks fit;
    fit.width = rgb.width;
    fit.height = rgb.height;
    fit.sky = std::move(use);
    fit.disk.assign(n, 0);
    for (int c = 0; c < 3; ++c) p[c] = SkyPlane(rgb, fit, c);
}

// The frame's sky (Planet.h, step 1). On the channels' mean in kPlaneBlock px blocks: each block's mean m = (a + b) / 2
// and noise d = (a - b) / 2 from the means a and b of its two checkerboard halves (kPlaneTile px tiles: smooth structure
// cancels in d, the noise does not, correlated or not); the blocks' noise sigma is MadStd(d). A least-squares plane over
// the member blocks, from all of them: first the blocks more than kSkySigmas above it leave, round by round, until none
// does (a body only adds light, so the plane settles on the sky under it, however much of the frame the body covers);
// then the members are the blocks within kSkySigmas of it either way (a dark block leaves too), until they settle. Each
// channel's plane is then fitted (SkyPlane) over the member blocks' pixels.
struct SkyModel {
    size_t blocks = 0, skyBlocks = 0;
    std::vector<uint8_t> sky;  // the member blocks' pixels
    Plane plane[3];
    bool judged() const { return blocks >= kPlaneMinBlocks; }
    bool hasSky() const {
        return judged() && skyBlocks >= kPlaneMinBlocks && skyBlocks >= kMinSkyBlockShare * static_cast<double>(blocks);
    }
};
SkyModel FitSky(const ser::FloatImage& rgb) {
    const int w = rgb.width, h = rgb.height;
    SkyModel s;
    struct Block { int x, y; double u, v, m; };
    std::vector<Block> blocks;
    std::vector<float> diff;
    for (int by = 0; by + kPlaneBlock <= h; by += kPlaneBlock)
        for (int bx = 0; bx + kPlaneBlock <= w; bx += kPlaneBlock) {
            double sum[2] = {0, 0};
            int k[2] = {0, 0};
            for (int y = by; y < by + kPlaneBlock; ++y)
                for (int x = bx; x < bx + kPlaneBlock; ++x) {
                    const float* q = &rgb.data[(static_cast<size_t>(y) * w + x) * 3];
                    const double l = (static_cast<double>(q[0]) + q[1] + q[2]) / 3;
                    if (!std::isfinite(l)) continue;
                    const int half = ((x - bx) / kPlaneTile + (y - by) / kPlaneTile) % 2;
                    sum[half] += l;
                    ++k[half];
                }
            if (!k[0] || !k[1]) continue;
            const double a = sum[0] / k[0], b = sum[1] / k[1];
            // The block's centre in Plane's coordinates.
            const double u = (bx + 0.5 * (kPlaneBlock - 1)) / std::max(1, w - 1) - 0.5;
            const double v = (by + 0.5 * (kPlaneBlock - 1)) / std::max(1, h - 1) - 0.5;
            blocks.push_back({bx, by, u, v, (a + b) / 2});
            diff.push_back(static_cast<float>((a - b) / 2));
        }
    s.blocks = blocks.size();
    if (!s.judged()) return s;
    // The blocks' noise, at least a float sample's rounding at the brightest block's level (a noise-free synthetic
    // frame's d is 0): at most 1.2e-7 for samples within [0, 1]; the real captures' is 3.8e-6 (2026-10-23 Saturn) to 5.7e-4.
    double top = 0;
    for (const Block& b : blocks) top = std::max(top, std::fabs(b.m));
    const double sigma = std::max(MadStd(diff), std::numeric_limits<float>::epsilon() * top), bound = kSkySigmas * sigma;
    std::vector<uint8_t> member(blocks.size(), 1), next(blocks.size());
    // The first phase only removes blocks, so it ends (within as many rounds as there are blocks); the second, which
    // may readmit blocks, has kSkyRounds.
    int second = 0;
    Plane p;
    for (bool below = true; below || second < kSkyRounds;) {  // below: the first phase, only the blocks above leave
        if (!below) ++second;
        double S[9] = {}, r[3] = {};
        size_t used = 0;
        for (size_t j = 0; j < blocks.size(); ++j) {
            if (!member[j]) continue;
            const double f[3] = {1, blocks[j].u, blocks[j].v};
            for (int a = 0; a < 3; ++a) {
                r[a] += f[a] * blocks[j].m;
                for (int b = 0; b < 3; ++b) S[a * 3 + b] += f[a] * f[b];
            }
            ++used;
        }
        double q[3];
        if (used < 3 || !Solve(3, S, r, q)) {  // the members are a line of blocks or fewer: no plane, no sky
            std::fill(member.begin(), member.end(), 0);
            break;
        }
        p = {q[0], q[1], q[2]};
        for (size_t j = 0; j < blocks.size(); ++j) {
            const double res = blocks[j].m - (p.a + p.b * blocks[j].u + p.c * blocks[j].v);
            next[j] = below ? (member[j] && res <= bound) : std::fabs(res) <= bound;
        }
        if (next == member) {
            if (!below) break;
            below = false;
        }
        member.swap(next);
    }
    s.sky.assign(Pixels(rgb), 0);
    for (size_t j = 0; j < blocks.size(); ++j) {
        if (!member[j]) continue;
        ++s.skyBlocks;
        for (int y = blocks[j].y; y < blocks[j].y + kPlaneBlock; ++y)
            std::fill_n(s.sky.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * w + blocks[j].x), kPlaneBlock, 1);
    }
    if (s.hasSky()) FitPlanes(rgb, s.sky, s.plane);
    return s;
}

// The 4-connected piece of `allowed` holding `at`, its holes (what the frame's edge cannot reach, 4-connected) filled.
std::vector<uint8_t> Piece(const std::vector<uint8_t>& allowed, int w, int h, size_t at) {
    const size_t n = allowed.size();
    std::vector<uint8_t> disk(n, 0), out(n, 0);
    std::vector<size_t> todo;
    auto grow = [&](std::vector<uint8_t>& mask, auto admit) {
        while (!todo.empty()) {
            const size_t j = todo.back();
            todo.pop_back();
            const int x = static_cast<int>(j % static_cast<size_t>(w)), y = static_cast<int>(j / static_cast<size_t>(w));
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& q : nb) {
                if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
                const size_t k = static_cast<size_t>(q[1]) * w + q[0];
                if (!mask[k] && admit(k)) {
                    mask[k] = 1;
                    todo.push_back(k);
                }
            }
        }
    };
    if (!allowed[at]) return disk;
    disk[at] = 1;
    todo.push_back(at);
    grow(disk, [&](size_t k) { return allowed[k] != 0; });
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t j = static_cast<size_t>(y) * w + x;
            if ((x == 0 || y == 0 || x == w - 1 || y == h - 1) && !disk[j]) {
                out[j] = 1;
                todo.push_back(j);
            }
        }
    grow(out, [&](size_t k) { return !disk[k]; });
    for (size_t j = 0; j < n; ++j) disk[j] = !out[j];
    return disk;
}

}  // namespace

LunarMasks SegmentPlanet(const ser::FloatImage& rgb, int threads) {
    RequireRgb(rgb, "SegmentPlanet");
    // 1. The sky: with none (a body filling the frame, or a frame too small to judge), SegmentLunar's masks.
    const SkyModel sky = FitSky(rgb);
    if (!sky.hasSky()) return SegmentLunar(rgb, threads);
    const int w = rgb.width, h = rgb.height;
    const size_t n = Pixels(rgb);
    // 2. The flattened copy: each channel less its sky plane (the image the pipeline processes is not touched; the
    // background step removes the sky's plane from it later). F: its channels' mean, 3 x 3 mean (SegmentLunar's levels
    // of the copy); the sky's level and scatter: the median and MadStd of F over the sky's blocks (correlated noise
    // included, which SegmentLunar's neighbour differences read low).
    ser::FloatImage flat = rgb;
    std::vector<float> L(n), F(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            for (int c = 0; c < 3; ++c)
                flat.data[i * 3 + c] = static_cast<float>(flat.data[i * 3 + c] - sky.plane[c].At(x, y, w, h));
            L[i] = (flat.data[i * 3] + flat.data[i * 3 + 1] + flat.data[i * 3 + 2]) / 3.0f;
        }
    size_t peak = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double s = 0;
            int k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int yy = y + dy, xx = x + dx;
                    if (yy < 0 || xx < 0 || yy >= h || xx >= w || !std::isfinite(L[static_cast<size_t>(yy) * w + xx])) continue;
                    s += L[static_cast<size_t>(yy) * w + xx];
                    ++k;
                }
            const size_t i = static_cast<size_t>(y) * w + x;
            F[i] = k ? static_cast<float>(s / k) : std::numeric_limits<float>::quiet_NaN();
            if (std::isfinite(F[i]) && !(F[i] <= F[peak])) peak = i;
        }
    std::vector<float> v;
    for (size_t i = 0; i < n; ++i)
        if (sky.sky[i] && std::isfinite(F[i])) v.push_back(F[i]);
    const double skyLevel = Median(v), scatter = MadStd(v);
    // 3. The candidate: the brightest pixel of the 8-connected group over the sky by 5 scatter and by a tenth of the
    // brightest pixel's height above it, holding the most light above the sky (final review: the frame's brightest
    // pixel alone took a field star brighter per pixel than a small Neptune or Uranus; a star's 1-2 px hold far less
    // light than a disc of a few px radius). With no group (a flat frame), the brightest pixel.
    {
        const double top = skyLevel + std::max(5 * scatter, 0.1 * (F[peak] - skyLevel));
        std::vector<uint8_t> seen(n, 0);
        std::vector<size_t> todo;
        double best = 0;
        for (size_t s0 = 0; s0 < n; ++s0) {
            if (seen[s0] || !(F[s0] > top)) continue;
            seen[s0] = 1;
            todo.assign(1, s0);
            double weight = 0;
            size_t brightest = s0;
            while (!todo.empty()) {
                const size_t i = todo.back();
                todo.pop_back();
                weight += F[i] - skyLevel;
                if (F[i] > F[brightest]) brightest = i;
                const int x = static_cast<int>(i % static_cast<size_t>(w)), y = static_cast<int>(i / static_cast<size_t>(w));
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int yy = y + dy, xx = x + dx;
                        if (yy < 0 || xx < 0 || yy >= h || xx >= w) continue;
                        const size_t j = static_cast<size_t>(yy) * w + xx;
                        if (seen[j] || !(F[j] > top)) continue;
                        seen[j] = 1;
                        todo.push_back(j);
                    }
            }
            if (weight > best) {
                best = weight;
                peak = brightest;
            }
        }
    }
    const int px = static_cast<int>(peak % static_cast<size_t>(w)), py = static_cast<int>(peak / static_cast<size_t>(w));
    // 4. The flattened copy through SegmentLunar: the whole frame, then windows of kPlanetWindow px and up, doubling,
    // centred on the candidate (clipped to the frame), while under a quarter of the frame. The first whose split is the
    // disc's and whose planet passes gives the disc:
    //  - The split: SegmentLunar's disc level D (the median of the crop's levels over their Otsu threshold) is
    //    kDiscSigmas of the sky's scatter over the sky. SegmentLunar's own test of D takes the noise from neighbour
    //    differences, which read correlated noise low, so in a large frame of sky its Otsu split of the sky's own noise
    //    passes (the real 2026-10-23 Neptune: D 0.0021 on a scatter of 0.0035) and a small disc does not move it; in a
    //    window the disc is a class of its own (0.285). The whole frame first: a window the disc fills most of puts the
    //    split inside the disc (Jupiter's belts and zones, Saturn's globe and rings), which raises D and with it the
    //    edge, 4% of the contrast (the real 2023-11-02 Jupiter: 2314 px in the 64 px window, 2405 in the frame).
    //  - The planet: SegmentLunar's piece holding the candidate, over the sky by kEdgeSigmas, holes filled; kMinPiece px
    //    or more, its mean kDiscSigmas over the sky, clear of the window's edges (but where they are the frame's). The
    //    edge at kEdgeSigmas: on a faint disc SegmentLunar's threshold (5 of its low noise sigma, or 4% of the contrast)
    //    joined the sky's noise to the disc: a level-0.1 r 3 disc on SmallInLargeSky's sky read 231 px, at 3 sigma 84
    //    px, as a bright disc's (the seeing's halo to about 2 blur sigma: 92 px); the real 2026-10-23 Neptune reads 88
    //    px (at 4 sigma 83, at 5 sigma 77).
    std::vector<int> sides(1, 0);  // 0: the whole frame
    for (int side = kPlanetWindow; static_cast<double>(side) * side < 0.25 * static_cast<double>(n); side *= 2) sides.push_back(side);
    const double cut = skyLevel + kEdgeSigmas * scatter;
    for (const int side : sides) {
        const int x0 = side ? std::max(0, px - side / 2) : 0, y0 = side ? std::max(0, py - side / 2) : 0;
        const int x1 = side ? std::min(w, x0 + side) : w, y1 = side ? std::min(h, y0 + side) : h, cw = x1 - x0, ch = y1 - y0;
        const size_t cn = static_cast<size_t>(cw) * ch;
        std::vector<float> levels;
        levels.reserve(cn);
        for (int y = y0; y < y1; ++y)
            levels.insert(levels.end(), F.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * w + x0),
                          F.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * w + x1));
        const double t0 = Otsu(levels);
        std::vector<float> upper;
        for (float l : levels)
            if (l > t0) upper.push_back(l);
        if (upper.empty() || !(Median(upper) - skyLevel > kDiscSigmas * scatter)) continue;
        ser::FloatImage crop{cw, ch, 3, std::vector<float>(cn * 3)};
        for (int y = 0; y < ch; ++y)
            std::copy_n(flat.data.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(y + y0) * w + x0) * 3),
                        static_cast<size_t>(cw) * 3, crop.data.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * cw * 3));
        const LunarMasks c = SegmentLunar(crop, threads);
        const size_t at = static_cast<size_t>(py - y0) * cw + (px - x0);
        if (c.diskCount == 0 || !c.disk[at]) continue;
        std::vector<uint8_t> allowed(cn);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const size_t j = static_cast<size_t>(y) * cw + x;
                allowed[j] = c.disk[j] && F[static_cast<size_t>(y + y0) * w + (x + x0)] > cut;
            }
        const std::vector<uint8_t> disk = Piece(allowed, cw, ch, at);
        size_t count = 0;
        double sum = 0;
        bool clear = true;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                if (!disk[static_cast<size_t>(y) * cw + x]) continue;
                ++count;
                sum += F[static_cast<size_t>(y + y0) * w + (x + x0)];
                if ((x == 0 && x0 > 0) || (y == 0 && y0 > 0) || (x == cw - 1 && x1 < w) || (y == ch - 1 && y1 < h)) clear = false;
            }
        if (count < kMinPiece || !clear || !(sum / static_cast<double>(count) > skyLevel + kDiscSigmas * scatter)) continue;
        // 5. The masks: the disc; the sky, over the whole frame, more than SegmentLunar's 4 px limb margin from it and
        // within kSkyPixelSigmas of the sky's level (a star or a moon stays out).
        LunarMasks out;
        out.width = w;
        out.height = h;
        out.disk.assign(n, 0);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) out.disk[static_cast<size_t>(y + y0) * w + (x + x0)] = disk[static_cast<size_t>(y) * cw + x];
        std::vector<uint8_t> near = out.disk;
        Dilate(near, w, h, 4);
        out.sky.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            out.sky[i] = !near[i] && std::isfinite(F[i]) && F[i] <= skyLevel + kSkyPixelSigmas * scatter;
            out.diskCount += out.disk[i];
            out.skyCount += out.sky[i];
        }
        return out;
    }
    // 6. None: no disc (spec anti-goal: a step that cannot run says why), never the frame SegmentLunar may take for one.
    LunarMasks none;
    none.width = w;
    none.height = h;
    none.disk.assign(n, 0);
    none.sky.assign(n, 0);
    none.reason = Fmt("no disc stands %.0f sigma over the sky (%.0f of the frame's %.0f %.0f px blocks fit its sky plane): no "
                      "window around the brightest candidate, nor the whole frame, holds one",
                      kDiscSigmas, sky.skyBlocks, sky.blocks, kPlaneBlock);
    return none;
}

// ---- Colour calibration ----

std::optional<BodyColour> PlanetReflectance(ser::Target t) {
    // Mallama, Krobusek & Pavlov 2017, "Comprehensive wide-band magnitudes and albedos for the planets", Icarus 282
    // (arXiv:1609.05048), Table 7: geometric albedos A_B, A_V (Johnson block) and A_r' (Sloan block);
    // B/G = A_B / A_V, R/G = A_r' / A_V as spec call 3 tabulates them. Red is Sloan r', not Cousins Rc (ruling Q3c):
    // the camera's R channel ends at its UV/IR-cut filter (~690 nm), and Rc reaches past that to ~800+ nm, into
    // methane absorption the camera never sees (Rc made Jupiter green-cream).
    switch (t) {
        case ser::Target::Mercury: return BodyColour{0.739, 1.190};  // 0.105 0.142 0.169
        case ser::Target::Venus: return BodyColour{0.955, 1.033};    // 0.658 0.689 0.712
        case ser::Target::Mars: return BodyColour{0.518, 1.441};     // 0.088 0.170 0.245
        case ser::Target::Jupiter: return BodyColour{0.823, 1.069};  // 0.443 0.538 0.575
        case ser::Target::Saturn: return BodyColour{0.679, 1.238};   // 0.339 0.499 0.618 (ring inclination 0)
        case ser::Target::Uranus: return BodyColour{1.150, 0.727};   // 0.561 0.488 0.355
        case ser::Target::Neptune: return BodyColour{1.271, 0.695};  // 0.562 0.442 0.307
        case ser::Target::Moon:
        case ser::Target::Other: break;
    }
    return std::nullopt;
}

namespace {

// CalibrateColour's background rule: a sky plane needs 1% of the frame.
constexpr double kMinSkyShare = 0.01;

// The disc's sums less the background. With `use` empty: over the disc's pixels with no saturated sample, which
// `use` then flags; else over the pixels `use` flags (the same pixels after the scaling).
struct DiscSums {
    double sum[3] = {0, 0, 0};
    size_t used = 0, saturated = 0;
};
DiscSums SumDisc(const ser::FloatImage& rgb, const LunarMasks& m, const Plane* bg, std::vector<uint8_t>& use) {
    const int w = rgb.width, h = rgb.height;
    const bool pick = use.empty();
    if (pick) use.assign(m.disk.size(), 0);
    DiscSums s;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!m.disk[i]) continue;
            const float* p = &rgb.data[i * 3];
            if (pick) {
                if (p[0] >= kClippedLuma || p[1] >= kClippedLuma || p[2] >= kClippedLuma) {
                    ++s.saturated;
                    continue;
                }
                use[i] = 1;
            } else if (!use[i]) {
                ++s.saturated;
                continue;
            }
            for (int c = 0; c < 3; ++c) s.sum[c] += p[c] - bg[c].At(x, y, w, h);
            ++s.used;
        }
    return s;
}

}  // namespace

StepRecord CalibratePlanetColour(ser::FloatImage& rgb, const LunarMasks& m, ser::Target t) {
    RequireMasks(rgb, m, "CalibratePlanetColour");
    const int w = rgb.width, h = rgb.height;
    const size_t n = Pixels(rgb);
    StepRecord rec;
    rec.skipped = true;
    const std::optional<BodyColour> ref = PlanetReflectance(t);
    if (!ref) {
        rec.history = std::string("Colour calibration skipped: no catalogue colour for the target ") + ser::TargetName(t) +
                      " (the planet path calibrates to a planet's reflectance); unchanged";
        return rec;
    }
    if (m.diskCount == 0) {
        rec.history = "Colour calibration skipped: no planet disc (" + m.reason + "); unchanged";
        return rec;
    }
    // Background reference: the sky's plane per channel, as CalibrateColour, so a gradient or pedestal does not bias
    // the disc's colour.
    const bool haveSky = m.skyCount >= kMinSkyShare * n;
    Plane bg[3];
    if (haveSky)
        for (int c = 0; c < 3; ++c) bg[c] = SkyPlane(rgb, m, c);
    std::vector<uint8_t> use;
    const DiscSums before = SumDisc(rgb, m, bg, use);
    if (before.used < kPlanetMinDiscPx || !(before.sum[0] > 0 && before.sum[1] > 0 && before.sum[2] > 0)) {
        rec.history = "Colour calibration skipped: " + std::to_string(before.used) + " unsaturated disc px (" +
                      std::to_string(before.saturated) + " with a saturated sample left out)" +
                      (before.used < kPlanetMinDiscPx ? ", under the " + std::to_string(kPlanetMinDiscPx) +
                                                            " a planet's colour reference needs"
                                                      : std::string(", and a channel's level is not positive")) +
                      "; unchanged";
        return rec;
    }
    // R and B scaled about the background so the disc's mean has the catalogue's B/G and R/G; G keeps its level.
    const double gain[3] = {ref->rOverG * before.sum[1] / before.sum[0], 1.0, ref->bOverG * before.sum[1] / before.sum[2]};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = &rgb.data[(static_cast<size_t>(y) * w + x) * 3];
            for (int c : {0, 2}) {
                const double b = bg[c].At(x, y, w, h);
                p[c] = static_cast<float>(b + (p[c] - b) * gain[c]);
            }
        }
    // Measured again on the same pixels.
    const DiscSums after = SumDisc(rgb, m, bg, use);
    rec.skipped = false;
    rec.history = std::string("Colour calibration: ") + ser::TargetName(t) +
                  Fmt("'s reflectance B/G %.3f R/G %.3f", ref->bOverG, ref->rOverG) +
                  " (Mallama, Krobusek & Pavlov 2017, Icarus 282, Table 7; camera B, G, R taken as Johnson B, Johnson V, "
                  "Sloan r'); disc " + std::to_string(before.used) + " px (" + std::to_string(before.saturated) +
                  " with a saturated sample left out)" +
                  Fmt(", B/G %.3f R/G %.3f before, ", before.sum[2] / before.sum[1], before.sum[0] / before.sum[1]) +
                  Fmt("%.3f and %.3f after", after.sum[2] / after.sum[1], after.sum[0] / after.sum[1]) +
                  "; background = " +
                  (haveSky ? std::string("sky plane") : "none (sky " + Percent(m.skyCount, n) + " of the frame, under 1%)") +
                  Fmt("; gains R %.4f G 1 B %.4f", gain[0], gain[2]);
    return rec;
}


// ---- Channel alignment for small discs ----

namespace {

constexpr int kLimbBand = 4;          // px of sky outside the disc in the fit (the limb's outer flank)
constexpr int kLimbInner = 4;         // px of disc inside its edge in the fit (the limb's inner flank)
constexpr double kSectorArc = 4;      // px of limb per sector (8 to 64 sectors)
constexpr int kJackGroups = 8;        // the jackknife deletes one of 8 runs of neighbouring sectors at a time
constexpr int kMinSectors = 4;        // sectors with data needed
constexpr size_t kMinSectorPx = 8;    // px a sector needs to count
constexpr double kPreSmooth = 1.0;    // AlignChannels' measurement smoothing (px)
constexpr double kNormalSmooth = 2.0; // the smoothing of G whose gradient gives the limb's normal (px)
constexpr double kMaxShift = 8;       // no convergence beyond this (px)
constexpr int kCropMargin = 4 + 8 + 4;  // around the band: the smoothing's reach + kMaxShift + taps and gradient
constexpr double kResampleMin = 0.1, kResampleSigmas = 3.0;  // AlignChannels' gate
constexpr double kCutMaxSigma = 0.05;  // a disc the frame cuts: corrected only when 1 sigma is within this (px)

// The fit's frame: a crop around the disc's limb.
struct FitData {
    ser::FloatImage smooth;       // the crop, smoothed by kPreSmooth
    std::vector<uint8_t> region;  // the limb band
    std::vector<int> sector;      // per pixel, 0 .. sectors - 1, by the normal's direction
    std::vector<float> nx, ny;    // per pixel, the limb's outward normal
    int sectors = 8;
};

// One sector's normal equations for (sx, sy, e), its gain and offset eliminated.
struct Sector {
    double Hs[9] = {}, gs[3] = {};
    double rss = 0;
    size_t n = 0;
};

// x = A^-1 b for a system of up to 3 unknowns (A and b are copied). False when singular or not finite.
bool SolveCopy(int n, const double* A, const double* b, double* x) {
    double M[9], v[3];
    std::copy(A, A + n * n, M);
    std::copy(b, b + n, v);
    if (!Solve(n, M, v, x)) return false;
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(x[i])) return false;
    return true;
}

// Per sector, at the shift s and the normal offset e: G ~ a_k (C(x + s) + e dC/dn) + b_k, with the least-squares
// a_k and b_k, and the normal equations of (sx, sy, e) with a_k and b_k eliminated (MeasureShift's Schur complement).
// Over the limb band's pixels whose Catmull-Rom taps and gradient stay inside the crop for shifts up to `bound` px.
// The Jacobian of position is the derivative along the limb's normal only: structure along the limb (bands that
// cross it, the rings' arms) carries colour as well as position, and is not used. One thread, row order.
std::vector<Sector> Accumulate(const FitData& f, int c, double sx, double sy, double e, int bound, int threads) {
    const int w = f.smooth.width, h = f.smooth.height, K = f.sectors;
    const std::vector<float> C = ShiftedPlane(f.smooth, c, sx, sy, threads);
    std::vector<Sector> sec(static_cast<size_t>(K));
    const int x0 = 2 + bound, x1 = w - 4 - bound, y0 = 2 + bound, y1 = h - 4 - bound;
    auto normal = [&](size_t i) {
        return 0.5 * (C[i + 1] - C[i - 1]) * f.nx[i] + 0.5 * (C[i + w] - C[i - w]) * f.ny[i];
    };
    std::vector<double> sc(K), scc(K), sg(K), scg(K), a(K, 1.0), b(K, 0.0);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!f.region[i]) continue;
            const int k = f.sector[i];
            const double cv = C[i] + e * normal(i), gv = f.smooth.data[i * 3 + 1];
            sc[k] += cv;
            scc[k] += cv * cv;
            sg[k] += gv;
            scg[k] += cv * gv;
            ++sec[k].n;
        }
    for (int k = 0; k < K; ++k) {
        if (sec[k].n == 0) continue;
        const double n = static_cast<double>(sec[k].n), var = scc[k] - sc[k] * sc[k] / n, cov = scg[k] - sc[k] * sg[k] / n;
        a[k] = var > 1e-12 * n ? cov / var : 1.0;
        b[k] = (sg[k] - a[k] * sc[k]) / n;
    }
    std::vector<std::array<double, 25>> H(K);
    std::vector<std::array<double, 5>> g(K);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!f.region[i]) continue;
            const int k = f.sector[i];
            const double gn = normal(i), cv = C[i] + e * gn;
            const double r = f.smooth.data[i * 3 + 1] - (a[k] * cv + b[k]);
            const double J[5] = {a[k] * gn * f.nx[i], a[k] * gn * f.ny[i], a[k] * gn, cv, 1.0};
            sec[k].rss += r * r;
            for (int p = 0; p < 5; ++p) {
                g[k][p] += J[p] * r;
                for (int q = 0; q < 5; ++q) H[k][p * 5 + q] += J[p] * J[q];
            }
        }
    for (int k = 0; k < K; ++k) {
        Sector& S = sec[k];
        const double* Hk = H[k].data();
        const double t00 = Hk[18], t01 = Hk[19], t11 = Hk[24], tdet = t00 * t11 - t01 * t01;
        const double i00 = tdet > 0 ? t11 / tdet : 0, i01 = tdet > 0 ? -t01 / tdet : 0, i11 = tdet > 0 ? t00 / tdet : 0;
        for (int p = 0; p < 3; ++p) {
            const double hp3 = Hk[p * 5 + 3], hp4 = Hk[p * 5 + 4];
            const double m0 = hp3 * i00 + hp4 * i01, m1 = hp3 * i01 + hp4 * i11;  // H_p,theta H_theta^-1
            S.gs[p] = g[k][p] - (m0 * g[k][3] + m1 * g[k][4]);
            for (int q = 0; q < 3; ++q) S.Hs[p * 3 + q] = Hk[p * 5 + q] - (m0 * Hk[3 * 5 + q] + m1 * Hk[4 * 5 + q]);
        }
    }
    return sec;
}

// The jackknife's group of sector k of K: runs of neighbouring directions.
int GroupOf(size_t k, size_t K) { return static_cast<int>(k * kJackGroups / K); }

// The equations summed over the sectors with kMinSectorPx, less group `skip` (-1: none).
void Sum(const std::vector<Sector>& sec, int skip, double H[9], double g[3]) {
    std::fill(H, H + 9, 0.0);
    std::fill(g, g + 3, 0.0);
    for (size_t k = 0; k < sec.size(); ++k)
        if (sec[k].n >= kMinSectorPx && GroupOf(k, sec.size()) != skip) {
            for (int p = 0; p < 9; ++p) H[p] += sec[k].Hs[p];
            for (int p = 0; p < 3; ++p) g[p] += sec[k].gs[p];
        }
}

// Channel c's shift against G (measured false and reason set when it cannot be measured).
DispersionShift MeasureChannel(const FitData& f, int c, int threads, int& sectorsUsed, size_t& pixels, std::string& reason) {
    DispersionShift out;
    double sx = 0, sy = 0, e = 0;
    // Passes: the pixels used are those whose taps stay inside for shifts up to `bound`; when the solution moves past
    // it, the next pass starts there with the wider bound.
    int bound = 1;
    bool converged = false;
    for (int pass = 0; pass < 4 && !converged; ++pass) {
        for (int round = 0; round < 60; ++round) {
            const std::vector<Sector> sec = Accumulate(f, c, sx, sy, e, bound, threads);
            double H[9], g[3], d[3];
            Sum(sec, -1, H, g);
            if (!SolveCopy(3, H, g, d)) {
                reason = "the limb does not constrain the shift (singular fit)";
                return out;
            }
            sx += d[0];
            sy += d[1];
            e += d[2];
            if (!std::isfinite(sx) || !std::isfinite(sy) || std::hypot(sx, sy) > kMaxShift || std::fabs(e) > kMaxShift) {
                reason = Fmt("no convergence within %.0f px", kMaxShift);
                return out;
            }
            if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < 1e-4) {
                converged = true;
                break;
            }
        }
        if (!converged) {
            reason = "no convergence within 60 rounds";
            return out;
        }
        const int need = static_cast<int>(std::ceil(std::max(std::fabs(sx), std::fabs(sy))));
        if (need > bound) {
            bound = need;
            converged = false;
        }
    }
    if (!converged) {
        reason = "no convergence";
        return out;
    }
    // At the solution: the formal 1-sigma, the grouped jackknife and the dependence on e.
    const std::vector<Sector> sec = Accumulate(f, c, sx, sy, e, bound, threads);
    double H[9], g[3], rss = 0;
    Sum(sec, -1, H, g);
    size_t npx = 0;
    int used = 0;
    for (const Sector& S : sec)
        if (S.n >= kMinSectorPx) {
            rss += S.rss;
            npx += S.n;
            ++used;
        }
    sectorsUsed = used;
    pixels = npx;
    if (used < kMinSectors) {
        reason = std::to_string(used) + " sector(s) of the limb with data, under the " + std::to_string(kMinSectors) +
                 " needed (the disc is mostly outside the frame)";
        return out;
    }
    const double inf = std::numeric_limits<double>::infinity();
    // Smoothed residuals are correlated over ~4 pi sigma^2 px: the independent-pixel variance is inflated by that area.
    const double corrArea = std::max(1.0, 4 * M_PI * kPreSmooth * kPreSmooth);
    const double dof = static_cast<double>(npx) - 3 - 2.0 * used;
    const double var = dof > 0 ? corrArea * rss / dof : inf;
    double cx[3], cy[3];
    const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
    const bool inv = SolveCopy(3, H, ex, cx) && SolveCopy(3, H, ey, cy);
    const double fx = inv && cx[0] > 0 ? std::sqrt(var * cx[0]) : inf, fy = inv && cy[1] > 0 ? std::sqrt(var * cy[1]) : inf;
    // Delete-one-group jackknife (linearised at the solution): a colour that differs around the limb moves the
    // groups' solutions apart, a true dispersion moves every group alike. Groups of neighbouring sectors, not single
    // sectors: a band's colour spans several, and deleting one alone left the others to repeat its bias.
    std::vector<double> jx, jy;
    for (int grp = 0; grp < kJackGroups; ++grp) {
        bool have = false;
        for (size_t k = 0; k < sec.size(); ++k) have = have || (GroupOf(k, sec.size()) == grp && sec[k].n >= kMinSectorPx);
        if (!have) continue;
        double Hk[9], gk[3], d[3];
        Sum(sec, grp, Hk, gk);
        if (!SolveCopy(3, Hk, gk, d)) {
            jx.assign(1, inf);
            jy.assign(1, inf);
            break;
        }
        jx.push_back(sx + d[0]);
        jy.push_back(sy + d[1]);
    }
    auto jack = [](const std::vector<double>& v) {
        if (v.size() < 2) return std::numeric_limits<double>::infinity();
        double mean = 0;
        for (double x : v) mean += x;
        mean /= static_cast<double>(v.size());
        double ss = 0;
        for (double x : v) ss += (x - mean) * (x - mean);
        return std::sqrt(ss * (static_cast<double>(v.size()) - 1) / static_cast<double>(v.size()));
    };
    // How far the shift depends on e: with e held at 0 the shift moves by H_ss^-1 H_se e (linearised). Nil for a limb
    // alike all round; on a limb the frame cuts, or one brighter on one side, the two trade off, and a channel whose limb
    // differs from G's (chromatic seeing: large e) reads a shift that is not there -- B blurred 2.0 px against 1.2 with
    // the light tilted 0.4 per radius read +0.29 px at r 10 with no true shift (Task 5 review). So it stays in each
    // channel's own uncertainty, whole disc or cut. It is not in DispersionSigmaAlong: along the axis of the other
    // channel, already corrected on its own, the opposite shift there is the evidence that the dispersion is real
    // (2023-11-02 Jupiter: B +0.64 px at +-0.47 with this term, +-0.13 without, beside R -0.85 corrected).
    const double det = H[0] * H[4] - H[1] * H[3], vx = H[2] * e, vy = H[5] * e;
    const double mx = det > 0 ? std::fabs((H[4] * vx - H[1] * vy) / det) : inf;
    const double my = det > 0 ? std::fabs((H[0] * vy - H[3] * vx) / det) : inf;
    out.dx = sx;
    out.dy = sy;
    out.ux = std::max({fx, jack(jx), mx});
    out.uy = std::max({fy, jack(jy), my});
    if (inv) out.cov = {var * cx[0], var * cx[1], var * cy[1]};
    else out.cov = {inf, 0, inf};
    for (size_t q = 0; q < jx.size(); ++q) out.groups.push_back({jx[q], jy[q]});
    out.measured = std::isfinite(out.ux) && std::isfinite(out.uy);
    if (!out.measured) reason = "its uncertainty could not be measured (one run of the limb carries the fit)";
    return out;
}

}  // namespace

PlanetDispersion MeasurePlanetDispersion(const ser::FloatImage& rgb, const LunarMasks& m, int threads) {
    RequireMasks(rgb, m, "MeasurePlanetDispersion");
    PlanetDispersion out;
    if (m.diskCount == 0) {
        out.reason = "no planet disc (" + m.reason + ")";
        return out;
    }
    if (m.diskCount < kPlanetMinDiscPx) {
        out.reason = "the disc is " + std::to_string(m.diskCount) + " px, under the " + std::to_string(kPlanetMinDiscPx) +
                     " a dispersion measurement needs";
        return out;
    }
    const int w = rgb.width, h = rgb.height;
    // The limb band: within kLimbBand px outside the disc and kLimbInner px inside its edge. The disc's interior is
    // left out: its colour structure (bands, albedo features) reads as a shift (whole disc, one gain per sector: 0.16
    // px on a 40 px disc and 0.47 px on a 200 px one with 3% band colour; this band: 0.05 and 0.02). The frame's edge
    // is not a limb: disc pixels beside it are in the band only when near the limb.
    std::vector<uint8_t> region = m.disk, inner(m.disk.size());
    Dilate(region, w, h, kLimbBand);
    for (size_t i = 0; i < inner.size(); ++i) inner[i] = !m.disk[i];
    Dilate(inner, w, h, kLimbInner);
    for (size_t i = 0; i < region.size(); ++i) region[i] = region[i] && inner[i];
    double cx = 0, cy = 0;
    int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (m.disk[i]) {
                cx += x;
                cy += y;
                out.frameCut = out.frameCut || x == 0 || y == 0 || x == w - 1 || y == h - 1;
            }
            if (region[i]) {
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
        }
    if (bx1 < 0) {
        out.reason = "the disc has no limb inside the frame (it fills the frame)";
        return out;
    }
    cx /= static_cast<double>(m.diskCount);
    cy /= static_cast<double>(m.diskCount);
    const int x0 = std::max(0, bx0 - kCropMargin), x1 = std::min(w - 1, bx1 + kCropMargin);
    const int y0 = std::max(0, by0 - kCropMargin), y1 = std::min(h - 1, by1 + kCropMargin);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    const size_t cn = static_cast<size_t>(cw) * ch;
    FitData f;
    // Sectors of about kSectorArc px of limb (the disc's equivalent circumference), 8 to 64, each with its own gain:
    // the limb's colour may vary along it.
    f.sectors = std::clamp(static_cast<int>(2 * M_PI * std::sqrt(m.diskCount / M_PI) / kSectorArc), 8, 64);
    ser::FloatImage crop{cw, ch, 3, std::vector<float>(cn * 3)};
    f.region.assign(cn, 0);
    f.sector.assign(cn, 0);
    f.nx.assign(cn, 0.0f);
    f.ny.assign(cn, 0.0f);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const size_t i = static_cast<size_t>(y) * cw + x, j = static_cast<size_t>(y + y0) * w + (x + x0);
            for (int c = 0; c < 3; ++c) crop.data[i * 3 + c] = rgb.data[j * 3 + c];
            f.region[i] = region[j];
        }
    // The limb's outward normal: down the gradient of G smoothed by kNormalSmooth px, which the limb's edge dominates,
    // so it holds for a disc the frame cuts (the centroid is not the disc's centre there), a phase, an oblate globe
    // and the rings' edges alike; where that gradient is under 1% of the band's largest, away from the centroid.
    {
        const ser::FloatImage gs = ChromaLayer(crop, kNormalSmooth, threads);
        std::vector<double> gx(cn, 0.0), gy(cn, 0.0);
        double gmax = 0;
        for (int y = 1; y + 1 < ch; ++y)
            for (int x = 1; x + 1 < cw; ++x) {
                const size_t i = static_cast<size_t>(y) * cw + x;
                gx[i] = -0.5 * (gs.data[(i + 1) * 3 + 1] - gs.data[(i - 1) * 3 + 1]);
                gy[i] = -0.5 * (gs.data[(i + cw) * 3 + 1] - gs.data[(i - cw) * 3 + 1]);
                if (f.region[i]) gmax = std::max(gmax, std::hypot(gx[i], gy[i]));
            }
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const size_t i = static_cast<size_t>(y) * cw + x;
                const double th = std::hypot(gx[i], gy[i]) > 0.01 * gmax ? std::atan2(gy[i], gx[i])
                                                                          : std::atan2(y + y0 - cy, x + x0 - cx);
                f.nx[i] = static_cast<float>(std::cos(th));
                f.ny[i] = static_cast<float>(std::sin(th));
                f.sector[i] = std::clamp(static_cast<int>(std::floor((th + M_PI) / (2 * M_PI) * f.sectors)), 0, f.sectors - 1);
            }
    }
    // AlignChannels' reason for smoothing first: Catmull-Rom resampling cuts white noise's variance most at half
    // pixels, which pulls a weak fit toward them; smoothed, the noise has little left at those frequencies.
    f.smooth = ChromaLayer(crop, kPreSmooth, threads);
    std::string reasonR, reasonB;
    int secR = 0, secB = 0;
    size_t pxR = 0, pxB = 0;
    out.r = MeasureChannel(f, 0, threads, secR, pxR, reasonR);
    out.b = MeasureChannel(f, 2, threads, secB, pxB, reasonB);
    out.sectors = std::min(secR, secB);
    out.pixels = std::min(pxR, pxB);
    if (!out.r.measured) out.reason = "the R shift against G could not be measured: " + reasonR;
    else if (!out.b.measured) out.reason = "the B shift against G could not be measured: " + reasonB;
    return out;
}

double DispersionSigmaAlong(const DispersionShift& s, double ax, double ay) {
    const double formal = std::sqrt(std::max(0.0, ax * ax * s.cov[0] + 2 * ax * ay * s.cov[1] + ay * ay * s.cov[2]));
    if (s.groups.size() < 2) return std::numeric_limits<double>::infinity();
    std::vector<double> v;
    double mean = 0;
    for (const auto& g : s.groups) {
        v.push_back(g[0] * ax + g[1] * ay);
        mean += v.back();
    }
    mean /= static_cast<double>(v.size());
    double ss = 0;
    for (double x : v) ss += (x - mean) * (x - mean);
    const double jack = std::sqrt(ss * (static_cast<double>(v.size()) - 1) / static_cast<double>(v.size()));
    return std::isfinite(formal) && std::isfinite(jack) ? std::max(formal, jack) : std::numeric_limits<double>::infinity();
}

StepRecord AlignPlanetChannels(ser::FloatImage& rgb, const LunarMasks& m, int threads, PlanetDispersion* result) {
    RequireMasks(rgb, m, "AlignPlanetChannels");
    PlanetDispersion d = MeasurePlanetDispersion(rgb, m, threads);
    StepRecord rec;
    rec.skipped = true;
    if (!d.reason.empty()) {
        rec.history = "Channel alignment skipped: " + d.reason + "; nothing resampled";
        if (result) *result = d;
        return rec;
    }
    const double ur = std::hypot(d.r.ux, d.r.uy), ub = std::hypot(d.b.ux, d.b.uy);
    const double mr = std::hypot(d.r.dx, d.r.dy), mb = std::hypot(d.b.dx, d.b.dy);
    rec.history = Fmt("Channel alignment (small disc): R %+.3f, %+.3f px (+-%.3f), B %+.3f, %+.3f px (+-%.3f) against G, from ",
                      d.r.dx, d.r.dy, ur, d.b.dx, d.b.dy, ub) +
                  std::to_string(d.pixels) + " px of the limb band (" + std::to_string(kLimbInner) + " px inside to " +
                  std::to_string(kLimbBand) + " px outside the disc's edge) in " + std::to_string(d.sectors) + " sectors";
    // Each channel on its own: corrected when its shift exceeds 0.1 px and 3 x its uncertainty; on a disc the frame
    // cuts, only when that uncertainty is also within kCutMaxSigma.
    auto gate = [&](double mag, double u) {
        return mag > kResampleMin && mag > kResampleSigmas * u && (!d.frameCut || u <= kCutMaxSigma);
    };
    d.r.applied = gate(mr, ur);
    d.b.applied = gate(mb, ub);
    // The dispersion axis (Task 5): atmospheric dispersion puts R and B on opposite sides of G along one line, so once
    // one channel passes, the other is tested along that line alone -- its component there, opposite the passing
    // channel, against 0.1 px and 3 x that component's own uncertainty (DispersionSigmaAlong); its component across
    // the line must be within 0.1 px or 3 sigma, else it is no dispersion and the axis gives it nothing. Applied, it
    // is resampled by that component alone. Not on a disc the frame cuts (Ruling Q1: there colour and shift trade off).
    std::string axisNote;
    if (d.r.applied != d.b.applied && !d.frameCut) {
        DispersionShift& q = d.r.applied ? d.b : d.r;
        const DispersionShift& p = d.r.applied ? d.r : d.b;
        const char* qn = d.r.applied ? "B" : "R";
        const char* pn = d.r.applied ? "R" : "B";
        const double pm = std::hypot(p.dx, p.dy), ax = -p.dx / pm, ay = -p.dy / pm;
        const double along = q.dx * ax + q.dy * ay, across = -q.dx * ay + q.dy * ax;
        const double sa = DispersionSigmaAlong(q, ax, ay), sc = DispersionSigmaAlong(q, -ay, ax);
        if (q.measured && along > kResampleMin && along > kResampleSigmas * sa &&
            std::fabs(across) <= std::max(kResampleMin, kResampleSigmas * sc)) {
            q.dx = along * ax;
            q.dy = along * ay;
            q.applied = q.alongAxis = true;
            axisNote = "; " + std::string(qn) + " along " + pn +
                       Fmt("'s dispersion axis: %+.3f px (+-%.3f; across it %+.3f, +-%.3f), applied as %+.3f, %+.3f px", along,
                           sa, across, sc, q.dx, q.dy);
        } else {
            axisNote = "; " + std::string(qn) + " along " + pn +
                       Fmt("'s dispersion axis: %+.3f px (+-%.3f; across it %+.3f, +-%.3f), not applied", along, sa, across, sc);
        }
    }
    if (result) *result = d;
    if (!d.r.applied && !d.b.applied) {
        rec.history += "; within 0.1 px or within 3 sigma of its uncertainty";
        if (d.frameCut) rec.history += Fmt(", or (the frame cuts the disc) its uncertainty over %.2f px", kCutMaxSigma);
        rec.history += ", not applied";
        return rec;
    }
    rec.history += axisNote;
    for (int c : {0, 2}) {
        const DispersionShift& s = c == 0 ? d.r : d.b;
        if (!s.applied) continue;
        std::vector<float> P = ShiftedPlane(rgb, c, s.dx, s.dy, threads);
        FillFrameEdge(P, rgb, s.dx, s.dy);
        for (size_t i = 0; i < P.size(); ++i) rgb.data[i * 3 + static_cast<size_t>(c)] = P[i];
    }
    rec.skipped = false;
    rec.history += d.r.applied && d.b.applied
                       ? "; R and B resampled onto G (Catmull-Rom)"
                       : (d.r.applied ? "; R resampled onto G (Catmull-Rom), B not" : "; B resampled onto G (Catmull-Rom), R not");
    if (d.r.applied != d.b.applied) {
        rec.history += " (within 0.1 px or within 3 sigma of its uncertainty";
        if (d.frameCut) rec.history += Fmt(", or (the frame cuts the disc) its uncertainty over %.2f px", kCutMaxSigma);
        rec.history += ")";
    }
    rec.history += "; at the frame's edge, the channel's ratio to G from the nearest pixels inside";
    return rec;
}

// ---- The planet pipeline ----

namespace {

constexpr const char* kPlanetProduct = "Planet processing";
// Step 1 found no disc: every later step says so instead of running (spec anti-goal: no defect hidden; Ruling P1: an
// accepted outcome for a faint Neptune).
constexpr const char* kNoPlanet = "no planet disc distinguished from the sky";
// Spec call 6: the mineral moon's +-6% deviation clamp and highlight fade, off. MineralColour has no switch for them;
// these values are beyond any reach: PreparePlanet clamps its display to [0, 1] after the alignment (AlignAndClamp), so
// C >= 0 and |d| < 2, and C <= 1 keeps w at 1.
constexpr double kNoClamp = 1e3, kNoFade = 1e3;

void RequirePlanetParams(const PlanetParams& p) {
    if (!(p.saturation >= 0) || !(p.chromaBlurSigma >= 0) || !(p.sharpen >= 0) || !std::isfinite(p.saturation) ||
        !std::isfinite(p.chromaBlurSigma) || !std::isfinite(p.sharpen))
        throw std::invalid_argument("Planet processing: saturation, chroma blur and sharpen must be finite and >= 0");
}

// AlignPlanetChannels on the display RGB, then every sample clamped to [0, 1] (Task 4 review, Important): Catmull-Rom
// overshoots below 0 (and above the display's top) beside the limb, and with the deviation clamp off a chroma mean near
// -eps made d unbounded there -- off-disc pixels past (1 + 2k) L, up to a one-channel speck at the gamut guard's 0.998
// on a pixel at the sky's level. With C >= 0, d lies in (-1, 2) and out_c <= (1 + 2k) L. HISTORY names the samples
// clamped. Nothing to clamp when nothing was resampled (the transfer's output is in [0, 1]).
StepRecord AlignAndClamp(ser::FloatImage& rgb, const LunarMasks& m, int threads) {
    StepRecord rec = AlignPlanetChannels(rgb, m, threads);
    if (rec.skipped) return rec;
    size_t below = 0, above = 0;
    for (float& v : rgb.data) {
        below += v < 0.0f;
        above += v > 1.0f;
        v = std::clamp(v, 0.0f, 1.0f);
    }
    rec.history += "; display clamped to [0, 1] after the resampling (" + std::to_string(below) + " samples below 0, " +
                   std::to_string(above) + " above 1)";
    return rec;
}

// The colour step's eps for a planet (Task 5): the mineral moon's 0.002 (Ruling R26: the sky's divide guard, set for a
// disc whose display median is 0.35) scaled by the disc's display median over 0.35, up to 0.002. The linear display
// puts the brightest sample at 0.95, so a small seeing-blurred disc's median sits low, and a fixed eps pulled its colour
// toward grey by about eps / C: 2026-10-23 Neptune (median 0.035) 2.4%, 2023-02-11 Mars (0.057) 1.9%, against
// acceptance 7's 2%. Scaled, the pull is at most 0.002 / 0.35 = 0.6% of the disc's median level. The sky stays
// guarded: there the output is L (C_c + eps) / (mean(C) + eps) with C >= 0, at most 3 L whatever eps (all of C in one
// channel).
double PlanetEps(const MineralCache& cache) {
    constexpr double kEps = 0.002, kReference = 0.35;
    std::vector<float> disc;
    for (size_t i = 0; i < cache.masks.disk.size(); ++i)
        if (cache.masks.disk[i]) disc.push_back(cache.lumBase.data[i]);
    if (disc.empty()) return kEps;
    const double d = Median(disc);
    return d > 0 ? kEps * std::min(1.0, d / kReference) : kEps;
}

double Seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

MineralCache PreparePlanet(const ser::FloatImage& stack, ser::Target t, int threads, const Progress& progress,
                           const StackNoise* noise) {
    if (!PlanetReflectance(t))
        throw std::invalid_argument(std::string(kPlanetProduct) +
                                    " needs a planet target (Mercury, Venus, Mars, Jupiter, Saturn, Uranus or Neptune: its "
                                    "colour reference is the planet's catalogue reflectance); this one is " +
                                    ser::TargetName(t));
    const DiscPipeline planet{kPhasePlanet,
                              kPlanetProduct,
                              "Planet disc",
                              kNoPlanet,
                              true,
                              [t](ser::FloatImage& rgb, const LunarMasks& m) { return CalibratePlanetColour(rgb, m, t); },
                              [threads](ser::FloatImage& rgb, const LunarMasks& m) { return AlignAndClamp(rgb, m, threads); },
                              0.0,  // linear (spec call 4)
                              false,
                              [](const ser::FloatImage& rgb, int n) { return SegmentPlanet(rgb, n); }};
    return PrepareDisc(stack, threads, progress, noise, planet);
}

MineralOutcome FinishPlanet(const MineralCache& cache, const PlanetParams& params, int threads, const Progress& progress) {
    RequirePlanetParams(params);
    MineralOutcome out;
    StepRecord sharpen, colour;
    auto t0 = std::chrono::steady_clock::now();
    const ser::FloatImage L = SharpenedLuminance(cache, params.sharpen, threads, &sharpen, kPlanetSharpen);
    const double sharpenSecs = Seconds(t0);
    PipelineSteps(progress, kPhasePlanet, kPlanetProduct, kPipelineSteps).Report(kPipelineSteps - 1);
    t0 = std::chrono::steady_clock::now();
    out.image = cache.display;
    if (cache.masks.diskCount == 0) {
        colour.skipped = true;
        colour.history = "Saturation skipped: " + (cache.notice.empty() ? std::string(kNoPlanet) : cache.notice) + "; nothing done";
    } else {
        MineralParams q;
        q.strength = params.saturation;
        q.chromaBlurSigma = params.chromaBlurSigma;
        q.scaleBlurWithWidth = false;  // the slider's px at any frame width (Planet.h)
        q.sharpen = params.sharpen;
        q.devClamp = kNoClamp;
        q.satThreshold = kNoFade;
        q.eps = PlanetEps(cache);
        const ColourCounts c = ColourTransfer(out.image, L, q, threads);
        colour.history = Fmt("Saturation: %.2fx the natural colour (the chroma layer's channel ratios on the sharp "
                             "luminance), chroma blur %.2f px, eps %.4f; no deviation clamp, no highlight fade",
                             params.saturation, c.sigma, q.eps) +
                         "; " + Percent(c.held, Pixels(out.image)) + " of pixels held in gamut";
    }
    const double colourSecs = Seconds(t0);
    PipelineSteps(progress, kPhasePlanet, kPlanetProduct, kPipelineSteps).Report(kPipelineSteps);
    out.history = cache.history;
    out.history.push_back(sharpen.history);
    out.history.push_back(colour.history);
    out.stepSeconds = cache.stepSeconds;
    out.stepSeconds.push_back({"Sharpen", sharpenSecs});
    out.stepSeconds.push_back({"Saturation", colourSecs});
    out.notice = cache.notice;
    return out;
}

}  // namespace planetary::stack
