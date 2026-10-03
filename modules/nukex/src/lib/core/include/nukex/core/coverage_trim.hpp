#pragma once

#include <algorithm>

namespace nukex {

class Cube;

/// An inclusive pixel rectangle of the stacked output.
struct TrimBounds {
    int  x0 = 0, y0 = 0;
    int  x1 = -1, y1 = -1;
    bool applied = false;   // false when the rectangle is the whole frame

    int width()  const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
};

/// The largest rectangle in which EVERY frame contributed to EVERY populated
/// slot -- intersection mode.
///
/// A dithered, drifting session does not cover a rectangle: the outer ring of
/// the output is reached by fewer and fewer frames the further out it goes.
/// Those pixels are correctly averaged, but they are averaged over less data,
/// and it shows. Measured on a 74-frame stack, against an interior noise of
/// 0.000234: the top ten rows carry 0.001093 (4.7x) and the right ten columns
/// 0.000837 (3.6x). At the contrast the shadow point now delivers, those
/// bands read as a frame around the picture.
///
/// A slot falls short for a pixel when its sample count is below that slot's
/// peak anywhere in the frame. ANY slot falling short disqualifies the pixel,
/// because per-channel registration can push one colour plane off the source
/// while the others stay on it -- which is exactly how a stack ends up with a
/// dead red column beside complete green and blue ones, and the composed
/// pixel is broken either way.
///
/// A slot no frame ever filled takes no part: an L-only stack still allocates
/// the colour slots its channel config names, and treating those as uncovered
/// everywhere would reduce the output to nothing.
///
/// Walking inward from each edge until the edge line happens to be clean is
/// NOT equivalent and can stop early, because coverage is ragged rather than
/// rectangular. This computes the genuine largest covered rectangle.
TrimBounds full_coverage_rect(const Cube& cube);

} // namespace nukex
