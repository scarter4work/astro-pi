#pragma once

#include "nukex/stretch/param_model.hpp"
#include <memory>
#include <string>

namespace nukex {

enum class ActiveLayer {
    None = 0,              // no model available -> factory defaults (Layer 1)
    CommunityBootstrap,    // Layer 2
    UserLearned,           // Layer 3
};

struct ActiveModel {
    ActiveLayer       layer       = ActiveLayer::None;
    const ParamModel* model       = nullptr;   // nullptr iff layer == None
    std::string       description;             // e.g. "Layer 3 (user-learned)"
};

/// Loads both Layer 2 and Layer 3 from disk (once) and answers "what's the
/// active model for this stretch right now?" on every Auto run.
///
/// At v4.0.1.0 ship:
///   * bootstrap_path -> file typically absent (Phase 8.5 deferred) -> Layer 2 empty
///   * user_path -> file absent until first user rating -> Layer 3 empty
///   * Active layer -> None -> factory defaults
///
/// Reload() is called after each rating Save to pick up the refreshed Layer 3.
class LayerLoader {
public:
    LayerLoader(std::string bootstrap_path, std::string user_path);

    void reload();

    ActiveModel active_for_stretch(const std::string& stretch_name) const;

private:
    std::string bootstrap_path_;
    std::string user_path_;
    ParamModelMap bootstrap_models_;
    ParamModelMap user_models_;
};

} // namespace nukex
