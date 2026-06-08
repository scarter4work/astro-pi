#include "nukex/learning/ridge_regression.hpp"

namespace nukex::learning {

bool fit_ridge(const Eigen::MatrixXd& X,
               const Eigen::VectorXd& y,
               double                 lambda,
               Eigen::VectorXd&       coeffs) {
    const Eigen::Index n_rows     = X.rows();
    const Eigen::Index n_features = X.cols();

    if (n_rows == 0 || n_features == 0) {
        return false;
    }
    if (y.size() != n_rows) {
        return false;
    }
    if (lambda < 0.0) {
        return false;
    }
    if (!X.allFinite() || !y.allFinite()) {
        return false;
    }

    // Normal equations: (X^T X + lambda I) b = X^T y
    Eigen::MatrixXd A = X.transpose() * X;
    A.diagonal().array() += lambda;
    const Eigen::VectorXd rhs = X.transpose() * y;

    Eigen::LDLT<Eigen::MatrixXd> ldlt(A);
    if (ldlt.info() != Eigen::Success) {
        return false;
    }

    Eigen::VectorXd sol = ldlt.solve(rhs);
    if (!sol.allFinite()) {
        return false;
    }

    coeffs = std::move(sol);
    return true;
}

} // namespace nukex::learning
