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
