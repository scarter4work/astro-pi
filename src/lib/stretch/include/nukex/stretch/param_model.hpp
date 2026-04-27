#pragma once

#include "nukex/stretch/image_stats.hpp"
#include "nukex/stretch/stretch_op.hpp"

#include <map>
#include <string>
#include <vector>

namespace nukex {

struct ParamCoefficients {
    std::vector<double> feature_mean;
    std::vector<double> feature_std;
    std::vector<double> coefficients;
    double              intercept    = 0.0;
    double              lambda       = 1.0;
    int                 n_train_rows = 0;
    double              cv_r_squared = 0.0;
};

/// Per-stretch trained model. Holds one ParamCoefficients per trainable param.
class ParamModel {
public:
    ParamModel() = default;
    explicit ParamModel(std::string stretch_name);

    const std::string& stretch_name() const { return stretch_name_; }
    void add_param(const std::string& param_name, ParamCoefficients coeffs);

    bool empty() const { return per_param_.empty(); }
    bool has_param(const std::string& n) const { return per_param_.count(n) > 0; }
    const std::map<std::string, ParamCoefficients>& per_param() const { return per_param_; }

    /// Predict param values from an image-stat row and mutate `op` accordingly.
    /// Predicted values are clamped against op.param_bounds(). Params present
    /// in the model but not in param_bounds are silently dropped.
    /// Returns true if at least one param was set.
    bool predict_and_apply(const ImageStats& stats, StretchOp& op) const;

private:
    std::string stretch_name_;
    std::map<std::string, ParamCoefficients> per_param_;
};

// Free functions: one file holds the full per-stretch map.
//
// Format (JSON):
//   {
//     "schema_version": 1,
//     "stretches": {
//       "VeraLux": {
//         "log_D": { "feature_mean": [...], "feature_std": [...],
//                    "coefficients": [...], "intercept": 3.5,
//                    "lambda": 1.0, "n_train_rows": 42, "cv_r_squared": 0.31 },
//         ...
//       },
//       ...
//     }
//   }
using ParamModelMap = std::map<std::string, ParamModel>;

bool write_param_models_json(const ParamModelMap& models, const std::string& path);
bool read_param_models_json (const std::string& path, ParamModelMap& out);

} // namespace nukex
