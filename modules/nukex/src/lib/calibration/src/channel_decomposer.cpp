#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/calibration/qe_database.hpp"

namespace nukex {

Eigen::MatrixXd ChannelDecomposer::build_q(const std::string& camera,
                                           const std::string& filter_name) const {
    if (!db_.has_camera(camera)) {
        throw UnknownCameraError("Camera not in QE DB: " + camera);
    }
    if (!db_.has_filter(filter_name)) {
        throw UnknownFilterError("Filter not in QE DB: " + filter_name);
    }
    auto fp = db_.lookup_filter(filter_name);
    if (fp.lines.empty()) {
        throw UnknownFilterError("Filter " + filter_name + " has no emission lines defined");
    }

    const int n_lines = static_cast<int>(fp.lines.size());
    Eigen::MatrixXd Q(3, n_lines);
    for (int j = 0; j < n_lines; ++j) {
        const double wl = fp.lines[j].wavelength_nm;
        Q(0, j) = db_.lookup_camera_qe(camera, wl, Photosite::R);
        Q(1, j) = db_.lookup_camera_qe(camera, wl, Photosite::G);
        Q(2, j) = db_.lookup_camera_qe(camera, wl, Photosite::B);
    }

    // Singularity check via rank. A rank-deficient Q means the QE values at
    // the chosen line wavelengths don't form a non-degenerate basis — usually
    // because a buggy or fudged DB entry has identical R/G/B values. Loud
    // failure beats a silently nonsensical least-squares answer.
    Eigen::FullPivLU<Eigen::MatrixXd> lu(Q);
    if (lu.rank() < n_lines) {
        throw SingularQError("Q matrix for (" + camera + ", " + filter_name +
                             ") is singular — QE values must form a non-degenerate basis. " +
                             "Filter QE in DB is suspect. Report bug + check override.");
    }

    return Q;
}

Eigen::VectorXd ChannelDecomposer::solve(const std::string&     camera,
                                         const std::string&     filter_name,
                                         const Eigen::Vector3d& rgb) const {
    Eigen::MatrixXd Q = build_q(camera, filter_name);
    return Q.colPivHouseholderQr().solve(rgb);
}

} // namespace nukex
