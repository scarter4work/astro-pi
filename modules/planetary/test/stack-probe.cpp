// Test-only CLI over RunStack (Task 6), for real-data smoke runs and the benchmark harness (Task 9).
// Never shipped, not a ctest case. Reads the video only; writes only --out and the --mineral-* files.
//
//   stack-probe <video> [--bayer auto|none|RGGB|GRBG|GBRG|BGGR] [--threads N] [--out stack.fits|stack.tif]
//               [--bits 16|32] [--json] [--frames N] [--blur S] [--keep-best F] [--sample X,Y]...
//               [--cancel-at measuring|aligning|stacking,F] [--parity even|odd]
//               [--target auto|<name>] [--mineral] [--strength K] [--chroma-blur S] [--sharpen A]
//               [--mineral-out x.fits] [--mineral-also K:y.fits]... [--sharpen-also A:z.fits]...
//               [--mineral-stages DIR] [--mineral-crop L,T,R,B]
//               [--planet] [--saturation K] [--planet-out x.fits] [--planet-stages DIR]
//               [--chroma-blur-also S:y.fits]... [--saturation-also K:y.fits]...
//
// Runs RunStack on the video (default: Auto pattern, every hardware thread) and prints the summary --
// as one JSON object with --json, else as plain lines -- including the phase times, the read
// throughput and the process's peak resident memory (getrusage). Progress goes to stderr.
// --out writes the stack: FITS (.fits/.fit, 32-bit float, NAXIS3 = 3 planes for colour, first row =
// top of the image) or TIFF (.tif/.tiff, uncompressed, interleaved, --bits 16 (default; [0, 1] scaled to
// 0..65535) or 32 (IEEE float)). Probe-only knobs, both through a reader wrapper: --frames N stacks the
// first N frames; --blur S blurs every frame by a Gaussian of sigma S sensor px before the engine sees it
// (each colour site plane on its own for a mosaic, each channel for RGB), the capture's own noise put back
// by level (its noise model, Noise.h): a synthetic defocus of a real capture, for calibrating the focus
// floor. --keep-best F (investigation only: the benchmark's Ruling 2 round asks whether a gap to
// AutoStakkert's P20 is the selection's breadth) measures every frame with the engine's own MeasureAll and
// hands RunStack only the ceil(F N) frames of highest whole-frame quality, which RunStack then selects among
// as usual -- never a product knob (the spec's anti-goals forbid a percentage). --sample X,Y (repeatable)
// adds the stack's value at pixel (X, Y) -- on the output's grid, after the crop -- in every channel to the
// JSON ("samples"; the headless PixInsight probe compares PlanetaryStack's window with them,
// test/run-probe-stack.sh), next to the image's size ("image"), the first and last frame stacked
// ("stackedFirst", "stackedLast": the capture's own frame numbers, mapped back through --keep-best's or
// --parity's subset), the crop (Engine.h "The crop": "crop" {left, top, right, bottom, uncovered, thin {left,
// top, right, bottom}: of each side's px, those cut as thin, Ruling R19}, so output pixel (x, y) is the reference
// grid's (x + left, y + top)) and the frames' guard band
// ("guard", Accumulate.h "The frame's border"; Ruling R14). --parity even|odd (Task A3, Ruling R12) hands the
// engine only the even- or the odd-numbered frames of the capture (after --frames), as frames 0..n-1: two
// independent half-stacks for the split-half chroma noise check (edge_chroma.py --noise); the JSON names it
// ("parity"). Each half is a run of its own, with its own reference and so its own grid: edge_chroma.py
// registers the halves before it compares them.
// Mineral moon (Task B9, real-data acceptance). --target auto (default) takes the target from the capture's name
// (CaptureName.h), or names it (Moon, Jupiter, ..., Other). --mineral applies the window's gate first
// (MineralMoonRefusal on that target, the name's filter and the resolved pattern): refused, it prints the reason
// and exits 4 without stacking. Otherwise it runs PrepareMineral + FinishMineral on the stack at --strength
// (default 8), --chroma-blur (default 2.5) and --sharpen (default kSharpenDefault) -- MakeMineralMoon's own two
// halves, kept apart so the cache can be reused -- and the JSON gains "mineral": the seven HISTORY lines, the step
// times, FinishMineral's own time ("sliderSeconds": what a slider move costs), the notice, the fade threshold and
// the display curve. --mineral-out writes the mineral image (FITS); --mineral-also K:PATH (repeatable) re-runs only
// the sharpening and the colour on the same cache at strength K, as the window's sliders do, and writes it;
// --sharpen-also A:PATH the same at Sharpen amount A (Task SHARP). --mineral-stages DIR
// writes, as FITS: display.fits (the cache's display RGB: calibrated, background-neutral, display-transferred,
// channel-aligned, unsharpened), lum_sharp.fits (the L sharpened at --sharpen), lin_unsharp.fits and lin_sharp.fits (the
// channels' mean of display.fits and the sharpened L, each taken back through the inverse display transfer
// pointwise -- one map for both, so their DAN_interior compare like for like), calibrated.fits (the linear RGB
// after Colour calibration and Background, from a replica of steps 1-4 that also gives the exact display curve;
// its masks are checked equal to the cache's) and masks.fits (plane 0 disk, plane 1 sky, 0/1). The stack's noise
// (StackOutcome::noise, Ruling R35) goes to PrepareMineral, and the JSON's "stackNoise" gives its model line and the
// frame count k_eff over its grid. --mineral-crop L,T,R,B cuts the stack (and its noise) by those px before the
// mineral moon, which then runs on that field (--out still writes the whole stack): B9's equal-field split-half check.
// Planet processing (planet Task 5, real-data acceptance; spec 2026-10-29-planetary-planet-processing-design.md).
// --planet applies the window's gate first (PlanetRefusal on the target, the name's filter and the resolved pattern):
// refused, it prints the reason and exits 4 without stacking. Otherwise PreparePlanet + FinishPlanet on the stack at
// --saturation (default kPlanetSaturationDefault), --chroma-blur and --sharpen (under --planet their defaults are
// kPlanetChromaBlurDefault and kPlanetSharpenDefault), with the stack's noise; the JSON gains "planet": the seven
// HISTORY lines, the step times, "sliderSeconds" (FinishPlanet alone), the notice, the display's white point, the
// disc's px and box, and "dispersion": MeasurePlanetDispersion before the alignment (on a replica of steps 1-4, with
// what AlignPlanetChannels applied there; the replica's aligned and clamped display is checked equal to the cache's)
// and after it (on the cache's display). --planet-out writes the output (FITS); --sharpen-also A:PATH,
// --chroma-blur-also S:PATH and --saturation-also K:PATH (repeatable) re-run FinishPlanet on the same cache with that
// one slider moved, as the window's sliders do. --planet-stages DIR writes display.fits (the cache's: calibrated,
// background-neutral, linear, aligned, clamped), display_unaligned.fits (the replica's, before the alignment),
// lum_sharp.fits, lin_unsharp.fits, lin_sharp.fits, calibrated.fits and masks.fits, as --mineral-stages does.
// Exit 0 with an image, 2 when diagnosed, 1 on error, 4 when --mineral or --planet is refused.
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <random>
#include <thread>
#include <vector>

#include "CaptureName.h"
#include "Engine.h"
#include "Measure.h"
#include "Mineral.h"
#include "Noise.h"
#include "Planet.h"
#include "ProbeFits.h"
#include "ProbeSource.h"
#include "SerFile.h"

