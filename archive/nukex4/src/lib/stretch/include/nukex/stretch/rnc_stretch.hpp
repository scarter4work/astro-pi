#pragma once
#include "nukex/stretch/stretch_op.hpp"
namespace nukex {
class RNCStretch : public StretchOp {
public:
    float black_point = 0.0f;
    float white_point = 1.0f;
    float gamma       = 2.2f;
    bool  luminance_only = false;
    RNCStretch() { name = "RNC"; category = StretchCategory::SECONDARY; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;
};
} // namespace nukex
