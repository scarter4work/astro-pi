// Test-only CLI over RunStack (Task 6), for real-data smoke runs and the benchmark harness (Task 9).
// Never shipped, not a ctest case. Reads the video only; writes only --out.
//
//   stack-probe <video> [--bayer auto|none|RGGB|GRBG|GBRG|BGGR] [--threads N] [--out stack.fits|stack.tif]
//               [--bits 16|32] [--json] [--frames N] [--blur S] [--keep-best F] [--sample X,Y]...
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
// adds the stack's value at pixel (X, Y)
// in every channel to the JSON ("samples"; the headless PixInsight probe compares PlanetaryStack's window
// with them, test/run-probe-stack.sh), next to the image's size ("image") and the first and last frame
// stacked ("stackedFirst", "stackedLast"). Exit 0 with an image, 2 when diagnosed, 1 on error.
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <random>
#include <thread>
#include <vector>

#include "Engine.h"
#include "Measure.h"
#include "Noise.h"
#include "ProbeSource.h"
#include "SerFile.h"

using namespace planetary;

using probe::ProbeSource;

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

void WriteFits(const std::string& path, const ser::FloatImage& im) {
    std::vector<std::string> cards;
    auto card = [&](const std::string& key, const std::string& value) {
        std::string c = key;
        c.resize(8, ' ');
        c += "= ";
        std::string v = value;
        if (v.size() < 20) v = std::string(20 - v.size(), ' ') + v;
        c += v;
        c.resize(80, ' ');
        cards.push_back(c);
    };
    card("SIMPLE", "T");
    card("BITPIX", "-32");
    card("NAXIS", im.channels == 1 ? "2" : "3");
    card("NAXIS1", std::to_string(im.width));
    card("NAXIS2", std::to_string(im.height));
    if (im.channels != 1) card("NAXIS3", std::to_string(im.channels));
    std::string end = "END";
    end.resize(80, ' ');
    cards.push_back(end);
    std::string header;
    for (const auto& c : cards) header += c;
    header.resize((header.size() + 2879) / 2880 * 2880, ' ');
    std::vector<uint8_t> data;
    data.reserve(im.data.size() * 4);
    for (int c = 0; c < im.channels; ++c)
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x) {
                const float v = im.data[(static_cast<size_t>(y) * im.width + x) * im.channels + c];
                uint32_t u;
                std::memcpy(&u, &v, 4);
                for (int k = 3; k >= 0; --k) data.push_back(static_cast<uint8_t>(u >> (8 * k)));
            }
    data.resize((data.size() + 2879) / 2880 * 2880, 0);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fwrite(header.data(), 1, header.size(), f);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

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
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

bool EndsWith(const std::string& s, const std::string& t) { return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }

// --keep-best F: the engine sees only these frames of the source (ascending), as frames 0..n-1.
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
        if (i < 0 || i >= static_cast<int64_t>(keep_->size())) throw ser::SerError("stack-probe: frame past --keep-best");
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
                             "[--out stack.fits|stack.tif] [--bits 16|32] [--json] [--frames N] [--blur S] [--keep-best F] [--sample X,Y]...\n");
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
    std::vector<std::pair<int, int>> samples;
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
        else if (a == "--sample") {
            const std::string v = next();
            const size_t comma = v.find(',');
            if (comma == std::string::npos) throw std::invalid_argument("--sample: X,Y");
            samples.emplace_back(std::stoi(v.substr(0, comma)), std::stoi(v.substr(comma + 1)));
        }
        else throw std::invalid_argument("unknown argument " + a);
    }
    if (bits != 16 && bits != 32) throw std::invalid_argument("--bits: 16 or 32");

    stack::Progress progress;
    auto lastPrint = std::chrono::steady_clock::now();
    std::string lastPhase;
    progress.report = [&](const std::string& phase, int64_t done, int64_t total) {
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
    const stack::StackOutcome o = stack::RunStack(factory, threads, progress);
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double peakMB = ru.ru_maxrss / 1024.0;  // KiB on Linux
    const stack::StackSummary& s = o.summary;

    if (o.image && !out.empty()) {
        if (EndsWith(out, ".fits") || EndsWith(out, ".fit")) WriteFits(out, *o.image);
        else if (EndsWith(out, ".tif") || EndsWith(out, ".tiff")) WriteTiff(out, *o.image, bits);
        else throw std::invalid_argument("--out: .fits/.fit or .tif/.tiff");
    }


    if (json) {
        std::string j = "{";
        j += "\"capture\":" + Json(path) + ",\"frames\":" + std::to_string(o.info.frameCount) + ",\"width\":" +
             std::to_string(o.info.header.width) + ",\"height\":" + std::to_string(o.info.header.height) +
             ",\"colour\":" + Json(ser::ColorIdName(o.info.colorId)) + ",\"undeclaredMosaic\":" +
             (stack::UndeclaredMosaic(o.info) ? "true" : "false") + ",\"threads\":" + std::to_string(threads) +
             ",\"blur\":" + Fmt(blur, 2);
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
                 ",\"channels\":" + std::to_string(im.channels) + "},\"stackedFirst\":" + std::to_string(o.frames.stacked.front()) +
                 ",\"stackedLast\":" + std::to_string(o.frames.stacked.back()) + ",\"samples\":[";
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
    return o.image ? 0 : 2;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
}
