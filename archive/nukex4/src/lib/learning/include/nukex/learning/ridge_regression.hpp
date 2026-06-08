#pragma once

#include <Eigen/Dense>

namespace nukex::learning {

// Closed-form ridge regression. Fits coefficients minimizing
//     || y - X b ||^2 + lambda || b ||^2
// using normal equations: b = (X^T X + lambda I)^{-1} X^T y.
//
// Inputs:
//   X       -- n_rows x n_features design matrix (no intercept column)
//   y       -- n_rows vector of targets
//   lambda  -- L2 regularization strength (>= 0)
//
// Outputs:
//   coeffs  -- n_features vector (not including intercept; caller handles mean-center)
//
// Returns false and leaves coeffs untouched on:
//   * n_rows == 0, n_features == 0, or mismatched dims
//   * X^T X + lambda I is singular (LDLT fails)
//   * non-finite values in X or y
bool fit_ridge(const Eigen::MatrixXd& X,
               const Eigen::VectorXd& y,
               double                 lambda,
               Eigen::VectorXd&       coeffs);

} // namespace nukex::learning
