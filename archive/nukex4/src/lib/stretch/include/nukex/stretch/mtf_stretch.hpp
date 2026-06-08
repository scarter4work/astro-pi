#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class MTFStretch : public StretchOp {
public:
    float midtone    = 0.25f;
    float shadows    = 0.0f;
    float highlights = 1.0f;
    bool  luminance_only = false;
    MTFStretch() { name = "MTF"; category = StretchCategory::FINISHER; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;
};
} // namespace nukex
