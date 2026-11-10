// Golden-file helpers for the gdg_core parity tests (test tooling, PCL-free).
#pragma once

#include "Npy.h"
#include "core/Image.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::test {

inline std::string golden_path(const std::string& name) {
    return std::string(GDG_GOLDEN_DIR) + "/synthetic/" + name + ".npy";
}

inline NpyArray golden(const std::string& name) { return load_npy(golden_path(name)); }

/// A (H, W) or (H, W, C) golden as a gdg::Image (same C-order layout, no copy reordering).
inline gdg::Image golden_image(const std::string& name) {
    NpyArray a = golden(name);
    if (a.shape.size() != 2 && a.shape.size() != 3)
        throw std::runtime_error(name + ": not a 2-D or 3-D array");
    gdg::Image im;
    im.height = static_cast<int>(a.shape[0]);
    im.width = static_cast<int>(a.shape[1]);
    im.channels = a.shape.size() == 3 ? static_cast<int>(a.shape[2]) : 1;
    im.data = std::move(a.data);
    return im;
}

/// Largest |a - b|; NaN where exactly one side is NaN (so a NaN mismatch can never pass a
/// tolerance), 0 where both are NaN. Throws on a size mismatch.
inline double max_abs_diff(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) throw std::runtime_error("max_abs_diff: size mismatch");
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const bool na = std::isnan(a[i]), nb = std::isnan(b[i]);
        if (na && nb) continue;
        if (na != nb) return std::nan("");
        const double d = std::fabs(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

/// Count of elements that are not bit-identical in value (NaN == NaN counts as equal).
inline size_t count_unequal(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) throw std::runtime_error("count_unequal: size mismatch");
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::isnan(a[i]) && std::isnan(b[i])) continue;
        if (a[i] != b[i]) ++n;
    }
    return n;
}

}  // namespace gdg::test
