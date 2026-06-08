#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class LogStretch : public StretchOp {
public:
    float alpha = 1000.0f;
    bool  luminance_only = false;
    LogStretch() { name = "Log"; category = StretchCategory::PRIMARY; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;
};
} // namespace nukex
