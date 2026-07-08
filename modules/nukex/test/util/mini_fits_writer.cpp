#include "mini_fits_writer.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"

#include <fitsio.h>
#include <Eigen/Dense>

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace nukex { namespace test_util {

namespace {

void write_fits_with_metadata(const std::string& path,
                              int w, int h,
                              const std::vector<float>& pixels,
                              const std::string& bayer,
                              const std::string& instrument,
                              const std::string& filter) {
    fitsfile* fp = nullptr;
    int status = 0;
    std::remove(path.c_str()); // overwrite if present
    // CFITSIO requires a leading '!' to clobber an existing file even
    // after removal races (defensive; the remove() above already handles
    // the common case).
    std::string create_path = "!" + path;
    fits_create_file(&fp, create_path.c_str(), &status);
    long naxes[2] = {w, h};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<long>(w) * h,
                   const_cast<float*>(pixels.data()), &status);
    if (!bayer.empty())
        fits_update_key_str(fp, "BAYERPAT", bayer.c_str(), nullptr, &status);
    if (!instrument.empty())
        fits_update_key_str(fp, "INSTRUME", instrument.c_str(), nullptr, &status);
    if (!filter.empty())
        fits_update_key_str(fp, "FILTER", filter.c_str(), nullptr, &status);
    fits_close_file(fp, &status);
    if (status != 0) {
        char msg[FLEN_ERRMSG];
        fits_get_errstatus(status, msg);
        throw std::runtime_error(std::string("MiniFITSWriter: ") + msg);
    }
}

// Lay out (r, g, b) into a single-channel Bayer mosaic for the given
// 4-character pattern string (e.g. "RGGB"), top-left 2x2 super-pixel
// convention matching StackingEngine::parse_bayer_pattern /
// DebayerEngine's site layout.
std::vector<float> bayerize(int w, int h, const std::string& pattern,
                            float r, float g, float b) {
    std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);
    if (pattern.size() != 4) return out;

    auto site_value = [&](char c) -> float {
        switch (c) {
            case 'R': return r;
            case 'G': return g;
            case 'B': return b;
            default:  return 0.0f;
        }
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int phase_x = x % 2;
            int phase_y = y % 2;
            char c = pattern[phase_y * 2 + phase_x]; // row-major 2x2 tile
            out[static_cast<size_t>(y) * w + x] = site_value(c);
        }
    }
    return out;
}

QEDatabase load_test_db() {
    QEDatabase db;
    auto r = db.load_shipped(std::string(NUKEX_TEST_FIXTURES_DIR) + "/qe/minimal_db.json");
    if (!r.ok) throw std::runtime_error(std::string("MiniFITSWriter: ") + r.error);
    return db;
}

// Builds a Q-solved RGGB Bayer frame for `filter` (must be a DUAL_NB filter
// present in the fixture QE DB) whose recovered (line1, line2) via
// ChannelDecomposer::solve equal (line1_val, line2_val).
void write_qsolved(const std::string& path,
                   int w, int h,
                   const std::string& camera,
                   const std::string& filter,
                   double line1_val, double line2_val) {
    QEDatabase db = load_test_db();
    ChannelDecomposer dec(db);
    Eigen::MatrixXd Q = dec.build_q(camera, filter); // 3x2

    Eigen::Vector2d truth(line1_val, line2_val);
    Eigen::Vector3d rgb = Q * truth;

    auto pixels = bayerize(w, h, "RGGB",
                           static_cast<float>(rgb(0)),
                           static_cast<float>(rgb(1)),
                           static_cast<float>(rgb(2)));
    write_fits_with_metadata(path, w, h, pixels, "RGGB", camera, filter);
}

} // namespace

void write_synthetic_bayer(const std::string& path,
                           int w, int h,
                           const std::string& bayer,
                           const std::string& instrument,
                           const std::string& filter,
                           float uniform_value) {
    auto pixels = bayerize(w, h, bayer, uniform_value, uniform_value, uniform_value);
    write_fits_with_metadata(path, w, h, pixels, bayer, instrument, filter);
}

void write_synthetic_mono(const std::string& path,
                          int w, int h,
                          const std::string& instrument,
                          const std::string& filter,
                          float uniform_value) {
    std::vector<float> pixels(static_cast<size_t>(w) * h, uniform_value);
    write_fits_with_metadata(path, w, h, pixels, /*bayer=*/"", instrument, filter);
}

void write_synthetic_q_solved_hao3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float ha, float oiii) {
    write_qsolved(path, w, h, camera, "HaO3", ha, oiii);
}

void write_synthetic_q_solved_s2o3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float sii, float oiii) {
    write_qsolved(path, w, h, camera, "S2O3", sii, oiii);
}

void write_synthetic_negative_emission_hao3(const std::string& path,
                                            int w, int h,
                                            const std::string& camera) {
    // Ha=-0.1 forces the linear solve to recover a negative Ha value at
    // every pixel (the rgb triple is generated exactly as Q * (-0.1, 0.5),
    // so the least-squares solve inverts it back to (-0.1, 0.5) to
    // numerical precision -- see ChannelDecomposer's own round-trip test
    // for this Q-invertibility property).
    write_qsolved(path, w, h, camera, "HaO3", -0.1, 0.5);
}

}} // namespace nukex::test_util
