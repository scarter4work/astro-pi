#ifndef NUKEX_CALIBRATION_CHANNEL_DECOMPOSER_HPP
#define NUKEX_CALIBRATION_CHANNEL_DECOMPOSER_HPP

#include <Eigen/Dense>

#include <stdexcept>
#include <string>

namespace nukex {

class QEDatabase;

class UnknownCameraError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UnknownFilterError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SingularQError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ChannelDecomposer turns observed (R, G, B) photosite intensities back into
// per-emission-line intensities, given the camera's spectral QE curve and the
// filter's emission-line set.
//
// The mapping is linear: rgb = Q · lines, where Q is the 3×N matrix whose
// columns are (QE_R, QE_G, QE_B) sampled at each emission-line wavelength.
// solve() inverts it via Eigen's column-pivoting QR (numerically stable for
// the small 3×2 / 3×3 systems we deal with).
//
// Holds a non-owning reference to the QEDatabase — the database must outlive
// the decomposer.
class ChannelDecomposer {
public:
    explicit ChannelDecomposer(const QEDatabase& db) : db_(db) {}

    // Build the 3×N Q matrix for the (camera, filter) pair.
    // For dual-NB: 3×2; for broadband-OSC: 3×3 (identity-like, mostly diagonal).
    //
    // Throws:
    //   - UnknownCameraError if camera not in DB
    //   - UnknownFilterError if filter not in DB or has no emission lines
    //   - SingularQError    if Q is rank-deficient (degenerate QE values)
    Eigen::MatrixXd build_q(const std::string& camera,
                            const std::string& filter_name) const;

    // Solve (R, G, B) → (line1, line2, ...) via least-squares QR.
    // Throws the same set as build_q().
    Eigen::VectorXd solve(const std::string&     camera,
                          const std::string&     filter_name,
                          const Eigen::Vector3d& rgb) const;

private:
    const QEDatabase& db_;
};

} // namespace nukex

#endif
