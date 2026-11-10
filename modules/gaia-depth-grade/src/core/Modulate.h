// Strength -> per-star render modulation, as the Python core's modulate.py (PCL-free).
#pragma once

#include "Config.h"

#include <vector>

namespace gdg {

struct Modulation {
    std::vector<double> brightness, size, contrast, saturation;
};

/// modulate.compute_modulation: brightness 1 + g.b*s, size 1 + g.size*s, contrast g.c*s,
/// saturation 1 + g.sat*s.
inline Modulation compute_modulation(const std::vector<double>& s, const Gains& g) {
    Modulation m;
    m.brightness.reserve(s.size());
    m.size.reserve(s.size());
    m.contrast.reserve(s.size());
    m.saturation.reserve(s.size());
    for (double v : s) {
        m.brightness.push_back(1.0 + g.brightness * v);
        m.size.push_back(1.0 + g.size * v);
        m.contrast.push_back(g.contrast * v);
        m.saturation.push_back(1.0 + g.saturation * v);
    }
    return m;
}

}  // namespace gdg
