#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/io/image.hpp"
#include <filesystem>

using namespace nukex;

TEST_CASE("StackingEngine: empty input produces empty result", "[engine]") {
    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute({}, {});
    REQUIRE(result.n_frames_processed == 0);
    REQUIRE(result.stacked.empty());
    REQUIRE(result.noise_map.empty());
    REQUIRE(result.quality_map.empty());
    REQUIRE(result.n_frames_failed_alignment == 0);
}

// Integration test with real FITS data.  Tagged [.integration] so it
// is skipped by default (takes 30s+ on typical hardware, enough to
// trip the default ctest 30s timeout).  Invoke explicitly with:
//   ./test/test_stacking_engine [integration]
TEST_CASE("StackingEngine: M16 integration test", "[.integration][engine]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    // Find first 5 FITS files
    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 5) break;
        }
    }
    if (lights.size() < 3) {
        SKIP("Not enough FITS files in " + data_dir);
    }

    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {});
    REQUIRE(result.n_frames_processed >= 3);
    REQUIRE(!result.stacked.empty());
    REQUIRE(result.stacked.width() > 0);
    REQUIRE(result.stacked.height() > 0);
    REQUIRE(!result.noise_map.empty());
    REQUIRE(!result.quality_map.empty());
    REQUIRE(result.quality_map.n_channels() == 4);
}

#include "nukex/core/progress_observer.hpp"

// ── Test helper: records all observer calls ──────────────────────

class RecordingObserver : public nukex::ProgressObserver {
public:
    struct Event {
        enum Type { BEGIN, ADVANCE, END, MESSAGE } type;
        std::string text;
        int value = 0;
    };

    std::vector<Event> events;
    bool cancelled = false;
    int cancel_after_advances = -1;  // cancel after N advance(steps>0) calls
    int advance_count = 0;

    void begin_phase(const std::string& name, int total) override {
        events.push_back({Event::BEGIN, name, total});
    }
    void advance(int steps, const std::string& detail) override {
        events.push_back({Event::ADVANCE, detail, steps});
        if (steps > 0) {
            advance_count++;
            if (cancel_after_advances > 0 && advance_count >= cancel_after_advances) {
                cancelled = true;
            }
        }
    }
    void end_phase() override {
        events.push_back({Event::END, {}, 0});
    }
    void message(const std::string& text) override {
        events.push_back({Event::MESSAGE, text, 0});
    }
    bool is_cancelled() const override { return cancelled; }

    // Count how many BEGIN events have a matching END
    bool all_phases_closed() const {
        int depth = 0;
        for (const auto& e : events) {
            if (e.type == Event::BEGIN) depth++;
            if (e.type == Event::END) depth--;
            if (depth < 0) return false;  // extra END
        }
        return depth == 0;
    }

    int count_type(Event::Type t) const {
        int n = 0;
        for (const auto& e : events) if (e.type == t) n++;
        return n;
    }

    int count_bar_advances() const {
        int n = 0;
        for (const auto& e : events)
            if (e.type == Event::ADVANCE && e.value > 0) n++;
        return n;
    }
};

TEST_CASE("StackingEngine: observer receives no calls for empty input", "[engine][progress]") {
    RecordingObserver obs;
    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute({}, {}, &obs);
    REQUIRE(result.n_frames_processed == 0);
    // Empty input returns before any phases start
    REQUIRE(obs.events.empty());
}

TEST_CASE("StackingEngine: observer phases are balanced", "[.integration][engine][progress]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 3) break;
        }
    }
    if (lights.size() < 3) SKIP("Not enough FITS files");

    RecordingObserver obs;
    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {}, &obs);
    REQUIRE(result.n_frames_processed >= 3);

    // All begin_phase/end_phase pairs are balanced
    REQUIRE(obs.all_phases_closed());

    // At least 3 phases: A, B, C
    REQUIRE(obs.count_type(RecordingObserver::Event::BEGIN) >= 3);

    // Progress bar advances at least once per frame in Phase A
    REQUIRE(obs.count_bar_advances() >= static_cast<int>(lights.size()));
}

TEST_CASE("StackingEngine: cancellation mid-Phase-A returns partial result", "[.integration][engine][progress]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 5) break;
        }
    }
    if (lights.size() < 5) SKIP("Need at least 5 FITS files");

    RecordingObserver obs;
    obs.cancel_after_advances = 2;  // Cancel after 2 frames

    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {}, &obs);

    // Should have processed some but not all frames
    REQUIRE(result.n_frames_processed >= 1);
    REQUIRE(result.n_frames_processed < static_cast<int>(lights.size()));

    // Phases should still be balanced (early return closes phases)
    REQUIRE(obs.all_phases_closed());
}

TEST_CASE("StackingEngine: cancellation mid-Phase-B returns partial result", "[.integration][engine][progress]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 3) break;
        }
    }
    if (lights.size() < 3) SKIP("Not enough FITS files");

    // Set cancel_after_advances high enough to pass all of Phase A
    // (one advance per frame = 3), then cancel during Phase B batches.
    // Phase A produces 3 bar advances. Phase B starts after that.
    // Cancel at advance 5 = during Phase B batch processing.
    RecordingObserver obs;
    obs.cancel_after_advances = 5;

    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {}, &obs);

    // All frames should have been processed (Phase A completed)
    REQUIRE(result.n_frames_processed == static_cast<int>(lights.size()));

    // Phases should still be balanced (cancellation closes phases properly)
    REQUIRE(obs.all_phases_closed());

    // Should have cancellation message
    bool found_cancel_msg = false;
    for (const auto& e : obs.events) {
        if (e.type == RecordingObserver::Event::MESSAGE &&
            e.text.find("Cancelled") != std::string::npos) {
            found_cancel_msg = true;
            break;
        }
    }
    REQUIRE(found_cancel_msg);
}
