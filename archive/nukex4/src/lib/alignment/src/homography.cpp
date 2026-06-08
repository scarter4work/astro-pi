#include "nukex/alignment/types.hpp"
#include "nukex/alignment/homography.hpp"
#include <Eigen/Dense>
#include <Eigen/SVD>
#include <random>
#include <cmath>

namespace nukex {

bool HomographyMatrix::is_identity(float tolerance) const {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float expected = (r == c) ? 1.0f : 0.0f;
            if (std::abs(H[r][c] - expected) > tolerance) return false;
        }
    }
    return true;
}

float HomographyMatrix::rotation_degrees() const {
    return std::atan2(H[1][0], H[0][0]) * 180.0f / 3.14159265358979f;
}

bool HomographyMatrix::is_meridian_flip(float angle_tolerance_deg) const {
    float angle = std::abs(rotation_degrees());
    return std::abs(angle - 180.0f) < angle_tolerance_deg;
}

HomographyMatrix HomographyMatrix::identity() {
    HomographyMatrix m;
    m.H = {{{1,0,0}, {0,1,0}, {0,0,1}}};
    return m;
}

/// Compute Hartley normalization transform for a set of 2D points.
/// Returns (T, x_norm, y_norm) where T is a 3x3 normalization matrix
/// such that the normalized points have centroid at origin and RMS
/// distance of sqrt(2) from origin.
static Eigen::Matrix3f compute_normalization(
    const float* xs, const float* ys, int n,
    std::vector<float>& x_out, std::vector<float>& y_out) {

    float mean_x = 0, mean_y = 0;
    for (int i = 0; i < n; i++) { mean_x += xs[i]; mean_y += ys[i]; }
    mean_x /= n; mean_y /= n;

    float scale = 0;
    for (int i = 0; i < n; i++) {
        float dx = xs[i] - mean_x, dy = ys[i] - mean_y;
        scale += std::sqrt(dx*dx + dy*dy);
    }
    scale /= n;
    if (scale < 1e-10f) scale = 1.0f;
    scale = std::sqrt(2.0f) / scale;

    x_out.resize(n);
    y_out.resize(n);
    for (int i = 0; i < n; i++) {
        x_out[i] = (xs[i] - mean_x) * scale;
        y_out[i] = (ys[i] - mean_y) * scale;
    }

    Eigen::Matrix3f T;
    T << scale, 0, -mean_x * scale,
         0, scale, -mean_y * scale,
         0,     0,              1.0f;
    return T;
}

HomographyMatrix HomographyComputer::dlt_4point(
    const float src_x[4], const float src_y[4],
    const float dst_x[4], const float dst_y[4]) {

    // Hartley normalization for numerical stability
    std::vector<float> nx_s, ny_s, nx_d, ny_d;
    Eigen::Matrix3f Ts = compute_normalization(src_x, src_y, 4, nx_s, ny_s);
    Eigen::Matrix3f Td = compute_normalization(dst_x, dst_y, 4, nx_d, ny_d);

    // Build the 8×9 DLT matrix A where A * h = 0
    // For each correspondence (x,y) → (x',y') in normalized coords:
    //   [-x -y -1  0  0  0  x'x  x'y  x']
    //   [ 0  0  0 -x -y -1  y'x  y'y  y']
    Eigen::Matrix<float, 8, 9> A;
    A.setZero();

    for (int i = 0; i < 4; i++) {
        float x = nx_s[i], y = ny_s[i];
        float xp = nx_d[i], yp = ny_d[i];

        A(2*i, 0) = -x;  A(2*i, 1) = -y;  A(2*i, 2) = -1.0f;
        A(2*i, 6) = xp*x; A(2*i, 7) = xp*y; A(2*i, 8) = xp;

        A(2*i+1, 3) = -x;  A(2*i+1, 4) = -y;  A(2*i+1, 5) = -1.0f;
        A(2*i+1, 6) = yp*x; A(2*i+1, 7) = yp*y; A(2*i+1, 8) = yp;
    }

    // SVD of A; h is the last column of V
    Eigen::JacobiSVD<Eigen::Matrix<float, 8, 9>> svd(A, Eigen::ComputeFullV);
    Eigen::Matrix<float, 9, 1> h = svd.matrixV().col(8);

    // Reshape h into normalized 3×3 homography
    Eigen::Matrix3f Hn;
    Hn << h(0), h(1), h(2),
          h(3), h(4), h(5),
          h(6), h(7), h(8);

    // Denormalize: H_real = Td_inv * Hn * Ts
    Eigen::Matrix3f H_real = Td.inverse() * Hn * Ts;

    // Normalize so H(2,2) = 1
    if (std::abs(H_real(2,2)) > 1e-10f) {
        H_real /= H_real(2,2);
    }

    HomographyMatrix H;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            H(r,c) = H_real(r,c);

    return H;
}

