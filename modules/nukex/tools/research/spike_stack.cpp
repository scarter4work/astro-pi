// NukeX research driver — runs StackingEngine headless, with no PixInsight.
//
// Rebuilt 2026-09-08. The 2026-09-07 original lived in a scratch directory and
// was lost with it; this copy is in the repo precisely so that does not happen
// a third time.
//
// Usage:  spike_stack <lights-dir> [cache-dir]
//
// It exists to drive the fitting instrumentation in model_selector.cpp
// (NUKEX_DUMP_VOXELS) and gpu_executor.cpp (NUKEX_DUMP_MU) over a real corpus
// without borrowing /opt/PixInsight/bin, which a running imaging session
// cannot tolerate.
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/progress_observer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Prints one line per phase, so a long run is legible in a log without the
// per-frame firehose the PixInsight observer emits.
class PhaseObserver : public nukex::ProgressObserver {
public:
    void begin_phase(const std::string& name, int steps) override {
        phase_ = name;
        t0_    = std::chrono::steady_clock::now();
        std::printf("[phase ] %-32s (%d steps)\n", name.c_str(), steps);
        std::fflush(stdout);
    }
    void advance(int, const std::string&) override {}
    void end_phase() override {
        const double s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0_).count();
        std::printf("[done  ] %-32s %8.1f s\n", phase_.c_str(), s);
        std::fflush(stdout);
    }
    bool is_cancelled() const override { return false; }
    void message(const std::string& m) override {
        std::printf("[msg   ] %s\n", m.c_str());
        std::fflush(stdout);
    }
private:
    std::string phase_;
    std::chrono::steady_clock::time_point t0_;
};

std::vector<std::string> fits_in(const fs::path& dir) {
    std::vector<std::string> out;
    if (!fs::is_directory(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".fit" || ext == ".fits" || ext == ".fts")
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());   // deterministic frame order
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <lights-dir> [cache-dir]\n", argv[0]);
        return 2;
    }
    std::vector<std::string> lights = fits_in(argv[1]);
    // Some E2E cases cap the frame count (the M16 dual-NB case uses 12 of 88),
    // and a comparison is only fair against the same frames.
    if (const char* lim = std::getenv("NUKEX_SPIKE_MAX_FRAMES")) {
        const size_t k = std::strtoul(lim, nullptr, 10);
        if (k > 0 && k < lights.size()) lights.resize(k);
    }
    if (lights.empty()) {
        std::fprintf(stderr, "no FITS files under %s\n", argv[1]);
        return 2;
    }

    nukex::StackingEngine::Config cfg;
    // NOT /tmp: it is tmpfs on this box, and the frame cache exists to keep
    // frame data out of RAM.
    cfg.cache_dir = (argc > 2) ? argv[2]
                               : std::string(getenv("HOME")) + "/.cache/nukex_spike_frames";
    fs::create_directories(cfg.cache_dir);

    // The controlled experiment the fitting question needs: per-frame sky
    // normalisation ON vs OFF over the SAME corpus. The hypothesis is that
    // normalisation removes the spurious per-frame bimodality the GMM was
    // winning on, so its win rate should collapse when this is on.
    if (const char* s = std::getenv("NUKEX_SPIKE_NO_NORM")) {
        if (*s && *s != '0') {
            cfg.normalize_frames = false;
            std::printf("*** per-frame sky normalisation DISABLED ***\n");
        }
    }

    std::printf("corpus : %s\n", argv[1]);
    std::printf("frames : %zu\n", lights.size());
    std::printf("cache  : %s\n\n", cfg.cache_dir.c_str());

    PhaseObserver obs;
    nukex::StackingEngine engine(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    auto result = engine.execute(lights, {}, &obs);
    const double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    if (!result.ok) {
        std::fprintf(stderr, "\nFAILED: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("\nok: %d frames processed, %d failed alignment, %.1f s wall\n",
                result.n_frames_processed, result.n_frames_failed_alignment, wall);
    return 0;
}
