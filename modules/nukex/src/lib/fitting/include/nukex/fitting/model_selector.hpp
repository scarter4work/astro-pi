#pragma once
#include "nukex/core/distribution.hpp"
#include "nukex/core/voxel.hpp"
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class ModelSelector {
public:
    struct Config {
        double aicc_threshold      = 2.0;
        int    min_samples_for_gmm = 30;
        int    min_samples_for_fit = 10;
    };
    ModelSelector();
    explicit ModelSelector(const Config& config);
    void select(const float* values, const float* weights, int n,
                SubcubeVoxel& voxel, int channel);
    FitResult select_best(const float* values, const float* weights, int n);
private:
    Config config_;
};
} // namespace nukex
