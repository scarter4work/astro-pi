#include "nukex/learning/train_model.hpp"
#include "nukex/learning/rating_db.hpp"
#include "nukex/learning/ridge_regression.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>
#include <cmath>

namespace nukex::learning {

namespace {

// Build a 29-col feature row from a RunRecord matching ImageStats::to_feature_row ordering.
std::array<double, 29> record_to_row(const RunRecord& r) {
    std::array<double, 29> out{};
    int k = 0;
    for (int i = 0; i < 24; ++i) out[k++] = r.per_channel_stats[i];
    out[k++] = r.bright_concentration;
    out[k++] = r.color_rg;
    out[k++] = r.color_bg;
    out[k++] = r.fwhm_median;
    out[k++] = static_cast<double>(r.star_count);
    return out;
}

struct Scaler {
    std::vector<double> mean;
    std::vector<double> std_;
};

Scaler fit_scaler(const Eigen::MatrixXd& X) {
    Scaler s;
    const Eigen::Index n = X.rows(), p = X.cols();
    s.mean.assign(p, 0.0);
    s.std_.assign(p, 1.0);
    if (n == 0) return s;
    for (Eigen::Index j = 0; j < p; ++j) {
        double sum = 0;
        for (Eigen::Index i = 0; i < n; ++i) sum += X(i, j);
        s.mean[j] = sum / static_cast<double>(n);
        double sq = 0;
        for (Eigen::Index i = 0; i < n; ++i) {
            const double d = X(i, j) - s.mean[j];
            sq += d * d;
        }
        double var = sq / static_cast<double>(n);
        s.std_[j] = (var < 1e-12) ? 1.0 : std::sqrt(var);
    }
    return s;
}

Eigen::MatrixXd z_score(const Eigen::MatrixXd& X, const Scaler& s) {
    Eigen::MatrixXd Z = X;
    for (Eigen::Index j = 0; j < X.cols(); ++j) {
        for (Eigen::Index i = 0; i < X.rows(); ++i) {
            Z(i, j) = (X(i, j) - s.mean[j]) / s.std_[j];
        }
    }
    return Z;
}

} // namespace

StretchCoefficients train_one_stretch(sqlite3* db,
                                      const std::string& stretch_name,
                                      double lambda,
                                      int min_rows) {
    StretchCoefficients out;
    out.stretch_name = stretch_name;

    auto rows = select_runs_for_stretch(db, stretch_name);
    if (static_cast<int>(rows.size()) < min_rows) return out;

    // Parse params_json from every row to discover param names and collect targets.
    using json = nlohmann::json;
    std::map<std::string, std::vector<double>> param_targets;
    std::vector<std::array<double, 29>> feature_rows;
    feature_rows.reserve(rows.size());

    for (const auto& r : rows) {
        const auto row = record_to_row(r);
        bool finite = true;
        for (double v : row) if (!std::isfinite(v)) { finite = false; break; }
        if (!finite) continue;

        json params;
        try { params = json::parse(r.params_json); }
        catch (...) { continue; }
        if (!params.is_object()) continue;

        feature_rows.push_back(row);
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (!it.value().is_number()) continue;
            param_targets[it.key()].push_back(it.value().get<double>());
        }
    }
    if (feature_rows.size() < static_cast<std::size_t>(min_rows)) return out;

    const Eigen::Index n = static_cast<Eigen::Index>(feature_rows.size());
    const Eigen::Index p = 29;
    Eigen::MatrixXd X(n, p);
    for (Eigen::Index i = 0; i < n; ++i) {
        for (Eigen::Index j = 0; j < p; ++j) {
            X(i, j) = feature_rows[i][j];
        }
    }
    const Scaler scaler = fit_scaler(X);
    const Eigen::MatrixXd Z = z_score(X, scaler);

    for (auto& [pname, targets] : param_targets) {
        if (targets.size() != static_cast<std::size_t>(n)) continue;
        Eigen::VectorXd y(n);
        double mean_y = 0.0;
        for (Eigen::Index i = 0; i < n; ++i) { y(i) = targets[i]; mean_y += targets[i]; }
        mean_y /= static_cast<double>(n);
        Eigen::VectorXd y_c = y.array() - mean_y;

        Eigen::VectorXd beta;
        if (!fit_ridge(Z, y_c, lambda, beta)) continue;

        ParamCoefficients c;
        c.feature_mean = scaler.mean;
        c.feature_std  = scaler.std_;
        c.coefficients.assign(beta.data(), beta.data() + beta.size());
        c.intercept    = mean_y;
        c.lambda       = lambda;
        c.n_train_rows = static_cast<int>(n);
        // CV R^2 computed in Phase 8.5 when it actually gates a release.
        c.cv_r_squared = 0.0;
        out.per_param.emplace(pname, std::move(c));
    }
    return out;
}

std::map<std::string, StretchCoefficients>
train_all_stretches(sqlite3* db, double lambda) {
    const std::array<const char*, 7> names = {
        "VeraLux", "GHS", "MTF", "ArcSinh", "Log", "Lupton", "CLAHE"
    };
    std::map<std::string, StretchCoefficients> out;
    for (const char* n : names) {
        auto c = train_one_stretch(db, n, lambda);
        if (!c.per_param.empty()) out.emplace(n, std::move(c));
    }
    return out;
}

} // namespace nukex::learning