using namespace planetary;

using probe::ProbeSource;
using probe::WriteFits;

namespace {

ser::BayerChoice ParseBayer(const std::string& s) {
    if (s == "auto" || s == "Auto") return ser::BayerChoice::Auto;
    if (s == "none" || s == "None") return ser::BayerChoice::None;
    if (s == "RGGB") return ser::BayerChoice::RGGB;
    if (s == "GRBG") return ser::BayerChoice::GRBG;
    if (s == "GBRG") return ser::BayerChoice::GBRG;
    if (s == "BGGR") return ser::BayerChoice::BGGR;
    throw std::invalid_argument("--bayer: auto, none, RGGB, GRBG, GBRG or BGGR");
}

const char* FocusName(stack::FocusOutcome k) {
    switch (k) {
        case stack::FocusOutcome::NotJudged: return "NotJudged";
        case stack::FocusOutcome::InFocus: return "InFocus";
        case stack::FocusOutcome::Warned: return "Warned";
    }
    return "?";
}

const char* KindName(stack::DiagnosisKind k) {
    switch (k) {
        case stack::DiagnosisKind::None: return "None";
        case stack::DiagnosisKind::NoTarget: return "NoTarget";
        case stack::DiagnosisKind::OutOfFocus: return "OutOfFocus";
        case stack::DiagnosisKind::PoorSeeing: return "PoorSeeing";
        case stack::DiagnosisKind::Clipped: return "Clipped";
        case stack::DiagnosisKind::TargetLeavesFrame: return "TargetLeavesFrame";
        case stack::DiagnosisKind::Unaligned: return "Unaligned";
    }
    return "?";
}

std::string Json(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else o += c;
    }
    return o + "\"";
}

std::string Fmt(double v, int d = 3) {
    if (!std::isfinite(v)) return "null";
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}

void Put16BE(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xFF); }

// Baseline TIFF, big-endian ("MM"), one strip, interleaved samples.
void WriteTiff(const std::string& path, const ser::FloatImage& im, int bits) {
    const uint32_t w = static_cast<uint32_t>(im.width), h = static_cast<uint32_t>(im.height), spp = static_cast<uint32_t>(im.channels);
    std::vector<uint8_t> pixels;
    for (float v : im.data) {
        if (bits == 16) {
            Put16BE(pixels, static_cast<uint16_t>(std::lround(std::clamp(static_cast<double>(v), 0.0, 1.0) * 65535)));
        } else {
            uint32_t u;
            std::memcpy(&u, &v, 4);
            for (int k = 3; k >= 0; --k) pixels.push_back(static_cast<uint8_t>(u >> (8 * k)));
        }
    }
    std::vector<uint8_t> b = {'M', 'M', 0, 42};
    auto put32 = [&](uint32_t v) { for (int k = 3; k >= 0; --k) b.push_back(static_cast<uint8_t>(v >> (8 * k))); };
    auto put16 = [&](uint16_t v) { Put16BE(b, v); };
    const uint32_t dataOff = 8, bpsOff = dataOff + static_cast<uint32_t>(pixels.size());
    put32(0);  // IFD offset, patched below
    b.insert(b.end(), pixels.begin(), pixels.end());
    // BitsPerSample / SampleFormat arrays when spp > 1.
    const uint32_t arrays = bpsOff;
    for (uint32_t i = 0; i < spp; ++i) put16(static_cast<uint16_t>(bits));
    for (uint32_t i = 0; i < spp; ++i) put16(bits == 32 ? 3 : 1);
    if (b.size() & 1) b.push_back(0);
    const uint32_t ifd = static_cast<uint32_t>(b.size());
    for (int k = 0; k < 4; ++k) b[4 + k] = static_cast<uint8_t>(ifd >> (8 * (3 - k)));
    struct Entry { uint16_t tag, type; uint32_t count, value; };
    std::vector<Entry> e = {
        {256, 4, 1, w}, {257, 4, 1, h},
        {258, 3, spp, spp == 1 ? static_cast<uint32_t>(bits) << 16 : arrays},
        {259, 3, 1, 1u << 16}, {262, 3, 1, (spp == 3 ? 2u : 1u) << 16}, {273, 4, 1, dataOff},
        {277, 3, 1, spp << 16}, {278, 4, 1, h}, {279, 4, 1, static_cast<uint32_t>(pixels.size())},
        {284, 3, 1, 1u << 16},
        {339, 3, spp, spp == 1 ? (bits == 32 ? 3u : 1u) << 16 : arrays + 2 * spp}};
    put16(static_cast<uint16_t>(e.size()));
    for (const Entry& x : e) {
        put16(x.tag);
        put16(x.type);
        put32(x.count);
        put32(x.value);
    }
    put32(0);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    const bool written = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    if (std::fclose(f) != 0 || !written) throw std::runtime_error("cannot write " + path + " (disk full?)");
}

ser::Target ParseTarget(const std::string& s) {
    for (ser::Target t : {ser::Target::Other, ser::Target::Moon, ser::Target::Mercury, ser::Target::Venus, ser::Target::Mars,
                          ser::Target::Jupiter, ser::Target::Saturn, ser::Target::Uranus, ser::Target::Neptune})
        if (s == ser::TargetName(t)) return t;
    throw std::invalid_argument("--target: auto, Moon, Mercury, Venus, Mars, Jupiter, Saturn, Uranus, Neptune or Other");
}

const char* FilterName(ser::FilterClass f) {
    switch (f) {
        case ser::FilterClass::Broadband: return "Broadband";
        case ser::FilterClass::Narrowband: return "Narrowband";
        case ser::FilterClass::IrPass: return "IR-pass";
        case ser::FilterClass::Unknown: break;
    }
    return "Unknown";
}

// The channels' mean, one channel.
ser::FloatImage Mean3(const ser::FloatImage& rgb) {
    ser::FloatImage l;
    l.width = rgb.width;
    l.height = rgb.height;
    l.channels = 1;
    l.data.resize(rgb.data.size() / 3);
    for (size_t i = 0; i < l.data.size(); ++i) l.data[i] = (rgb.data[i * 3] + rgb.data[i * 3 + 1] + rgb.data[i * 3 + 2]) / 3.0f;
    return l;
}

// The display transfer undone, pointwise: MTF(., m) is inverted by MTF(., 1 - m), then times the white point.
ser::FloatImage UnDisplay(ser::FloatImage l, const stack::DisplayCurve& c) {
    const double m = 1 - c.balance;
    for (float& v : l.data) {
        const double y = std::clamp(static_cast<double>(v), 0.0, 1.0);
        const double x = y <= 0 ? 0 : y >= 1 ? 1 : (m - 1) * y / ((2 * m - 1) * y - m);
        v = static_cast<float>(c.white * x);
    }
    return l;
}

// --planet: one MeasurePlanetDispersion result (with what AlignPlanetChannels applied, where it ran) as JSON.
std::string DispersionJson(const stack::PlanetDispersion& d) {
    auto ch = [](const stack::DispersionShift& s) {
        return "{\"dx\":" + Fmt(s.dx, 4) + ",\"dy\":" + Fmt(s.dy, 4) + ",\"ux\":" + Fmt(s.ux, 4) + ",\"uy\":" + Fmt(s.uy, 4) +
               ",\"measured\":" + (s.measured ? "true" : "false") + ",\"applied\":" + (s.applied ? "true" : "false") +
               ",\"alongAxis\":" + (s.alongAxis ? "true" : "false") + "}";
    };
    return "{\"r\":" + ch(d.r) + ",\"b\":" + ch(d.b) + ",\"pixels\":" + std::to_string(d.pixels) + ",\"sectors\":" +
           std::to_string(d.sectors) + ",\"frameCut\":" + (d.frameCut ? "true" : "false") + ",\"reason\":" + Json(d.reason) + "}";
}

