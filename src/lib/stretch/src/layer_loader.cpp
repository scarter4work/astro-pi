#include "nukex/stretch/layer_loader.hpp"
#include <filesystem>

namespace nukex {

LayerLoader::LayerLoader(std::string bootstrap_path, std::string user_path)
    : bootstrap_path_(std::move(bootstrap_path))
    , user_path_     (std::move(user_path)) {
    reload();
}

void LayerLoader::reload() {
    bootstrap_models_.clear();
    user_models_.clear();
    namespace fs = std::filesystem;
    if (!bootstrap_path_.empty() && fs::exists(bootstrap_path_)) {
        read_param_models_json(bootstrap_path_, bootstrap_models_);
    }
    if (!user_path_.empty() && fs::exists(user_path_)) {
        read_param_models_json(user_path_, user_models_);
    }
}

ActiveModel LayerLoader::active_for_stretch(const std::string& stretch_name) const {
    ActiveModel a;
    auto it = user_models_.find(stretch_name);
    if (it != user_models_.end() && !it->second.empty()) {
        a.layer = ActiveLayer::UserLearned;
        a.model = &it->second;
        a.description = "Layer 3 (user-learned)";
        return a;
    }
    auto bit = bootstrap_models_.find(stretch_name);
    if (bit != bootstrap_models_.end() && !bit->second.empty()) {
        a.layer = ActiveLayer::CommunityBootstrap;
        a.model = &bit->second;
        a.description = "Layer 2 (community bootstrap)";
        return a;
    }
    a.layer = ActiveLayer::None;
    a.description = "Layer 1 (factory defaults)";
    return a;
}

} // namespace nukex
