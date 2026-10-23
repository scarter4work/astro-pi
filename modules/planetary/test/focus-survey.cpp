// Test-only CLI: Select's step-7 focus reading on a real capture, exactly as Select takes it
// (Selection::focusReading / focusLimit / focusQuality), for the in-focus survey and the defocus series
// behind kGrossBlurLimit and kBlurLimit (Select.h step 7; test/run-focus-survey.sh runs it). Never shipped,
// not a ctest case: it needs the NAS captures, which it only reads.
//
//   focus-survey <video> [--frames N] [--blur S] [--threads N] [--dump-stats]
//
// MeasureAll + SelectFrames on the capture (--frames / --blur: test/ProbeSource.h, as stack-probe), then
// one line: path | blur | frames | r50 (median quality / qualitySigma) | R_h | class (large / small / -) |
// reading (blur over the judged radius) | warn and limit (the same) | S = reading / limit | the quality read
// at | verdict (stacked, warned -- step 7's warn band, Ruling P15b -- or the diagnosis) | the selection's
// explanation. --dump-stats prints instead the FrameStats SelectFrames reads, as C++
// initialisers for a test fixture (test_select's real-capture cases). Exit 0, or 1 on error.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ProbeSource.h"
#include "Select.h"

using namespace planetary;

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: focus-survey <video> [--frames N] [--blur S] [--threads N] [--dump-stats]\n");
        return 1;
    }
    const std::string path = argv[1];
    double blur = 0;
    int64_t limit = INT64_MAX;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    bool dump = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dump-stats") {
            dump = true;
            continue;
        }
        if (i + 1 >= argc) throw std::invalid_argument("focus-survey: " + a + " needs a value");
        if (a == "--blur") blur = std::stod(argv[++i]);
        else if (a == "--frames") limit = std::stoll(argv[++i]);
        else if (a == "--threads") threads = std::stoi(argv[++i]);
        else throw std::invalid_argument("focus-survey: unknown option " + a);
    }
    const ser::BayerChoice bayer = ser::BayerChoice::Auto;
    const auto noise = blur > 0 ? probe::UnblurredNoise(path, bayer, limit, threads) : nullptr;
    const stack::SourceFactory factory = [&]() -> std::unique_ptr<ser::FrameSource> {
        return std::make_unique<probe::ProbeSource>(ser::OpenVideo(path, bayer), limit, blur, noise);
    };
    const ser::FileInfo info = factory()->Info();
    const std::vector<stack::FrameStats> stats = stack::MeasureAll(factory, info, threads, {});
    const stack::Selection sel = stack::SelectFrames(stats, info);
    if (dump) {
        std::printf("// %s, %lld frames%s: %dx%d, colour %d, depth %d; reading %.4f (limit %.4f), quality %.6f\n", path.c_str(),
                    static_cast<long long>(stats.size()), blur > 0 ? (", blurred " + std::to_string(blur) + " px").c_str() : "",
                    info.header.width, info.header.height, static_cast<int>(info.colorId), info.normDepth, sel.focusReading,
                    sel.focusLimit, sel.focusQuality);
        for (const auto& s : stats)
            std::printf("    {%lld, %d, %.4f, %.4f, %d, %d, %d, %d, %.2f, %.6f, %.6f, %.8g, %.8g, %.8g},\n",
                        static_cast<long long>(s.index), s.targetFound ? 1 : 0, s.cx, s.cy, s.bx0, s.by0, s.bx1, s.by1, s.halfRadius,
                        s.brightness, s.clippedFraction, s.quality, s.qualitySigma, s.quality2Sigma);
        return 0;
    }
    std::vector<double> r;
    for (const auto& s : stats)
        if (s.targetFound && s.quality > 0 && s.qualitySigma > 0) r.push_back(s.quality / s.qualitySigma);
    std::sort(r.begin(), r.end());
    const char* kind = sel.diagnosis.kind == stack::DiagnosisKind::None         ? (sel.focusWarning.empty() ? "stacked" : "warned")
                       : sel.diagnosis.kind == stack::DiagnosisKind::OutOfFocus ? "OutOfFocus"
                       : sel.diagnosis.kind == stack::DiagnosisKind::PoorSeeing ? "PoorSeeing"
                       : sel.diagnosis.kind == stack::DiagnosisKind::NoTarget   ? "NoTarget"
                                                                                : "other";
    std::printf("%s | blur %.1f | frames %lld | r50 %.1f | R_h %.1f | %s | reading %.4f | warn %.4f | limit %.4f | S %.3f | q %.5f | %s | %s\n",
                path.c_str(), blur, static_cast<long long>(stats.size()), r.empty() ? 0.0 : r[r.size() / 2], sel.targetRadius,
                !sel.focusJudged ? "-" : sel.targetRadius < stack::kMinJudgedRadius ? "small" : "large", sel.focusReading,
                sel.focusWarn, sel.focusLimit, sel.focusLimit > 0 ? sel.focusReading / sel.focusLimit : 0.0, sel.focusQuality, kind,
                sel.explanation.substr(0, 120).c_str());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "focus-survey: %s\n", e.what());
    return 1;
}
