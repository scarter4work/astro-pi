### Task 20: Synthetic-FITS writer; un-gate the 8 integration tests; add the generic-camera case

**State today.** `test/integration/test_phase_a_router.cpp` and `test_phase_b_qsolve.cpp` hold their real bodies inside `#if WIRED_BY_TASK_20` with a `#define WIRED_BY_TASK_20 0` and `SKIP(...)` placeholders. They call `test_util::write_synthetic_bayer / write_synthetic_mono / write_synthetic_q_solved_hao3 / write_synthetic_q_solved_s2o3` and include `test_data_loader.hpp` under `using namespace nukex;`, so the writer lives in `namespace nukex::test_util`. `test_util` is a static library declared in `test/CMakeLists.txt` (not `test/util/CMakeLists.txt`). No synthetic FITS writer exists anywhere in the tree.

**Files:**
- Create: `test/util/synthetic_fits.hpp`, `test/util/synthetic_fits.cpp`
- Modify: `test/CMakeLists.txt` (`test_util` sources + links)
- Modify: `test/integration/test_phase_a_router.cpp`, `test/integration/test_phase_b_qsolve.cpp`

**Interfaces:**
- Consumes: `nukex::QEDatabase::load_shipped`, `nukex::ChannelDecomposer::build_q(camera, filter) -> Eigen::MatrixXd (3×N)`, cfitsio.
- Produces (namespace `nukex::test_util`):

```cpp
void write_synthetic_bayer(const std::string& path, int w, int h, const std::string& bayer,
                           const std::string& instrument, const std::string& filter, float uniform_value);
void write_synthetic_mono (const std::string& path, int w, int h,
                           const std::string& instrument, const std::string& filter, float uniform_value);
void write_synthetic_q_solved_hao3(const std::string& path, int w, int h, const std::string& camera,
                                   float ha, float oiii, const std::string& instrume = "");
void write_synthetic_q_solved_s2o3(const std::string& path, int w, int h, const std::string& camera,
                                   float sii, float oiii, const std::string& instrume = "");
```
`instrume` empty → the camera name is written as INSTRUME; non-empty → written verbatim (lets a test exercise the Task 15b generic fallback while the Q matrix still comes from `camera`).

- [ ] **Step 1: Write the writer's own unit test** — create `test/unit/io/test_synthetic_fits.cpp`

```cpp
#include "catch_amalgamated.hpp"
#include "synthetic_fits.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("synthetic_fits: Bayer frame round-trips headers, size and photosite layout", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_bayer.fits";
    test_util::write_synthetic_bayer(p.string(), 32, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.image.width()  == 32);
    REQUIRE(r.image.height() == 16);
    REQUIRE(r.image.n_channels() == 1);
    REQUIRE(r.metadata.bayer_pattern == "RGGB");
    REQUIRE(r.metadata.instrument    == "ASI585MC");
    REQUIRE(r.metadata.filter        == "HaO3");
    REQUIRE(r.image.at(0, 0, 0) == Catch::Approx(0.5f));
}

TEST_CASE("synthetic_fits: mono frame has no BAYERPAT", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_mono.fits";
    test_util::write_synthetic_mono(p.string(), 8, 8, "ASI2600MM", "L", 0.25f);
    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.metadata.bayer_pattern.empty());
    REQUIRE(r.metadata.filter == "L");
}

TEST_CASE("synthetic_fits: q-solved HaO3 frame's photosites invert back to the targets", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_qsolved.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 8, 8, "ASI585MC", 0.5f, 0.3f);
    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);

    QEDatabase db;
    REQUIRE(db.load_shipped(std::string(NUKEX_TEST_FIXTURES_DIR) + "/qe/minimal_db.json").ok);
    ChannelDecomposer dec(db);
    // RGGB: (0,0)=R, (1,0)=G, (1,1)=B
    Eigen::Vector3d rgb(r.image.at(0, 0, 0), r.image.at(1, 0, 0), r.image.at(1, 1, 0));
    Eigen::VectorXd lines = dec.solve("ASI585MC", "HaO3", rgb);
    REQUIRE(lines(0) == Catch::Approx(0.5).margin(1e-6));
    REQUIRE(lines(1) == Catch::Approx(0.3).margin(1e-6));
}

TEST_CASE("synthetic_fits: instrume override is written verbatim", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_instrume.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 8, 8, "ASI585MC", 0.5f, 0.3f, "Unknown Cam X");
    REQUIRE(FITSReader::read_headers(p.string()).instrument == "Unknown Cam X");
}
```

Register in `test/CMakeLists.txt` after the `test_util` block:

```cmake
nukex_add_test(test_synthetic_fits unit/io/test_synthetic_fits.cpp test_util nukex4_calibration nukex4_io)
```

- [ ] **Step 2: Run to verify failure**

