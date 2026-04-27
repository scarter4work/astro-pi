#pragma once

#include "nukex/io/image.hpp"
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace nukex {

enum class StretchCategory { PRIMARY, SECONDARY, FINISHER };

/// Base class for all stretch operations.
///
/// Each op implements apply(Image&) for in-place image stretching and
/// optionally apply_scalar(float) for single-value computation (LUT, testing).
/// All ops work on float32 images normalized to [0, 1].
///
/// Phase 8 additions:
///   * param_bounds() -- named, clampable parameter ranges for prediction.
///   * set_param(name, value) -- apply a predicted value to the named field.
///     Defaults to a no-op; each op overrides for its own named fields.
class StretchOp {
public:
    bool            enabled  = false;
    int             position = 0;
    std::string     name;
    StretchCategory category = StretchCategory::PRIMARY;

    virtual ~StretchOp() = default;

    /// Apply stretch to image in-place.
    virtual void apply(Image& img) const = 0;

    /// Apply to a single float value. Default: identity.
    virtual float apply_scalar(float x) const { return x; }

    /// Returns the set of clampable parameters for this op.
    /// Key: human-readable param name. Value: (min, max) inclusive bounds.
    /// An empty map means this op has no tunable parameters.
    virtual std::map<std::string, std::pair<float, float>> param_bounds() const {
        return {};
    }

    /// Set a named parameter by value (already clamped by caller).
    /// Returns true if the parameter was recognised and applied.
    virtual bool set_param(const std::string& /*param_name*/, float /*value*/) {
        return false;
    }

    /// Read a named parameter. Returns std::nullopt if unknown.
    virtual std::optional<float> get_param(const std::string& /*param_name*/) const {
        return std::nullopt;
    }
};

} // namespace nukex