HomographyMatrix HomographyComputer::dlt_npoint(
    const std::vector<float>& src_x, const std::vector<float>& src_y,
    const std::vector<float>& dst_x, const std::vector<float>& dst_y) {

    int n = static_cast<int>(src_x.size());

    // Hartley normalization for numerical stability
    std::vector<float> nx_s, ny_s, nx_d, ny_d;
    Eigen::Matrix3f Ts = compute_normalization(src_x.data(), src_y.data(), n, nx_s, ny_s);
    Eigen::Matrix3f Td = compute_normalization(dst_x.data(), dst_y.data(), n, nx_d, ny_d);

    Eigen::MatrixXf A(2 * n, 9);
    A.setZero();

    for (int i = 0; i < n; i++) {
        float x = nx_s[i], y = ny_s[i];
        float xp = nx_d[i], yp = ny_d[i];

        A(2*i, 0) = -x;  A(2*i, 1) = -y;  A(2*i, 2) = -1.0f;
        A(2*i, 6) = xp*x; A(2*i, 7) = xp*y; A(2*i, 8) = xp;

        A(2*i+1, 3) = -x;  A(2*i+1, 4) = -y;  A(2*i+1, 5) = -1.0f;
        A(2*i+1, 6) = yp*x; A(2*i+1, 7) = yp*y; A(2*i+1, 8) = yp;
    }

    Eigen::JacobiSVD<Eigen::MatrixXf> svd(A, Eigen::ComputeFullV);
    Eigen::VectorXf h = svd.matrixV().col(8);

    Eigen::Matrix3f Hn;
    Hn << h(0), h(1), h(2),
          h(3), h(4), h(5),
          h(6), h(7), h(8);

    // Denormalize: H_real = Td_inv * Hn * Ts
    Eigen::Matrix3f H_real = Td.inverse() * Hn * Ts;

    if (std::abs(H_real(2,2)) > 1e-10f) {
        H_real /= H_real(2,2);
    }

    HomographyMatrix H;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            H(r,c) = H_real(r,c);

    return H;
}

int HomographyComputer::count_inliers(
    const HomographyMatrix& H,
    const StarCatalog& source,
    const StarCatalog& reference,
    const std::vector<std::pair<int, int>>& matches,
    float threshold,
    std::vector<bool>& inlier_mask) {

    float thresh2 = threshold * threshold;
    int count = 0;
    inlier_mask.resize(matches.size());

    for (size_t i = 0; i < matches.size(); i++) {
        auto [si, ri] = matches[i];
        auto [xp, yp] = H.transform(source.stars[si].x, source.stars[si].y);

        float dx = xp - reference.stars[ri].x;
        float dy = yp - reference.stars[ri].y;
        float d2 = dx * dx + dy * dy;

        inlier_mask[i] = (d2 < thresh2);
        if (inlier_mask[i]) count++;
    }

    return count;
}

AlignmentResult HomographyComputer::compute(
    const StarCatalog& source,
    const StarCatalog& reference,
    const std::vector<std::pair<int, int>>& matches,
    const Config& config) {

    AlignmentResult result;

    if (static_cast<int>(matches.size()) < config.min_matches) {
        result.alignment_failed = true;
        result.weight_penalty = 0.5f;
        result.H = HomographyMatrix::identity();
        return result;
    }

    // RANSAC
    std::mt19937 rng(42);  // deterministic for reproducibility
    int best_inlier_count = 0;
    std::vector<bool> best_inlier_mask;

    HomographyMatrix best_H;
    int n_matches = static_cast<int>(matches.size());

    for (int iter = 0; iter < config.ransac_iterations; iter++) {
        // Sample 4 random correspondences
        int indices[4];
        for (int i = 0; i < 4; i++) {
            bool unique;
            do {
                indices[i] = rng() % n_matches;
                unique = true;
                for (int j = 0; j < i; j++) {
                    if (indices[i] == indices[j]) { unique = false; break; }
                }
            } while (!unique);
        }

        float sx[4], sy[4], dx[4], dy[4];
        for (int i = 0; i < 4; i++) {
            auto [si, ri] = matches[indices[i]];
            sx[i] = source.stars[si].x;
            sy[i] = source.stars[si].y;
            dx[i] = reference.stars[ri].x;
            dy[i] = reference.stars[ri].y;
        }

        HomographyMatrix H = dlt_4point(sx, sy, dx, dy);

        // Count inliers
        std::vector<bool> inlier_mask;
        int inlier_count = count_inliers(H, source, reference, matches,
                                          config.inlier_threshold, inlier_mask);

        if (inlier_count > best_inlier_count) {
            best_inlier_count = inlier_count;
            best_inlier_mask = inlier_mask;
            best_H = H;
        }
    }

    if (best_inlier_count < config.min_matches) {
        result.alignment_failed = true;
        result.weight_penalty = 0.5f;
        result.H = HomographyMatrix::identity();
        return result;
    }

    // Refine: DLT on all inliers
    std::vector<float> sx, sy, dx, dy;
    for (size_t i = 0; i < matches.size(); i++) {
        if (best_inlier_mask[i]) {
            auto [si, ri] = matches[i];
            sx.push_back(source.stars[si].x);
            sy.push_back(source.stars[si].y);
            dx.push_back(reference.stars[ri].x);
            dy.push_back(reference.stars[ri].y);
        }
    }

    result.H = dlt_npoint(sx, sy, dx, dy);
    result.match.n_inliers = best_inlier_count;
    result.match.success = true;

    // Compute RMS error on inliers
    float sum_err2 = 0.0f;
    for (size_t i = 0; i < sx.size(); i++) {
        auto [xp, yp] = result.H.transform(sx[i], sy[i]);
        float ex = xp - dx[i];
        float ey = yp - dy[i];
        sum_err2 += ex * ex + ey * ey;
    }
    result.match.rms_error = std::sqrt(sum_err2 / static_cast<float>(sx.size()));

    // Degeneracy check: if the RMS error after DLT refinement on all inliers
    // is much larger than the inlier threshold, the inlier set is degenerate
    // (e.g. colinear stars) and the resulting homography is unreliable.
    if (result.match.rms_error > 5.0f * config.inlier_threshold) {
        result.alignment_failed = true;
        result.weight_penalty = 0.5f;
        result.H = HomographyMatrix::identity();
        return result;
    }

    // Check for meridian flip
    result.is_meridian_flipped = result.H.is_meridian_flip();

    return result;
}

