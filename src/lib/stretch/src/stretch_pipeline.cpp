#include "nukex/stretch/stretch_pipeline.hpp"
#include <algorithm>
#include <cmath>

namespace nukex {

void StretchPipeline::execute(Image& img) const {
    // Collect enabled ops, sort by position
    std::vector<const StretchOp*> ordered;
    for (const auto& op : ops) {
        if (op->enabled) ordered.push_back(op.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const StretchOp* a, const StretchOp* b) {
                  return a->position < b->position;
              });

    for (const auto* op : ordered) {
        op->apply(img);
    }
}

std::vector<std::string> StretchPipeline::check_ordering() const {
    std::vector<std::string> warnings;

    // Collect enabled ops sorted by position
    std::vector<const StretchOp*> ordered;
    for (const auto& op : ops) {
        if (op->enabled) ordered.push_back(op.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const StretchOp* a, const StretchOp* b) {
                  return a->position < b->position;
              });

    if (ordered.empty()) return warnings;

    // Check: warn if any SECONDARY precedes all PRIMARYs
    int first_primary = -1, first_secondary = -1;
    for (int i = 0; i < static_cast<int>(ordered.size()); i++) {
        if (ordered[i]->category == StretchCategory::PRIMARY && first_primary < 0)
            first_primary = i;
        if (ordered[i]->category == StretchCategory::SECONDARY && first_secondary < 0)
            first_secondary = i;
    }
    if (first_secondary >= 0 && (first_primary < 0 || first_secondary < first_primary)) {
        warnings.push_back("Advisory: SECONDARY stretch '" + ordered[first_secondary]->name
                          + "' precedes all PRIMARY stretches. Consider reordering.");
    }

    // Check: warn if FINISHER precedes any PRIMARY or SECONDARY
    for (int i = 0; i < static_cast<int>(ordered.size()); i++) {
        if (ordered[i]->category == StretchCategory::FINISHER) {
            for (int j = i + 1; j < static_cast<int>(ordered.size()); j++) {
                if (ordered[j]->category != StretchCategory::FINISHER) {
                    warnings.push_back("Advisory: FINISHER '" + ordered[i]->name
                                      + "' precedes '" + ordered[j]->name
                                      + "'. Finishers work best last.");
                    break;
                }
            }
        }
    }

    return warnings;
}

Image StretchPipeline::quick_preview_stretch(const Image& linear_src, float alpha) {
    Image preview = linear_src.clone();
    float norm = std::asinh(alpha);
    if (norm < 1e-10f) return preview;

    preview.apply([alpha, norm](float x) -> float {
        return std::asinh(alpha * x) / norm;
    });
    return preview;
}

} // namespace nukex
