#pragma once

#include <map>
#include <string>
#include <vector>

struct sqlite3;

namespace nukex::learning {

struct ParamCoefficients {
    std::vector<double> feature_mean;    // for z-score normalization
    std::vector<double> feature_std;     // clipped >= 1e-12
    std::vector<double> coefficients;    // one per feature
    double              intercept   = 0.0;
    double              lambda      = 1.0;
    int                 n_train_rows = 0;
    double              cv_r_squared = 0.0;
};

struct StretchCoefficients {
    std::string stretch_name;
    // Keyed by param name (e.g. "log_D"). One ParamCoefficients per trainable param.
    std::map<std::string, ParamCoefficients> per_param;
};

// Train a ridge model per param for a single stretch from all rows in the
// DB (user + attached bootstrap). Returns empty `per_param` if there are
// fewer than min_rows rated rows for this stretch.
StretchCoefficients train_one_stretch(sqlite3*            db,
                                      const std::string&  stretch_name,
                                      double              lambda,
                                      int                 min_rows = 8);

// Convenience: train all 7 stretches. Stretches with insufficient data
// return empty per_param entries and are filtered from the output.
std::map<std::string, StretchCoefficients>
train_all_stretches(sqlite3* db, double lambda);

} // namespace nukex::learning