Image HomographyComputer::warp(const Image& source, const HomographyMatrix& H,
                                int output_width, int output_height) {
    Image output(output_width, output_height, source.n_channels());

    // Compute inverse H for backward mapping
    // H maps source→ref, so H_inv maps ref→source
    Eigen::Matrix3f He;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            He(r, c) = H(r, c);

    Eigen::Matrix3f H_inv = He.inverse();

    int sw = source.width();
    int sh = source.height();

    for (int ch = 0; ch < source.n_channels(); ch++) {
        for (int y = 0; y < output_height; y++) {
            for (int x = 0; x < output_width; x++) {
                // Map output (x, y) back to source coordinates
                float w = H_inv(2, 0) * x + H_inv(2, 1) * y + H_inv(2, 2);
                if (std::abs(w) < 1e-10f) continue;
                float sx = (H_inv(0, 0) * x + H_inv(0, 1) * y + H_inv(0, 2)) / w;
                float sy = (H_inv(1, 0) * x + H_inv(1, 1) * y + H_inv(1, 2)) / w;

                // Bilinear interpolation
                if (!std::isfinite(sx) || !std::isfinite(sy)) continue;
                if (sx < 0 || sx >= sw - 1 || sy < 0 || sy >= sh - 1) continue;

                int ix = static_cast<int>(sx);
                int iy = static_cast<int>(sy);
                float fx = sx - ix;
                float fy = sy - iy;

                float v00 = source.at(ix,     iy,     ch);
                float v10 = source.at(ix + 1, iy,     ch);
                float v01 = source.at(ix,     iy + 1, ch);
                float v11 = source.at(ix + 1, iy + 1, ch);

                float val = v00 * (1-fx) * (1-fy) +
                            v10 * fx * (1-fy) +
                            v01 * (1-fx) * fy +
                            v11 * fx * fy;

                output.at(x, y, ch) = val;
            }
        }
    }

    return output;
}

HomographyMatrix HomographyComputer::correct_meridian_flip(
    const HomographyMatrix& H, int width, int height) {
    // Pre-multiply H with 180-degree rotation F about image center to remove the flip.
    //
    // H maps source -> reference and contains a 180-degree flip component.
    // Decompose H as H = F * T where F is the flip and T is the residual transform.
    // We want to recover T = F_inv * H. Since F is a 180-degree rotation about
    // the image center, F is its own inverse (F * F = I), so T = F * H.
    //
    // This is why pre-multiplication (F * H) is the correct order:
    //   F * H = F * (F * T) = (F * F) * T = I * T = T
    //
    // Note: H * F would give (F * T) * F = F * T * F which is NOT T in general,
    // because matrix multiplication is not commutative.
    //
    // flip F = [-1  0  w-1]
    //          [ 0 -1  h-1]
    //          [ 0  0    1]
    Eigen::Matrix3f He, Fe;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            He(r, c) = H(r, c);

    Fe << -1, 0, static_cast<float>(width - 1),
           0, -1, static_cast<float>(height - 1),
           0,  0, 1;

    Eigen::Matrix3f result = Fe * He;

    HomographyMatrix corrected;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            corrected(r, c) = result(r, c);

    // Normalize
    if (std::abs(corrected(2, 2)) > 1e-10f) {
        float s = 1.0f / corrected(2, 2);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                corrected(r, c) *= s;
    }

    return corrected;
}

} // namespace nukex
