#pragma once

#include "nukex/io/image.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/gpu/gpu_config.hpp"
#include "nukex/core/progress_observer.hpp"
#include <string>
#include <vector>

namespace nukex {

class StackingEngine {
public:
    struct Config {
        FrameAligner::Config  aligner_config;
        WeightConfig          weight_config;
        ModelSelector::Config fitting_config;
        std::string           cache_dir = "/tmp";
        GPUExecutorConfig     gpu_config;
    };

    explicit StackingEngine(const Config& config);

    struct Result {
        Image stacked;
        Image noise_map;
        Image quality_map;
        int   n_frames_processed = 0;
        int   n_frames_failed_alignment = 0;
    };

    Result execute(const std::vector<std::string>& light_paths,
                   const std::vector<std::string>& flat_paths,
                   ProgressObserver* progress = nullptr);

private:
    Config config_;
};

} // namespace nukex