Run: `cd build && cmake .. 2>&1 | tail -1 && make test_synthetic_fits 2>&1 | grep -E "error" | head -2`
Expected: `synthetic_fits.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `test/util/synthetic_fits.hpp`

```cpp
#pragma once
#include <string>

namespace nukex { namespace test_util {

// Single-frame FITS writers for integration tests. Float32 pixels; headers
// BAYERPAT / INSTRUME / FILTER written only when non-empty.
void write_synthetic_bayer(const std::string& path, int w, int h, const std::string& bayer,
                           const std::string& instrument, const std::string& filter, float uniform_value);

void write_synthetic_mono(const std::string& path, int w, int h,
                          const std::string& instrument, const std::string& filter, float uniform_value);

// RGGB Bayer frames whose (R, G, B) photosite values are Q * (line1, line2)
// for the (camera, filter) Q matrix from test/fixtures/qe/minimal_db.json,
// so a Q-solve recovers exactly (line1, line2) at every pixel.
// `instrume` empty -> INSTRUME = camera; otherwise written verbatim.
void write_synthetic_q_solved_hao3(const std::string& path, int w, int h, const std::string& camera,
                                   float ha, float oiii, const std::string& instrume = "");
void write_synthetic_q_solved_s2o3(const std::string& path, int w, int h, const std::string& camera,
                                   float sii, float oiii, const std::string& instrume = "");

}} // namespace nukex::test_util
```

`test/util/synthetic_fits.cpp`:

```cpp
#include "synthetic_fits.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"

#include <fitsio.h>
#include <Eigen/Dense>

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace nukex { namespace test_util {

namespace {

void write_fits(const std::string& path, int w, int h, const std::vector<float>& pixels,
                const std::string& bayer, const std::string& instrument, const std::string& filter) {
    fitsfile* fp = nullptr;
    int status = 0;
    std::remove(path.c_str());
    fits_create_file(&fp, path.c_str(), &status);
    long naxes[2] = {w, h};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<long>(w) * h,
                   const_cast<float*>(pixels.data()), &status);
    if (!bayer.empty())      fits_update_key_str(fp, "BAYERPAT", bayer.c_str(),      nullptr, &status);
    if (!instrument.empty()) fits_update_key_str(fp, "INSTRUME", instrument.c_str(), nullptr, &status);
    if (!filter.empty())     fits_update_key_str(fp, "FILTER",   filter.c_str(),     nullptr, &status);
    fits_close_file(fp, &status);
    if (status != 0) {
        char msg[FLEN_ERRMSG];
        fits_get_errstatus(status, msg);
        throw std::runtime_error(std::string("synthetic_fits: ") + msg + " writing " + path);
    }
}

// Lay (r, g, b) onto a 2x2 Bayer mosaic. pattern[(y%2)*2 + x%2] names the photosite.
std::vector<float> bayerize(int w, int h, const std::string& pattern, float r, float g, float b) {
    if (pattern.size() != 4) throw std::runtime_error("synthetic_fits: BAYERPAT must be 4 chars");
    std::vector<float> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const char site = pattern[(y % 2) * 2 + (x % 2)];
            out[static_cast<size_t>(y) * w + x] = site == 'R' ? r : site == 'B' ? b : g;
        }
    }
    return out;
}

const QEDatabase& fixture_db() {
    static const QEDatabase db = [] {
        QEDatabase d;
        auto r = d.load_shipped(std::string(NUKEX_TEST_FIXTURES_DIR) + "/qe/minimal_db.json");
        if (!r.ok) throw std::runtime_error("synthetic_fits: " + r.error);
        return d;
    }();
    return db;
}

void write_q_solved(const std::string& path, int w, int h, const std::string& camera,
                    const std::string& filter, double line1, double line2, const std::string& instrume) {
    ChannelDecomposer dec(fixture_db());
    Eigen::MatrixXd Q = dec.build_q(camera, filter);          // 3 x 2
    Eigen::Vector3d rgb = Q * Eigen::Vector2d(line1, line2);
    auto pixels = bayerize(w, h, "RGGB",
                           static_cast<float>(rgb(0)), static_cast<float>(rgb(1)), static_cast<float>(rgb(2)));
    write_fits(path, w, h, pixels, "RGGB", instrume.empty() ? camera : instrume, filter);
}

} // namespace

void write_synthetic_bayer(const std::string& path, int w, int h, const std::string& bayer,
                           const std::string& instrument, const std::string& filter, float uniform_value) {
    write_fits(path, w, h, bayerize(w, h, bayer, uniform_value, uniform_value, uniform_value),
               bayer, instrument, filter);
}

void write_synthetic_mono(const std::string& path, int w, int h,
                          const std::string& instrument, const std::string& filter, float uniform_value) {
    write_fits(path, w, h, std::vector<float>(static_cast<size_t>(w) * h, uniform_value),
               "", instrument, filter);
}

