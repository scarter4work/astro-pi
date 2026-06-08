#include "nukex/stretch/param_model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace nukex {

ParamModel::ParamModel(std::string stretch_name)
    : stretch_name_(std::move(stretch_name)) {}

void ParamModel::add_param(const std::string& param_name, ParamCoefficients coeffs) {
    per_param_.emplace(param_name, std::move(coeffs));
}

namespace {

bool row_finite(const std::array<double, 29>& row) {
    for (double v : row) if (!std::isfinite(v)) return false;
    return true;
}

double predict_scalar(const ParamCoefficients& c, const std::array<double, 29>& row) {
    double sum = c.intercept;
    const auto n = std::min<std::size_t>(row.size(), c.coefficients.size());
    for (std::size_t i = 0; i < n; ++i) {
        const double std_ = (c.feature_std[i] > 1e-12) ? c.feature_std[i] : 1.0;
        const double z    = (row[i] - c.feature_mean[i]) / std_;
        sum += c.coefficients[i] * z;
    }
    return sum;
}

} // namespace

bool ParamModel::predict_and_apply(const ImageStats& stats, StretchOp& op) const {
    const auto row = stats.to_feature_row();
    if (!row_finite(row)) return false;

    const auto bounds = op.param_bounds();
    if (bounds.empty()) return false;

    bool applied_any = false;
    for (const auto& [pname, coeffs] : per_param_) {
        auto it = bounds.find(pname);
        if (it == bounds.end()) continue;

        double v = predict_scalar(coeffs, row);
        if (!std::isfinite(v)) continue;

        const float lo = it->second.first;
        const float hi = it->second.second;
        if (v < lo) v = lo;
        if (v > hi) v = hi;

        if (op.set_param(pname, static_cast<float>(v))) {
            applied_any = true;
        }
    }
    return applied_any;
}

using nlohmann::json;

static json coeffs_to_json(const ParamCoefficients& c) {
    return {
        {"feature_mean", c.feature_mean},
        {"feature_std",  c.feature_std},
        {"coefficients", c.coefficients},
        {"intercept",    c.intercept},
        {"lambda",       c.lambda},
        {"n_train_rows", c.n_train_rows},
        {"cv_r_squared", c.cv_r_squared},
    };
}

static ParamCoefficients coeffs_from_json(const json& j) {
    ParamCoefficients c;
    c.feature_mean = j.at("feature_mean").get<std::vector<double>>();
    c.feature_std  = j.at("feature_std") .get<std::vector<double>>();
    c.coefficients = j.at("coefficients").get<std::vector<double>>();
    c.intercept    = j.at("intercept")   .get<double>();
    c.lambda       = j.at("lambda")      .get<double>();
    c.n_train_rows = j.at("n_train_rows").get<int>();
    c.cv_r_squared = j.at("cv_r_squared").get<double>();
    return c;
}

bool write_param_models_json(const ParamModelMap& models, const std::string& path) {
    json root;
    root["schema_version"] = 1;
    json& stretches = root["stretches"];
    for (const auto& [stretch_name, model] : models) {
        json per;
        for (const auto& [pname, c] : model.per_param()) {
            per[pname] = coeffs_to_json(c);
        }
        stretches[stretch_name] = per;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << root.dump(2);
    return f.good();
}

bool read_param_models_json(const std::string& path, ParamModelMap& out) {
    out.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    try {
        json root = json::parse(ss.str());
        const int schema = root.value("schema_version", 0);
        if (schema != 1) return false;
        const json& stretches = root.at("stretches");
        for (auto it = stretches.begin(); it != stretches.end(); ++it) {
            ParamModel m(it.key());
            const json& per = it.value();
            for (auto pit = per.begin(); pit != per.end(); ++pit) {
                m.add_param(pit.key(), coeffs_from_json(pit.value()));
            }
            out.emplace(it.key(), std::move(m));
        }
    } catch (const json::exception&) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace nukex
