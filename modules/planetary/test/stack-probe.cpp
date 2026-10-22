// Test-only CLI over RunStack (Task 6), for real-data smoke runs and the benchmark harness (Task 9).
// Never shipped, not a ctest case. Reads the video only; writes only --out.
//
//   stack-probe <video> [--bayer auto|none|RGGB|GRBG|GBRG|BGGR] [--threads N] [--out stack.fits|stack.tif]
//               [--bits 16|32] [--json] [--frames N] [--blur S]
//
// Runs RunStack on the video (default: Auto pattern, every hardware thread) and prints the summary --
// as one JSON object with --json, else as plain lines -- including the phase times, the read
// throughput and the process's peak resident memory (getrusage). Progress goes to stderr.
// --out writes the stack: FITS (.fits/.fit, 32-bit float, NAXIS3 = 3 planes for colour, first row =
// top of the image) or TIFF (.tif/.tiff, uncompressed, interleaved, --bits 16 (default; [0, 1] scaled to
// 0..65535) or 32 (IEEE float)). Probe-only knobs, both through a reader wrapper: --frames N stacks the
// first N frames; --blur S blurs every frame by a Gaussian of sigma S sensor px before the engine sees it
// (each colour site plane on its own for a mosaic, each channel for RGB): a synthetic defocus of a real
// capture, for calibrating the focus floor. Exit 0 with an image, 2 when diagnosed, 1 on error.
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <random>
#include <thread>
#include <vector>

#include "Engine.h"
#include "Measure.h"
#include "SerFile.h"

using namespace planetary;

namespace {

// --frames and --blur.
class ProbeSource : public ser::FrameSource {
public:
    ProbeSource(std::unique_ptr<ser::FrameSource> s, int64_t limit, double blur)
        : s_(std::move(s)), info_(s_->Info()), blur_(blur) {
        info_.frameCount = std::min(info_.frameCount, limit);
        step_ = ser::IsBayer(info_.colorId) || stack::UndeclaredMosaic(info_) ? 2 : 1;
        if (blur_ > 0) {
            const double sg = blur_ / step_;  // in site-plane px
            const int r = static_cast<int>(std::ceil(3 * sg));
            double sum = 0;
            for (int k = -r; k <= r; ++k) {
                kernel_.push_back(std::exp(-0.5 * k * k / (sg * sg)));
                sum += kernel_.back();
            }
            for (double& k : kernel_) k /= sum;
        }
    }
    const ser::FileInfo& Info() const override { return info_; }
    ser::Frame ReadFrame(int64_t i) const override {
        if (i >= info_.frameCount) throw ser::SerError("stack-probe: frame past --frames");
        ser::Frame f = s_->ReadFrame(i);
        if (blur_ > 0) Blur(f, i);
        return f;
    }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return s_->FrameTimestamp(i); }

private:
    // Separable Gaussian over each plane of same-colour samples (stride step_ in x and y, per channel),
    // mirrored at the plane's edges; then the plane's own pixel noise is put back (the blur removed it, a
    // defocused capture keeps it): Gaussian noise of the plane's EstimatePixelNoise before blurring (8-bit
    // quantisation included), seeded by the frame index, and the result rounded to whole DN again.
    void Blur(ser::Frame& f, int64_t index) const {
        const int W = f.width, H = f.height, C = f.channels, r = static_cast<int>(kernel_.size() / 2);
        std::vector<double> in(static_cast<size_t>(W) * H * C), tmp(in.size());
        for (size_t k = 0; k < in.size(); ++k) in[k] = f.samples[k];
        std::mt19937 rng(static_cast<unsigned>(1234567 + index));
        std::normal_distribution<double> gauss(0, 1);
        auto mirror = [](int i, int n) {
            if (n == 1) return 0;
            while (i < 0 || i >= n) i = i < 0 ? -i : 2 * (n - 1) - i;
            return i;
        };
        for (int py = 0; py < step_; ++py)
            for (int px = 0; px < step_; ++px)
                for (int c = 0; c < C; ++c) {
                    const int pw = (W - px + step_ - 1) / step_, ph = (H - py + step_ - 1) / step_;
                    auto at = [&](std::vector<double>& v, int x, int y) -> double& {
                        return v[(static_cast<size_t>(py + y * step_) * W + (px + x * step_)) * C + c];
                    };
                    ser::FloatImage plane{pw, ph, 1, std::vector<float>(static_cast<size_t>(pw) * ph)};
                    for (int y = 0; y < ph; ++y)
                        for (int x = 0; x < pw; ++x) plane.data[static_cast<size_t>(y) * pw + x] = static_cast<float>(at(in, x, y));
                    const double noise = stack::EstimatePixelNoise(plane).sigma;  // in DN (the plane is not normalised)
                    for (int y = 0; y < ph; ++y)
                        for (int x = 0; x < pw; ++x) {
                            double s = 0;
                            for (int k = -r; k <= r; ++k) s += kernel_[static_cast<size_t>(k + r)] * at(in, mirror(x + k, pw), y);
                            at(tmp, x, y) = s;
                        }
                    for (int y = 0; y < ph; ++y)
                        for (int x = 0; x < pw; ++x) {
                            double s = 0;
                            for (int k = -r; k <= r; ++k) s += kernel_[static_cast<size_t>(k + r)] * at(tmp, x, mirror(y + k, ph));
                            at(in, x, y) = s + noise * gauss(rng);
                        }
                }
        for (size_t k = 0; k < in.size(); ++k) f.samples[k] = static_cast<uint16_t>(std::clamp(std::lround(in[k]), 0L, 65535L));
    }

    std::unique_ptr<ser::FrameSource> s_;
    ser::FileInfo info_;
    double blur_;
    int step_ = 1;
    std::vector<double> kernel_;
};

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

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: stack-probe <video> [--bayer auto|none|RGGB|GRBG|GBRG|BGGR] [--threads N] "
                             "[--out stack.fits|stack.tif] [--bits 16|32] [--json] [--frames N] [--blur S]\n");
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
    const stack::SourceFactory factory = [&]() -> std::unique_ptr<ser::FrameSource> {
        return std::make_unique<ProbeSource>(ser::OpenVideo(path, bayer), limit, blur);
    };
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
        j += ",\"memory\":{\"measureWorkers\":" + std::to_string(s.memory.measureWorkers) + ",\"alignWorkers\":" +
             std::to_string(s.memory.alignWorkers) + ",\"stackWorkers\":" + std::to_string(s.memory.stackWorkers) +
             ",\"observers\":" + std::to_string(s.memory.observers) + ",\"plannedPeakMB\":" + Fmt(s.memory.peakBytes / 1e6, 1) + "}";
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
