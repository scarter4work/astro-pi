// AutoContrast core (PCL-free): scipy.ndimage.label (4-connectivity) + find_objects.
#pragma once

#include <cstdint>
#include <vector>

namespace ac {

struct Box {
   int y0, y1, x0, x1;   // half-open: rows [y0, y1), columns [x0, x1)
};

struct Labels {
   int count = 0;                // number of components
   std::vector<int> label;       // w*h, row-major; 0 = background, 1..count
   std::vector<Box> boxes;       // boxes[k-1] is the bounding box of label k (find_objects)
};

// scipy.ndimage.label(mask) with the default 4-connected structure: labels are numbered in raster order
// of each component's first pixel. mask is w*h row-major, non-zero = foreground.
Labels label4(const std::vector<uint8_t>& mask, int w, int h);

} // namespace ac
