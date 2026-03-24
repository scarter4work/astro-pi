#pragma once

#include "StarDetector.h"
#include <vector>
#include <cstddef>

namespace nukex {

struct TriangleDescriptor {
    double ratioBA;     // b/a where a >= b >= c (side lengths)
    double ratioCA;     // c/a
    int idx0, idx1, idx2;       // star indices that form this triangle
    int vertOpposite[3];        // vertex opposite [shortest, middle, longest] side
};

struct AlignmentResult {
    // Similarity transform: ref = [[a,-b],[b,a]] * target + [tx,ty]
    double a  = 1.0;            // s*cos(theta)
    double b  = 0.0;            // s*sin(theta)
    double tx = 0.0, ty = 0.0;  // translation
    bool flipped = false;        // horizontal flip applied before rotation

    // Derived for logging
    double rotation = 0.0;       // degrees
    double scale    = 1.0;
    int dx = 0, dy = 0;         // rounded translation (backward compat)

    int numMatchedStars = 0;     // confirmed star-to-star matches
    double convergenceRMS = 0.0; // RMS of position residuals
    bool valid = false;          // true if alignment succeeded
};

// Create a descriptor for a triangle formed by 3 stars
TriangleDescriptor makeTriangleDescriptor(const StarPosition& s1,
                                           const StarPosition& s2,
                                           const StarPosition& s3,
                                           int idx1, int idx2, int idx3);

// Build descriptors from top-N brightest stars (assumes stars sorted by flux descending)
std::vector<TriangleDescriptor> buildDescriptors(const std::vector<StarPosition>& stars,
                                                  int maxStars = 50);

// Match a target frame's stars against reference stars, return alignment result.
// frameWidth is needed for flip detection (to mirror x-coordinates).
AlignmentResult matchFrames(const std::vector<StarPosition>& refStars,
                             const std::vector<StarPosition>& targetStars,
                             int frameWidth = 0,
                             int maxStars = 50,
                             double matchTolerance = 0.01,
                             int minMatches = 5);

} // namespace nukex
