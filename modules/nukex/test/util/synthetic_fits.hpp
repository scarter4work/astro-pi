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