void write_synthetic_q_solved_hao3(const std::string& path, int w, int h, const std::string& camera,
                                   float ha, float oiii, const std::string& instrume) {
    write_q_solved(path, w, h, camera, "HaO3", ha, oiii, instrume);
}

void write_synthetic_q_solved_s2o3(const std::string& path, int w, int h, const std::string& camera,
                                   float sii, float oiii, const std::string& instrume) {
    write_q_solved(path, w, h, camera, "S2O3", sii, oiii, instrume);
}

}} // namespace nukex::test_util
```

`test/CMakeLists.txt` `test_util` block:

```cmake
add_library(test_util STATIC
    util/png_writer.cpp
    util/test_data_loader.cpp
    util/synthetic_fits.cpp
)
target_include_directories(test_util PUBLIC
    "${CMAKE_SOURCE_DIR}/test/util"
    "${CMAKE_SOURCE_DIR}/third_party/stb"
    "${CMAKE_SOURCE_DIR}/third_party/catch2"
)
target_link_libraries(test_util PUBLIC nukex4_io nukex4_stretch nukex4_calibration PRIVATE cfitsio)
```

- [ ] **Step 4: Run the writer test**

Run: `cd build && cmake .. > /dev/null && make test_synthetic_fits 2>&1 | grep -E "error" ; ./test/test_synthetic_fits 2>&1 | tail -2`
Expected: `All tests passed (…)`.

- [ ] **Step 5: Un-gate the integration tests**

In both `test/integration/test_phase_a_router.cpp` and `test_phase_b_qsolve.cpp`:
1. Delete `#define WIRED_BY_TASK_20 0` and the header comment block that describes the gate (phase_a lines 3–22; phase_b lines 3–22 equivalent).
2. Replace `#if WIRED_BY_TASK_20` / `#include "test_data_loader.hpp"` / `#endif` with `#include "synthetic_fits.hpp"`.
3. In every `TEST_CASE`, delete the `#if WIRED_BY_TASK_20` line, the `#else` … `SKIP(...)` … `#endif` lines, leaving the real body.

Append to `test_phase_b_qsolve.cpp` (exercises Task 15b end to end):

```cpp
TEST_CASE("Phase B Q-solve: unknown INSTRUME falls back to generic_sony_imx_osc with a warning",
          "[.integration][phase_b]") {
    // Photosites are engineered from ASI585MC's Q; the fixture's generic
    // camera is a copy of ASI585MC, so the fallback recovers the same lines.
    auto tmp = fs::temp_directory_path() / "phase_b_generic.fits";
    test_util::write_synthetic_q_solved_hao3(tmp.string(), 16, 16, "ASI585MC", 0.5f, 0.3f,
                                              /*instrume*/"Unknown Cam X");
    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.qe_generic_camera_fallback);
    REQUIRE(result.derived.slots.at("Ha")[8 * 16 + 8] == Catch::Approx(0.5f).margin(0.02f));
}
```

- [ ] **Step 6: Run the integration binaries with the opt-in tag**

Run:
```bash
cd build && make test_phase_a_router test_phase_b_qsolve 2>&1 | grep -E "error"
./test/integration/test_phase_a_router "[integration]" 2>&1 | tail -3
./test/integration/test_phase_b_qsolve  "[integration]" 2>&1 | tail -3
```
Expected: phase_a `All tests passed (… in 5 test cases)`; phase_b `All tests passed (… in 5 test cases)`.

If a Q-solve case misses its ±0.02 target while the writer's own unit test passes, the discrepancy is inside the engine's Phase A/B numeric path (per-frame normalisation, welford selection), not the writer: read the engine's Phase A value routing for `DUAL_NB_OSC` before touching any tolerance, and report the root cause in the commit.

- [ ] **Step 7: Full ctest (default set) + commit**

Run: `cd build && ctest 2>&1 | tail -2`
Expected: all pass (the `[.integration]` cases stay opt-in).

```bash
git add test/util/synthetic_fits.hpp test/util/synthetic_fits.cpp test/unit/io/test_synthetic_fits.cpp \
        test/CMakeLists.txt test/integration
git commit -m "$(cat <<'EOF'
test(util): synthetic FITS writer; un-gate the Phase A/B integration tests

nukex::test_util::write_synthetic_{bayer,mono,q_solved_hao3,q_solved_s2o3}
write float32 single frames with BAYERPAT/INSTRUME/FILTER headers; the
q-solved flavours derive photosite values from the fixture QE DB's Q
matrix so the engine's Q-solve recovers the engineered lines. The eight
[.integration] cases written in Tasks 9-10 now run for real, plus a case
for the generic-camera fallback (Task 15b).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

## Wave 3 (needs the PixInsight desktop on this machine)