bool EndsWith(const std::string& s, const std::string& t) { return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }

// --keep-best F, --parity: the engine sees only these frames of the source (ascending), as frames 0..n-1.
class SubsetSource : public ser::FrameSource {
public:
    SubsetSource(std::unique_ptr<ser::FrameSource> s, std::shared_ptr<const std::vector<int64_t>> keep)
        : s_(std::move(s)), keep_(std::move(keep)), info_(s_->Info()) {
        info_.frameCount = static_cast<int64_t>(keep_->size());
    }
    const ser::FileInfo& Info() const override { return info_; }
    ser::Frame ReadFrame(int64_t i) const override { return s_->ReadFrame(At(i)); }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return s_->FrameTimestamp(At(i)); }

private:
    int64_t At(int64_t i) const {
        if (i < 0 || i >= static_cast<int64_t>(keep_->size())) throw ser::SerError("stack-probe: frame past the subset");
        return (*keep_)[static_cast<size_t>(i)];
    }
    std::unique_ptr<ser::FrameSource> s_;
    std::shared_ptr<const std::vector<int64_t>> keep_;
    ser::FileInfo info_;
};

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: stack-probe <video> [--bayer auto|none|RGGB|GRBG|GBRG|BGGR] [--threads N] "
                             "[--out stack.fits|stack.tif] [--bits 16|32] [--json] [--frames N] [--blur S] [--keep-best F] [--sample X,Y]... [--parity even|odd] "
                             "[--target auto|<name>] [--mineral] [--strength K] [--chroma-blur S] [--sharpen A] [--mineral-out x.fits] "
                             "[--mineral-also K:y.fits]... [--sharpen-also A:z.fits]... [--mineral-stages DIR] [--mineral-crop L,T,R,B] "
                             "[--planet] [--saturation K] [--planet-out x.fits] [--planet-stages DIR] "
                             "[--chroma-blur-also S:y.fits]... [--saturation-also K:y.fits]...\n");
        return 1;
    }
    const std::string path = argv[1];
    ser::BayerChoice bayer = ser::BayerChoice::Auto;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::string out;
    int bits = 16;
    bool json = false;
    int64_t limit = INT64_MAX;
    double blur = 0;
    double keepBest = 0;
    int parity = -1;  // --parity: 0 even, 1 odd, -1 every frame
    std::vector<std::pair<int, int>> samples;
    std::string targetArg = "auto";
    bool mineral = false, planet = false;
    double strength = 8.0, saturation = stack::kPlanetSaturationDefault;
    std::optional<double> chromaBlurArg, sharpenArg;  // unset: the mineral moon's or the planet's default
    std::string mineralOut, mineralStages, planetOut, planetStages;
    std::vector<std::pair<double, std::string>> mineralAlso, sharpenAlso, chromaAlso, saturationAlso;
    // V:PATH (the --*-also flags).
    auto valuePath = [](const std::string& flag, const std::string& v, std::vector<std::pair<double, std::string>>& to) {
        const size_t colon = v.find(':');
        if (colon == std::string::npos) throw std::invalid_argument(flag + ": V:PATH");
        to.emplace_back(std::stod(v.substr(0, colon)), v.substr(colon + 1));
    };
    int mineralCrop[4] = {0, 0, 0, 0};  // --mineral-crop L,T,R,B
    // --cancel-at PHASE,F (test only): cancelled() turns true once PHASE has reported F of its total (or a
    // later phase has started) -- the engine's own cancel path mid-run, as the window's Cancel and Quit
    // take it. Exit 3 when RunStack throws StackCancelled.
    std::string cancelPhase;
    double cancelFraction = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] {
            if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
            return std::string(argv[++i]);
        };
        if (a == "--bayer") bayer = ParseBayer(next());
        else if (a == "--threads") threads = std::max(1, std::atoi(next().c_str()));
        else if (a == "--out") out = next();
        else if (a == "--bits") bits = std::atoi(next().c_str());
        else if (a == "--json") json = true;
        else if (a == "--frames") limit = std::stoll(next());
        else if (a == "--blur") blur = std::stod(next());
        else if (a == "--keep-best") {
            keepBest = std::stod(next());
            if (!(keepBest > 0 && keepBest <= 1)) throw std::invalid_argument("--keep-best: a fraction in (0, 1]");
        }
        else if (a == "--parity") {
            const std::string v = next();
            if (v == "even") parity = 0;
            else if (v == "odd") parity = 1;
            else throw std::invalid_argument("--parity: even or odd");
        }
        else if (a == "--sample") {
            const std::string v = next();
            const size_t comma = v.find(',');
            if (comma == std::string::npos) throw std::invalid_argument("--sample: X,Y");
            samples.emplace_back(std::stoi(v.substr(0, comma)), std::stoi(v.substr(comma + 1)));
        }
        else if (a == "--target") targetArg = next();
        else if (a == "--mineral") mineral = true;
        else if (a == "--strength") strength = std::stod(next());
        else if (a == "--chroma-blur") chromaBlurArg = std::stod(next());
        else if (a == "--sharpen") sharpenArg = std::stod(next());
        else if (a == "--planet") planet = true;
        else if (a == "--saturation") saturation = std::stod(next());
        else if (a == "--planet-out") planetOut = next();
        else if (a == "--planet-stages") planetStages = next();
        else if (a == "--chroma-blur-also") valuePath(a, next(), chromaAlso);
        else if (a == "--saturation-also") valuePath(a, next(), saturationAlso);
        else if (a == "--mineral-out") mineralOut = next();
        else if (a == "--mineral-stages") mineralStages = next();
        else if (a == "--mineral-crop") {
            const std::string v = next();
            if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &mineralCrop[0], &mineralCrop[1], &mineralCrop[2], &mineralCrop[3]) != 4 ||
                *std::min_element(mineralCrop, mineralCrop + 4) < 0)
                throw std::invalid_argument("--mineral-crop: L,T,R,B (px, >= 0)");
        }
        else if (a == "--mineral-also") valuePath(a, next(), mineralAlso);
        else if (a == "--sharpen-also") valuePath(a, next(), sharpenAlso);
        else if (a == "--cancel-at") {
            const std::string v = next();
            const size_t comma = v.find(',');
            if (comma == std::string::npos) throw std::invalid_argument("--cancel-at: PHASE,F");
            const std::string ph = v.substr(0, comma);
            cancelPhase = ph == "measuring" ? stack::kPhaseMeasuring : ph == "aligning" ? stack::kPhaseAligning
                        : ph == "stacking" ? stack::kPhaseStacking : "";
            cancelFraction = std::stod(v.substr(comma + 1));
            if (cancelPhase.empty() || !(cancelFraction >= 0 && cancelFraction <= 1))
                throw std::invalid_argument("--cancel-at: measuring|aligning|stacking,F with F in [0, 1]");
        }
        else throw std::invalid_argument("unknown argument " + a);
    }
    if (bits != 16 && bits != 32) throw std::invalid_argument("--bits: 16 or 32");
    if (parity >= 0 && keepBest > 0) throw std::invalid_argument("--parity and --keep-best do not combine");
    if (mineral && planet) throw std::invalid_argument("--mineral and --planet do not combine");
    if (!mineral && (!mineralOut.empty() || !mineralStages.empty() || !mineralAlso.empty() ||
                     (mineralCrop[0] | mineralCrop[1] | mineralCrop[2] | mineralCrop[3])))
        throw std::invalid_argument("--mineral-out, --mineral-also, --mineral-crop and --mineral-stages need --mineral");
    if (!planet && (!planetOut.empty() || !planetStages.empty() || !chromaAlso.empty() || !saturationAlso.empty()))
        throw std::invalid_argument("--planet-out, --planet-stages, --chroma-blur-also and --saturation-also need --planet");
    if (!mineral && !planet && !sharpenAlso.empty()) throw std::invalid_argument("--sharpen-also needs --mineral or --planet");
    const double chromaBlur = chromaBlurArg.value_or(planet ? stack::kPlanetChromaBlurDefault : 2.5);
    const double sharpen = sharpenArg.value_or(planet ? stack::kPlanetSharpenDefault : stack::kSharpenDefault);
    for (const auto* list : {&mineralAlso, &sharpenAlso, &chromaAlso, &saturationAlso})
        for (const auto& f : *list)
            if (!EndsWith(f.second, ".fits") && !EndsWith(f.second, ".fit"))
                throw std::invalid_argument("--*-also: V:PATH with a .fits/.fit path (" + f.second + ")");
    for (const std::string* f : {&mineralOut, &planetOut})
        if (!f->empty() && !EndsWith(*f, ".fits") && !EndsWith(*f, ".fit"))
            throw std::invalid_argument("--mineral-out / --planet-out: a .fits/.fit path");
    const ser::CaptureName name = ser::ParseCaptureName(path);
    const ser::Target target = targetArg == "auto" ? name.target : ParseTarget(targetArg);

    stack::Progress progress;
    auto lastPrint = std::chrono::steady_clock::now();
    std::string lastPhase;
    const std::vector<std::string> phases = {stack::kPhaseMeasuring, stack::kPhaseAligning, stack::kPhaseStacking};
    auto phaseIndex = [&](const std::string& p) {
        return static_cast<int>(std::find(phases.begin(), phases.end(), p) - phases.begin());
    };
    std::atomic<bool> cancelNow{false};
    std::string cancelSeen;
    progress.report = [&](const std::string& phase, int64_t done, int64_t total) {
        // One report turns the flag on (compare_exchange), and only that one writes cancelSeen, so a report
        // from another thread can never write it at the same time; main reads it after RunStack has joined
        // every thread.
        bool expected = false;
        if (!cancelPhase.empty() &&
            (phaseIndex(phase) > phaseIndex(cancelPhase) ||
             (phase == cancelPhase && static_cast<double>(done) >= cancelFraction * static_cast<double>(total))) &&
            cancelNow.compare_exchange_strong(expected, true))
            cancelSeen = phase + " " + std::to_string(done) + "/" + std::to_string(total);
        const auto now = std::chrono::steady_clock::now();
        if (phase != lastPhase || done == total || now - lastPrint > std::chrono::seconds(2)) {
            std::fprintf(stderr, "  %s %lld/%lld\n", phase.c_str(), static_cast<long long>(done), static_cast<long long>(total));
            lastPrint = now;
            lastPhase = phase;
        }
    };
    // --blur puts back the noise the capture itself shows, by level: its model from the unblurred frames.
    const std::shared_ptr<const stack::NoiseModel> noise = blur > 0 ? probe::UnblurredNoise(path, bayer, limit, threads) : nullptr;
    const stack::SourceFactory probeFactory = [&]() -> std::unique_ptr<ser::FrameSource> {
        return std::make_unique<ProbeSource>(ser::OpenVideo(path, bayer), limit, blur, noise);
    };
    stack::SourceFactory factory = probeFactory;
    std::shared_ptr<std::vector<int64_t>> keep;
    if (keepBest > 0) {
        // Investigation only (Task 9, Ruling 2's round): the engine's own whole-frame quality
        // (MeasureAll, the measure SelectFrames ranks on) picks the sharpest ceil(F N) frames with a
        // target; RunStack then runs on those alone, selection and regions included.
        const ser::FileInfo info = probeFactory()->Info();
        const std::vector<stack::FrameStats> st = stack::MeasureAll(probeFactory, info, threads, {});
        std::vector<int64_t> order;
        for (const auto& s : st)
            if (s.targetFound) order.push_back(s.index);
        std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
            return st[static_cast<size_t>(a)].quality > st[static_cast<size_t>(b)].quality;
        });
        const size_t n = std::min(order.size(), static_cast<size_t>(std::ceil(keepBest * static_cast<double>(info.frameCount))));
        keep = std::make_shared<std::vector<int64_t>>(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(n));
        std::sort(keep->begin(), keep->end());
        std::fprintf(stderr, "  --keep-best %.3f: %zu of %lld frames handed to the engine\n", keepBest, keep->size(),
                     static_cast<long long>(info.frameCount));
        std::shared_ptr<const std::vector<int64_t>> k = keep;
        factory = [probeFactory, k]() -> std::unique_ptr<ser::FrameSource> { return std::make_unique<SubsetSource>(probeFactory(), k); };
    }
    if (parity >= 0) {
        const int64_t n = probeFactory()->Info().frameCount;
        keep = std::make_shared<std::vector<int64_t>>();
        for (int64_t i = parity; i < n; i += 2) keep->push_back(i);
        if (keep->empty()) throw std::invalid_argument("--parity: no frame of that parity");
        std::fprintf(stderr, "  --parity %s: %zu of %lld frames handed to the engine\n", parity ? "odd" : "even", keep->size(),
                     static_cast<long long>(n));
        std::shared_ptr<const std::vector<int64_t>> k = keep;
        factory = [probeFactory, k]() -> std::unique_ptr<ser::FrameSource> { return std::make_unique<SubsetSource>(probeFactory(), k); };
    }
    // --mineral: the window's gate, before any stacking (the option is disabled there, so nothing is stacked).
    // --planet: the same with PlanetRefusal.
    if (mineral || planet) {
        const char* key = mineral ? "mineral" : "planet";
        const std::optional<std::string> refusal =
            mineral ? ser::MineralMoonRefusal(target, name.filter, probeFactory()->Info(), name.filterToken)
                    : ser::PlanetRefusal(target, name.filter, probeFactory()->Info(), name.filterToken);
        if (refusal) {
            if (json)
                std::printf("{\"capture\":%s,\"target\":%s,\"filter\":%s,\"%s\":{\"refusal\":%s}}\n", Json(path).c_str(),
                            Json(ser::TargetName(target)).c_str(), Json(FilterName(name.filter)).c_str(), key,
                            Json(*refusal).c_str());
            else
                std::printf("%-9s refused: %s\n", key, refusal->c_str());
            if (std::fflush(stdout) != 0) throw std::runtime_error("cannot write the refusal to standard output");
            return 4;
        }
    }
    progress.cancelled = [&] { return cancelNow.load(); };
    stack::StackOutcome o;
    try {
        o = stack::RunStack(factory, threads, progress);
    } catch (const stack::StackCancelled&) {
        std::printf("cancelled in %s\n", cancelSeen.c_str());
        return 3;
    }
    if (!cancelPhase.empty()) {
        std::fprintf(stderr, "error: --cancel-at %s never fired\n", cancelPhase.c_str());
        return 1;
    }
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double peakMB = ru.ru_maxrss / 1024.0;  // KiB on Linux
    const stack::StackSummary& s = o.summary;
    // The engine numbers the frames it was handed; under --keep-best / --parity that is the subset (Task A3 review M4).
    auto captureFrame = [&](int64_t i) { return keep ? (*keep)[static_cast<size_t>(i)] : i; };

    if (o.image && !out.empty()) {
        if (EndsWith(out, ".fits") || EndsWith(out, ".fit")) WriteFits(out, *o.image);
        else if (EndsWith(out, ".tif") || EndsWith(out, ".tiff")) WriteTiff(out, *o.image, bits);
        else throw std::invalid_argument("--out: .fits/.fit or .tif/.tiff");
    }
    // --mineral: MakeMineralMoon's two halves (PrepareMineral, then FinishMineral), timed as it times them.
    std::optional<stack::MineralCache> mcache;
    std::vector<std::string> mHistory;
    std::vector<std::pair<std::string, double>> mSeconds;
    stack::DisplayCurve mCurve;
    std::optional<ser::FloatImage> mImage;  // the mineral image at --strength / --chroma-blur / --sharpen
    double sliderSeconds = 0;               // FinishMineral alone: what a slider move costs
    if (mineral && o.image && (mineralCrop[0] | mineralCrop[1] | mineralCrop[2] | mineralCrop[3])) {
        // --mineral-crop: the mineral moon on the stack cut to a field (B9's equal-field split-half check: the halves'
        // stacks reach 1-4 px apart on each side, and a chroma blur at a frame's edge sees only its own field). The
        // stack's noise is cut with it.
        const auto [L, T, R, B] = std::array<int, 4>{mineralCrop[0], mineralCrop[1], mineralCrop[2], mineralCrop[3]};
        ser::FloatImage& im = *o.image;
        const int cw = im.width - L - R, ch = im.height - T - B;
        if (cw < 16 || ch < 16) throw std::invalid_argument("--mineral-crop leaves under 16 x 16 px");
        ser::FloatImage cut{cw, ch, im.channels, std::vector<float>(static_cast<size_t>(cw) * ch * im.channels)};
        for (int y = 0; y < ch; ++y)
            std::copy_n(im.data.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(y + T) * im.width + L) * im.channels),
                        static_cast<size_t>(cw) * im.channels,
                        cut.data.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * cw * im.channels));
        im = std::move(cut);
        if (o.noise) {
            o.noise->width = cw;
            o.noise->height = ch;
            o.noise->originX += L;
            o.noise->originY += T;
        }
    }
    if (mineral && o.image) {
        mcache = stack::PrepareMineral(*o.image, threads, progress, o.noise ? &*o.noise : nullptr);
        stack::MineralParams p;
        p.strength = strength;
        p.chromaBlurSigma = chromaBlur;
        p.sharpen = sharpen;
        const auto t0 = std::chrono::steady_clock::now();
        stack::MineralOutcome finished = stack::FinishMineral(*mcache, p, threads);
        sliderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const ser::FloatImage& img = mImage.emplace(std::move(finished.image));
        mHistory = std::move(finished.history);
        mSeconds = std::move(finished.stepSeconds);
        if (!mineralOut.empty()) WriteFits(mineralOut, img);
        for (const auto& [k, file] : mineralAlso) {
            stack::MineralParams q = p;
            q.strength = k;
            WriteFits(file, stack::ApplyMineralColour(*mcache, q, threads));
        }
        for (const auto& [a, file] : sharpenAlso) {
            stack::MineralParams q = p;
            q.sharpen = a;
            WriteFits(file, stack::ApplyMineralColour(*mcache, q, threads));
        }
        // Steps 1-4 again on a copy (deterministic; the masks are checked equal), for the exact display curve and the
        // linear calibrated RGB, which the cache does not keep.
        ser::FloatImage lin = *o.image;
        std::vector<uint8_t> clipped(lin.data.size());
        for (size_t k = 0; k < lin.data.size(); ++k) clipped[k] = lin.data[k] >= stack::kClippedLuma;
        const stack::LunarMasks m = stack::SegmentLunar(lin, threads);
        if (m.disk != mcache->masks.disk || m.sky != mcache->masks.sky)
            throw std::runtime_error("--mineral: the replica's masks differ from PrepareMineral's");
        if (m.diskCount > 0) {
            stack::CalibrateColour(lin, m);
            stack::NeutraliseBackground(lin, m);
            ser::FloatImage disp = lin;
            stack::DisplayTransfer(disp, m, clipped, &mCurve);
        }
        if (!mineralStages.empty()) {
            const std::string d = mineralStages + "/";
            WriteFits(d + "display.fits", mcache->display);
            const ser::FloatImage lumSharp = stack::SharpenedLuminance(*mcache, sharpen, threads);
            WriteFits(d + "lum_sharp.fits", lumSharp);
            WriteFits(d + "lin_unsharp.fits", UnDisplay(Mean3(mcache->display), mCurve));
            WriteFits(d + "lin_sharp.fits", UnDisplay(lumSharp, mCurve));
            WriteFits(d + "calibrated.fits", lin);
            ser::FloatImage masks;
            masks.width = lin.width;
            masks.height = lin.height;
            masks.channels = 2;
            masks.data.resize(m.disk.size() * 2);
            for (size_t i = 0; i < m.disk.size(); ++i) {
                masks.data[i * 2] = m.disk[i];
                masks.data[i * 2 + 1] = m.sky[i];
            }
            WriteFits(d + "masks.fits", masks);
        }
    }
    // --planet: PreparePlanet + FinishPlanet (the window's two halves), timed as the window times a slider move.
    std::optional<stack::MineralCache> pcache;
    std::optional<ser::FloatImage> pImage;
    std::vector<std::string> pHistory;
    std::vector<std::pair<std::string, double>> pSeconds;
    double pSliderSeconds = 0;
    stack::DisplayCurve pCurve;
    std::optional<stack::PlanetDispersion> dispBefore, dispAfter;
    if (planet && o.image) {
        pcache = stack::PreparePlanet(*o.image, target, threads, progress, o.noise ? &*o.noise : nullptr);
        stack::PlanetParams p;
        p.saturation = saturation;
        p.chromaBlurSigma = chromaBlur;
        p.sharpen = sharpen;
        const auto t0 = std::chrono::steady_clock::now();
        stack::MineralOutcome finished = stack::FinishPlanet(*pcache, p, threads);
        pSliderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const ser::FloatImage& img = pImage.emplace(std::move(finished.image));
        pHistory = std::move(finished.history);
        pSeconds = std::move(finished.stepSeconds);
        if (!planetOut.empty()) WriteFits(planetOut, img);
        auto also = [&](const std::vector<std::pair<double, std::string>>& list, double stack::PlanetParams::*field) {
            for (const auto& [v, file] : list) {
                stack::PlanetParams q = p;
                q.*field = v;
                WriteFits(file, stack::FinishPlanet(*pcache, q, threads).image);
            }
        };
        also(sharpenAlso, &stack::PlanetParams::sharpen);
        also(chromaAlso, &stack::PlanetParams::chromaBlurSigma);
        also(saturationAlso, &stack::PlanetParams::saturation);
        // Steps 1-4 again on a copy (deterministic; the masks are checked equal): the linear calibrated RGB, the display
        // curve and the display before the alignment, which the cache does not keep; the dispersion is measured there
        // (before) and on the cache's display (after). The replica's own alignment and clamp must give the cache's
        // display bit for bit, or the "before" is not the cache's.
        ser::FloatImage lin = *o.image, unaligned;
        std::vector<uint8_t> clipped(lin.data.size());
        for (size_t k = 0; k < lin.data.size(); ++k) clipped[k] = lin.data[k] >= stack::kClippedLuma;
        const stack::LunarMasks m = stack::SegmentPlanet(lin, threads);
        if (m.disk != pcache->masks.disk || m.sky != pcache->masks.sky)
            throw std::runtime_error("--planet: the replica's masks differ from PreparePlanet's");
        if (m.diskCount > 0) {
            stack::CalibratePlanetColour(lin, m, target);
            stack::NeutraliseBackground(lin, m);
            unaligned = lin;
            stack::DisplayTransfer(unaligned, m, clipped, &pCurve, 0.0);
            ser::FloatImage aligned = unaligned;
            stack::PlanetDispersion before;
            const stack::StepRecord rec = stack::AlignPlanetChannels(aligned, m, threads, &before);
            if (!rec.skipped)
                for (float& v : aligned.data) v = std::clamp(v, 0.0f, 1.0f);
            if (aligned.data != pcache->display.data)
                throw std::runtime_error("--planet: the replica's aligned display differs from PreparePlanet's");
            dispBefore = before;
            dispAfter = stack::MeasurePlanetDispersion(pcache->display, m, threads);
        } else {
            unaligned = pcache->display;
        }
        if (!planetStages.empty()) {
            const std::string d = planetStages + "/";
            WriteFits(d + "display.fits", pcache->display);
            WriteFits(d + "display_unaligned.fits", unaligned);
            const ser::FloatImage lumSharp = stack::SharpenedLuminance(*pcache, sharpen, threads, nullptr, stack::kPlanetSharpen);
            WriteFits(d + "lum_sharp.fits", lumSharp);
            WriteFits(d + "lin_unsharp.fits", UnDisplay(Mean3(pcache->display), pCurve));
            WriteFits(d + "lin_sharp.fits", UnDisplay(lumSharp, pCurve));
            WriteFits(d + "calibrated.fits", lin);
            ser::FloatImage masks{lin.width, lin.height, 2, std::vector<float>(m.disk.size() * 2)};
            for (size_t i = 0; i < m.disk.size(); ++i) {
                masks.data[i * 2] = m.disk[i];
                masks.data[i * 2 + 1] = m.sky[i];
            }
            WriteFits(d + "masks.fits", masks);
        }
    }

    if (json) {
        std::string j = "{";
        j += "\"capture\":" + Json(path) + ",\"frames\":" + std::to_string(o.info.frameCount) + ",\"width\":" +
             std::to_string(o.info.header.width) + ",\"height\":" + std::to_string(o.info.header.height) +
             ",\"colour\":" + Json(ser::ColorIdName(o.info.colorId)) + ",\"undeclaredMosaic\":" +
             (stack::UndeclaredMosaic(o.info) ? "true" : "false") + ",\"threads\":" + std::to_string(threads) +
             ",\"blur\":" + Fmt(blur, 2) + ",\"parity\":" + (parity < 0 ? std::string("null") : Json(parity ? "odd" : "even"));
        j += ",\"diagnosis\":{\"kind\":" + Json(KindName(o.diagnosis.kind)) + ",\"message\":" + Json(o.diagnosis.message) + ",\"numbers\":{";
        for (size_t i = 0; i < o.diagnosis.numbers.size(); ++i)
            j += (i ? "," : "") + Json(o.diagnosis.numbers[i].first) + ":" + Fmt(o.diagnosis.numbers[i].second, 4);
        j += "}}";
        j += ",\"measured\":" + std::to_string(s.measured) + ",\"used\":" + std::to_string(s.used) + ",\"selection\":" +
             Json(s.selection) + ",\"alignment\":" + Json(s.alignment) + ",\"seconds\":" + Fmt(s.seconds) + ",\"phaseSeconds\":{";
        for (size_t i = 0; i < s.phaseSeconds.size(); ++i)
            j += (i ? "," : "") + Json(s.phaseSeconds[i].first) + ":" + Fmt(s.phaseSeconds[i].second);
        j += "},\"bytesRead\":" + std::to_string(s.bytesRead) + ",\"readMBps\":" + Fmt(s.readMBps, 1) + ",\"peakRssMB\":" +
             Fmt(peakMB, 1) + ",\"warnings\":[";
        for (size_t i = 0; i < s.warnings.size(); ++i) j += (i ? "," : "") + Json(s.warnings[i]);
        j += "],\"hotPixels\":" + std::to_string(s.hotPixels) + ",\"rejectedCells\":" + std::to_string(s.rejectedCells) +
             ",\"focus\":{\"outcome\":" + Json(FocusName(s.focus.outcome)) + ",\"disc\":" + (s.focus.disc ? "true" : "false") +
             ",\"measure\":" + Fmt(s.focus.measure) + ",\"warnAbove\":" + Fmt(s.focus.warnAbove) + ",\"refuseFrom\":" +
             Fmt(s.focus.refuseFrom) + ",\"detailScale\":" + Fmt(s.focus.detailScale) + ",\"limbBlur\":" + Fmt(s.focus.limbBlur) +
             ",\"note\":" + Json(s.focus.note) + ",\"radius\":" + Fmt(s.focus.radius, 2) + ",\"judged\":" +
             std::to_string(s.focus.judged) + ",\"windows\":" + std::to_string(s.focus.windows) + "}" +
             ",\"regions\":" + std::to_string(o.frames.regions) + ",\"admittedSuspects\":" +
             std::to_string(o.frames.admittedSuspects.size()) + ",\"rejectedSuspects\":" +
             std::to_string(o.frames.rejectedSuspects.size()) + ",\"alignmentExcluded\":" +
             std::to_string(o.frames.alignmentExcluded.size());
        // Per region (RegionPlan::detail): the bench's END models our stack's noise with these counts
        // (test/bench/metric.py, Ruling P28).
        j += ",\"regionDetail\":[";
        for (size_t r = 0; r < o.frames.regionDetail.size(); ++r) {
            const stack::RegionDetail& d = o.frames.regionDetail[r];
            j += std::string(r ? "," : "") + "{\"kept\":" + std::to_string(d.kept) + ",\"candidates\":" +
                 std::to_string(d.candidates) + ",\"noiseOnly\":" + (d.noiseOnly ? "true" : "false") + "}";
        }
        j += "]";
        j += ",\"memory\":{\"measureWorkers\":" + std::to_string(s.memory.measureWorkers) + ",\"alignWorkers\":" +
             std::to_string(s.memory.alignWorkers) + ",\"stackWorkers\":" + std::to_string(s.memory.stackWorkers) +
             ",\"observers\":" + std::to_string(s.memory.observers) + ",\"plannedPeakMB\":" + Fmt(s.memory.peakBytes / 1e6, 1) + "}";
        if (o.image) {
            const ser::FloatImage& im = *o.image;
            j += ",\"image\":{\"width\":" + std::to_string(im.width) + ",\"height\":" + std::to_string(im.height) +
                 ",\"channels\":" + std::to_string(im.channels) + "},\"stackedFirst\":" + std::to_string(captureFrame(o.frames.stacked.front())) +
                 ",\"stackedLast\":" + std::to_string(captureFrame(o.frames.stacked.back())) + ",\"crop\":{\"left\":" +
                 std::to_string(s.crop.left) + ",\"top\":" + std::to_string(s.crop.top) + ",\"right\":" + std::to_string(s.crop.right) +
                 ",\"bottom\":" + std::to_string(s.crop.bottom) + ",\"uncovered\":" + std::to_string(s.crop.uncovered) +
                 ",\"thin\":{\"left\":" + std::to_string(s.crop.thin[0]) + ",\"top\":" + std::to_string(s.crop.thin[1]) +
                 ",\"right\":" + std::to_string(s.crop.thin[2]) + ",\"bottom\":" + std::to_string(s.crop.thin[3]) + "}" +
                 "},\"guard\":" + std::to_string(s.crop.guard) + ",\"samples\":[";
            bool first = true;
            for (const auto& [x, y] : samples) {
                if (x < 0 || y < 0 || x >= im.width || y >= im.height) throw std::invalid_argument("--sample outside the image");
                for (int c = 0; c < im.channels; ++c) {
                    char v[48];
                    std::snprintf(v, sizeof v, "%.9g", im.data[(static_cast<size_t>(y) * im.width + x) * im.channels + c]);
                    j += std::string(first ? "" : ",") + "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                         ",\"c\":" + std::to_string(c) + ",\"v\":" + v + "}";
                    first = false;
                }
            }
            j += "]";
        }
        j += ",\"target\":" + Json(ser::TargetName(target)) + ",\"filter\":" + Json(FilterName(name.filter));
        if (mcache) {
            double total = 0;
            j += ",\"mineral\":{\"refusal\":null,\"notice\":" + Json(mcache->notice) + ",\"strength\":" + Fmt(strength, 2) +
                 ",\"chromaBlur\":" + Fmt(chromaBlur, 2) + ",\"sharpen\":" + Fmt(sharpen, 2) + ",\"sliderSeconds\":" +
                 Fmt(sliderSeconds, 4) + ",\"satThreshold\":" + Fmt(mcache->satThreshold, 6) +
                 ",\"curve\":{\"white\":" + Fmt(mCurve.white, 8) + ",\"balance\":" + Fmt(mCurve.balance, 8) + "},\"history\":[";
            for (size_t i = 0; i < mHistory.size(); ++i) j += (i ? "," : "") + Json(mHistory[i]);
            j += "],\"stepSeconds\":{";
            for (size_t i = 0; i < mSeconds.size(); ++i) {
                j += (i ? "," : "") + Json(mSeconds[i].first) + ":" + Fmt(mSeconds[i].second, 4);
                total += mSeconds[i].second;
            }
            j += "},\"seconds\":" + Fmt(total, 4) + ",\"output\":" + (mineralOut.empty() ? std::string("null") : Json(mineralOut)) +
                 ",\"also\":[";
            for (size_t i = 0; i < mineralAlso.size(); ++i)
                j += std::string(i ? "," : "") + "{\"strength\":" + Fmt(mineralAlso[i].first, 2) + ",\"output\":" +
                     Json(mineralAlso[i].second) + "}";
            j += "],\"sharpenAlso\":[";
            for (size_t i = 0; i < sharpenAlso.size(); ++i)
                j += std::string(i ? "," : "") + "{\"sharpen\":" + Fmt(sharpenAlso[i].first, 2) + ",\"output\":" +
                     Json(sharpenAlso[i].second) + "}";
            j += "],\"stages\":" + (mineralStages.empty() ? std::string("null") : Json(mineralStages));
            // Its smallest and largest sample and its value at every --sample point (Task B8: the headless probe
            // compares PlanetaryStack's _mineral window with them; --mineral-crop shifts the grid by L, T).
            const auto [lo, hi] = std::minmax_element(mImage->data.begin(), mImage->data.end());
            j += ",\"min\":" + Fmt(*lo, 6) + ",\"max\":" + Fmt(*hi, 6) + ",\"samples\":[";
            bool firstM = true;
            for (const auto& [x, y] : samples) {
                const int mx = x - mineralCrop[0], my = y - mineralCrop[1];
                if (mx < 0 || my < 0 || mx >= mImage->width || my >= mImage->height) continue;
                for (int c = 0; c < 3; ++c) {
                    char v[48];
                    std::snprintf(v, sizeof v, "%.9g", mImage->data[(static_cast<size_t>(my) * mImage->width + mx) * 3 + c]);
                    j += std::string(firstM ? "" : ",") + "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                         ",\"c\":" + std::to_string(c) + ",\"v\":" + v + "}";
                    firstM = false;
                }
            }
            j += "]}";
        }
        if (pcache) {
            double total = 0;
            const stack::LunarMasks& pm = pcache->masks;
            int bx0 = pm.width, bx1 = -1, by0 = pm.height, by1 = -1;
            for (int y = 0; y < pm.height; ++y)
                for (int x = 0; x < pm.width; ++x)
                    if (pm.disk[static_cast<size_t>(y) * pm.width + x]) {
                        bx0 = std::min(bx0, x);
                        bx1 = std::max(bx1, x);
                        by0 = std::min(by0, y);
                        by1 = std::max(by1, y);
                    }
            j += ",\"planet\":{\"refusal\":null,\"notice\":" + Json(pcache->notice) + ",\"saturation\":" + Fmt(saturation, 2) +
                 ",\"chromaBlur\":" + Fmt(chromaBlur, 2) + ",\"sharpen\":" + Fmt(sharpen, 2) + ",\"sliderSeconds\":" +
                 Fmt(pSliderSeconds, 4) + ",\"curve\":{\"white\":" + Fmt(pCurve.white, 8) + ",\"balance\":" +
                 Fmt(pCurve.balance, 8) + "},\"disc\":{\"pixels\":" + std::to_string(pm.diskCount) + ",\"sky\":" +
                 std::to_string(pm.skyCount) + ",\"equivRadius\":" + Fmt(std::sqrt(pm.diskCount / M_PI), 2) +
                 ",\"box\":" + (bx1 < 0 ? std::string("null")
                                        : "[" + std::to_string(bx0) + "," + std::to_string(by0) + "," + std::to_string(bx1) +
                                              "," + std::to_string(by1) + "]") +
                 ",\"reason\":" + Json(pm.reason) + "},\"dispersion\":{\"before\":" +
                 (dispBefore ? DispersionJson(*dispBefore) : std::string("null")) + ",\"after\":" +
                 (dispAfter ? DispersionJson(*dispAfter) : std::string("null")) + "},\"history\":[";
            for (size_t i = 0; i < pHistory.size(); ++i) j += (i ? "," : "") + Json(pHistory[i]);
            j += "],\"stepSeconds\":{";
            for (size_t i = 0; i < pSeconds.size(); ++i) {
                j += (i ? "," : "") + Json(pSeconds[i].first) + ":" + Fmt(pSeconds[i].second, 4);
                total += pSeconds[i].second;
            }
            j += "},\"seconds\":" + Fmt(total, 4) + ",\"output\":" + (planetOut.empty() ? std::string("null") : Json(planetOut));
            auto list = [&](const char* key, const char* field, const std::vector<std::pair<double, std::string>>& l) {
                j += std::string(",\"") + key + "\":[";
                for (size_t i = 0; i < l.size(); ++i)
                    j += std::string(i ? "," : "") + "{\"" + field + "\":" + Fmt(l[i].first, 3) + ",\"output\":" + Json(l[i].second) + "}";
                j += "]";
            };
            list("sharpenAlso", "sharpen", sharpenAlso);
            list("chromaBlurAlso", "chromaBlur", chromaAlso);
            list("saturationAlso", "saturation", saturationAlso);
            const auto [lo, hi] = std::minmax_element(pImage->data.begin(), pImage->data.end());
            j += ",\"stages\":" + (planetStages.empty() ? std::string("null") : Json(planetStages)) + ",\"min\":" + Fmt(*lo, 6) +
                 ",\"max\":" + Fmt(*hi, 6) + "}";
        }
        // The stack's noise (Ruling R35): the model's line and the noise-power frame count k_eff = 1 / alpha2 over the grid.
        if (o.noise) {
            std::vector<float> a2 = o.noise->alpha2;
            std::sort(a2.begin(), a2.end());
            const double med = a2.empty() ? 0 : a2[a2.size() / 2], lo = a2.empty() ? 0 : a2.front(), hi = a2.empty() ? 0 : a2.back();
            j += ",\"stackNoise\":{\"a\":" + Fmt(o.noise->model.a, 10) + ",\"b\":" + Fmt(o.noise->model.b, 10) + ",\"knots\":" +
                 std::to_string(o.noise->model.knots.size()) + ",\"grid\":" + std::to_string(o.noise->grid) + ",\"keffMedian\":" +
                 Fmt(med > 0 ? 1 / med : 0, 1) + ",\"keffMin\":" + Fmt(hi > 0 ? 1 / hi : 0, 1) + ",\"keffMax\":" +
                 Fmt(lo > 0 ? 1 / lo : 0, 1) + ",\"usable\":" + (o.noise->Usable() ? "true" : "false") + "}";
        }
        j += ",\"output\":" + (o.image && !out.empty() ? Json(out) : std::string("null")) + "}";
        std::printf("%s\n", j.c_str());
    } else {
        std::printf("capture   %s\n", path.c_str());
        std::printf("frames    %lld, %dx%d, %s%s, %d threads%s\n", static_cast<long long>(o.info.frameCount), o.info.header.width,
                    o.info.header.height, ser::ColorIdName(o.info.colorId), stack::UndeclaredMosaic(o.info) ? " (undeclared mosaic)" : "",
                    threads, blur > 0 ? (", blurred by sigma " + Fmt(blur, 2) + " px").c_str() : "");
        std::printf("diagnosis %s%s%s\n", KindName(o.diagnosis.kind), o.diagnosis.message.empty() ? "" : ": ", o.diagnosis.message.c_str());
        for (const auto& nb : o.diagnosis.numbers) std::printf("          %s = %s\n", nb.first.c_str(), Fmt(nb.second, 4).c_str());
        std::printf("selection %s\n", s.selection.c_str());
        std::printf("alignment %s\n", s.alignment.c_str());
        std::printf("used      %lld of %lld; %d regions\n", static_cast<long long>(s.used), static_cast<long long>(s.measured),
                    o.frames.regions);
        const stack::FocusCheck& fc = s.focus;
        std::printf("focus     %s, %s: measure %s (warn above %s, refuse from %s); detail %s px; limb blur %s px; "
                    "radius %.1f luma px; %d of %d windows judged%s%s\n",
                    FocusName(fc.outcome), fc.outcome == stack::FocusOutcome::NotJudged ? "nothing judged" : fc.disc ? "disc" : "frame-filling",
                    Fmt(fc.measure, 3).c_str(), Fmt(fc.warnAbove, 3).c_str(),
                    Fmt(fc.refuseFrom, 3).c_str(), Fmt(fc.detailScale, 2).c_str(), Fmt(fc.limbBlur, 2).c_str(),
                    fc.radius, fc.judged, fc.windows, fc.note.empty() ? "" : "; ", fc.note.c_str());
        std::printf("time      %.2f s:", s.seconds);
        for (const auto& p : s.phaseSeconds) std::printf(" %s %.2f;", p.first.c_str(), p.second);
        std::printf("\nread      %.1f MB at %.1f MB/s; peak RSS %.0f MB (planned engine peak %.0f MB; workers %d/%d/%d, observers %d)\n",
                    s.bytesRead / 1e6, s.readMBps, peakMB, s.memory.peakBytes / 1e6, s.memory.measureWorkers, s.memory.alignWorkers,
                    s.memory.stackWorkers, s.memory.observers);
        for (const auto& w : s.warnings) std::printf("warning   %s\n", w.c_str());
        if (o.image && !out.empty()) std::printf("wrote     %s\n", out.c_str());
        std::printf("target    %s, filter %s\n", ser::TargetName(target), FilterName(name.filter));
        if (mcache) {
            for (const auto& h : mHistory) std::printf("mineral   %s\n", h.c_str());
            double total = 0;
            std::printf("mineral   time:");
            for (const auto& p : mSeconds) {
                std::printf(" %s %.3f;", p.first.c_str(), p.second);
                total += p.second;
            }
            std::printf(" total %.3f s\n", total);
            if (!mcache->notice.empty()) std::printf("mineral   NOTICE: %s\n", mcache->notice.c_str());
            if (!mineralOut.empty()) std::printf("wrote     %s\n", mineralOut.c_str());
        }
        if (pcache) {
            for (const auto& h : pHistory) std::printf("planet    %s\n", h.c_str());
            double total = 0;
            std::printf("planet    time:");
            for (const auto& p : pSeconds) {
                std::printf(" %s %.3f;", p.first.c_str(), p.second);
                total += p.second;
            }
            std::printf(" total %.3f s; slider %.3f s\n", total, pSliderSeconds);
            if (!pcache->notice.empty()) std::printf("planet    NOTICE: %s\n", pcache->notice.c_str());
            if (!planetOut.empty()) std::printf("wrote     %s\n", planetOut.c_str());
        }
        // Focus profiles pooled over the windows judged: the share of the signal power at periods under
        // P sensor px, for a few P.
        {
            const double scale = stack::LumaScale(o.info);
            std::printf("focus     %zu windows;", o.frames.focus.size());
            for (double period : {3.0, 4.0, 6.0, 8.0, 12.0, 16.0}) {
                double fine = 0, all = 0;
                for (const auto& p : o.frames.focus) {
                    double snr = 0;
                    stack::FocusDetailScale(p, &snr);
                    if (!(snr >= stack::kMinFocusSnr)) continue;
                    for (size_t r = 1; r < p.cross.size(); ++r) {
                        all += p.cross[r];
                        if (static_cast<double>(r) / p.side >= scale / period) fine += p.cross[r];
                    }
                }
                std::printf(" share<%gpx %.4f;", period, all > 0 ? fine / all : 0.0);
            }
            std::printf("\n");
        }
        for (size_t r = 0; r < o.frames.regionDetail.size(); ++r) {
            const stack::RegionDetail& d = o.frames.regionDetail[r];
            std::printf("region    %3zu: best quality %.4f +- %.4f (detail %.2f luma px), kept %lld of %lld\n", r, d.bestQuality,
                        d.keptQualitySigma, d.detailScale, static_cast<long long>(d.kept), static_cast<long long>(d.candidates));
        }
    }
    // A summary lost to a full disk must not pass for one written (the GUI smoke read an empty JSON as "no warning").
    if (std::fflush(stdout) != 0 || std::ferror(stdout)) throw std::runtime_error("cannot write the summary to standard output (disk full?)");
    return o.image ? 0 : 2;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
}
