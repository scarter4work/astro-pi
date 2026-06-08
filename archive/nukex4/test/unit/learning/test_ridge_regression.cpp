#include "catch_amalgamated.hpp"
#include "nukex/learning/ridge_regression.hpp"

#include <limits>

using namespace nukex::learning;
using Eigen::MatrixXd;
using Eigen::VectorXd;

TEST_CASE("fit_ridge: one-feature linear regression", "[learning][ridge]") {
    // y = 2x, five points, lambda = 0 --> OLS recovers slope exactly.
    MatrixXd X(5, 1);
    VectorXd y(5);
    X << 1, 2, 3, 4, 5;
    y << 2, 4, 6, 8, 10;

    VectorXd coeffs;
    REQUIRE(fit_ridge(X, y, 0.0, coeffs));
    REQUIRE(coeffs.size() == 1);
    REQUIRE(coeffs(0) == Catch::Approx(2.0).epsilon(1e-10));
}

TEST_CASE("fit_ridge: two-feature scipy reference", "[learning][ridge]") {
    // Reference values computed in scipy with:
    //   from sklearn.linear_model import Ridge
    //   m = Ridge(alpha=1.0, fit_intercept=False).fit(X, y)
    //   print(m.coef_)
    MatrixXd X(4, 2);
    VectorXd y(4);
    X << 1, 0,
         0, 1,
         1, 1,
         2, 1;
    y << 1, 2, 3, 4;

    VectorXd coeffs;
    REQUIRE(fit_ridge(X, y, 1.0, coeffs));
    REQUIRE(coeffs.size() == 2);
    // Reference value verified 2026-04-22 with scikit-learn:
    //   X = [[1,0],[0,1],[1,1],[2,1]], y = [1,2,3,4]
    //   Ridge(alpha=1.0, fit_intercept=False).fit(X, y).coef_
    //   -> [1.10526316, 1.42105263]
    // Same result from hand-computed normal equations
    //   (X^T X + I) b = X^T y with A = [[7,3],[3,4]], rhs = [12,9]:
    //   b = [21/19, 27/19] = [1.10526..., 1.42105...].
    REQUIRE(coeffs(0) == Catch::Approx(1.10526316).epsilon(1e-6));
    REQUIRE(coeffs(1) == Catch::Approx(1.42105263).epsilon(1e-6));
}

TEST_CASE("fit_ridge: lambda shrinks coefficients toward zero", "[learning][ridge]") {
    MatrixXd X(3, 1);
    VectorXd y(3);
    X << 1, 2, 3;
    y << 1, 2, 3;

    VectorXd ols, shrunk;
    REQUIRE(fit_ridge(X, y, 0.0,   ols));
    REQUIRE(fit_ridge(X, y, 100.0, shrunk));
    REQUIRE(ols(0)    == Catch::Approx(1.0).epsilon(1e-9));
    REQUIRE(shrunk(0) <  0.5);   // strongly shrunk
    REQUIRE(shrunk(0) >  0.0);   // but still positive
}

TEST_CASE("fit_ridge: mismatched dims returns false", "[learning][ridge]") {
    MatrixXd X(3, 2);
    VectorXd y(4);
    X.setZero();
    y.setZero();

    VectorXd coeffs;
    REQUIRE_FALSE(fit_ridge(X, y, 1.0, coeffs));
}

TEST_CASE("fit_ridge: non-finite input returns false", "[learning][ridge]") {
    MatrixXd X(3, 1);
    VectorXd y(3);
    X << 1, 2, std::numeric_limits<double>::quiet_NaN();
    y << 1, 2, 3;

    VectorXd coeffs;
    REQUIRE_FALSE(fit_ridge(X, y, 1.0, coeffs));
}

TEST_CASE("fit_ridge: zero rows returns false", "[learning][ridge]") {
    MatrixXd X(0, 2);
    VectorXd y(0);

    VectorXd coeffs;
    REQUIRE_FALSE(fit_ridge(X, y, 1.0, coeffs));
}
