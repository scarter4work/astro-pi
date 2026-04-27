#pragma once
#include "nukex/io/image.hpp"
#include "nukex/core/cube.hpp"

namespace nukex {
class OutputAssembler {
public:
    struct OutputImages {
        Image stacked;
        Image noise_map;
        Image quality_map;
    };
    static Image assemble_quality_map(const Cube& cube);
};
} // namespace nukex
