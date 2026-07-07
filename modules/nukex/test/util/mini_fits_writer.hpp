#pragma once

#include <string>

namespace nukex { namespace test_util {

/// Write a single-channel synthetic FITS frame carrying raw Bayer-mosaic
/// data: a uniform value is placed at every photosite (R/G/B sites laid
/// out per `bayer`), so debayering it reconstructs a flat R=G=B=value
/// image. Sets BAYERPAT, INSTRUME, and FILTER header keywords so the
/// real FilterClassifier / StackingEngine routing is exercised end to
/// end -- this is NOT a pre-debayered image.
///
/// `filter` may be empty to simulate a missing FILTER keyword (routes to
/// BROADBAND_OSC on a Bayer frame).
void write_synthetic_bayer(const std::string& path,
                           int w, int h,
                           const std::string& bayer,
                           const std::string& instrument,
                           const std::string& filter,
                           float uniform_value);

/// Write a single-channel synthetic FITS frame with no BAYERPAT keyword
/// (mono camera / filter-wheel data). Every pixel is `uniform_value`.
void write_synthetic_mono(const std::string& path,
                          int w, int h,
                          const std::string& instrument,
                          const std::string& filter,
                          float uniform_value);

/// Write a synthetic RGGB Bayer FITS frame (FILTER=HaO3) whose per-site
/// R/G/B values are computed as Q * (ha, oiii) using the *real* Q matrix
/// for `camera`/"HaO3" from the shared QE fixture DB
/// (test/fixtures/qe/minimal_db.json). Because the pixel values are
/// exactly Q * truth, a least-squares Q-solve of the recovered R/G/B
/// recovers (ha, oiii) to numerical precision (see
/// ChannelDecomposer's own round-trip test).
void write_synthetic_q_solved_hao3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float ha, float oiii);

/// Same as write_synthetic_q_solved_hao3 but for FILTER=S2O3 (SII + OIII).
void write_synthetic_q_solved_s2o3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float sii, float oiii);

/// Write a synthetic RGGB Bayer FITS frame (FILTER=HaO3) engineered so the
/// Q-solve recovers a NEGATIVE Ha value (Ha=-0.1, OIII=0.5) -- exercises
/// the negative-emission clamp-to-0 path in StackingEngine's Phase B
/// Q-solve follow-up (negative_clamped_count).
void write_synthetic_negative_emission_hao3(const std::string& path,
                                            int w, int h,
                                            const std::string& camera);

}} // namespace nukex::test_util
