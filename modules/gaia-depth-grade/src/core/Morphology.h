// scipy.ndimage grey morphology with a flat square footprint (PCL-free, reentrant).
#pragma once

#include "Image.h"

namespace gdg {

/// scipy.ndimage.grey_closing(mono, size=(size, size)), mode='reflect', origin=0: a separable
/// max filter (dilation) then a separable min filter (erosion). For an even size scipy shifts the
/// dilation window by one (origin -1), so dilation covers [i - size/2 + 1, i + size/2] and
/// erosion [i - size/2, i + size/2 - 1]; odd sizes are centred. Throws std::invalid_argument for
/// a multi-channel image or size < 1.
Image grey_closing(const Image& mono, int size);

}  // namespace gdg
