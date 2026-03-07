#pragma once

#include "StarDetector.h"
#include <vector>
#include <cstddef>

namespace nukex {

struct TriangleDescriptor {
    double ratioBA;     // b/a where a >= b >= c (side lengths)
    double ratioCA;     // c/a
    int idx0, idx1, idx2; // star indices that form this triangle
};

struct AlignmentResult {
    int dx;                 // integer translation offset x
    int dy;                 // integer translation offset y
    int numMatchedStars;    // number of confirmed star-to-star matches
    double convergenceRMS;  // RMS of position residuals after alignment
    bool valid;             // true if enough matches were found
};

// Create a descriptor for a triangle formed by 3 stars
TriangleDescriptor makeTriangleDescriptor(const StarPosition& s1,
                                           const StarPosition& s2,
                                           const StarPosition& s3,
                                           int idx1, int idx2, int idx3);

// Build descriptors from top-N brightest stars (assumes stars sorted by flux descending)
std::vector<TriangleDescriptor> buildDescriptors(const std::vector<StarPosition>& stars,
                                                  int maxStars = 50);

// Match a target frame's stars against reference stars, return alignment result
AlignmentResult matchFrames(const std::vector<StarPosition>& refStars,
                             const std::vector<StarPosition>& targetStars,
                             int maxStars = 50,
                             double matchTolerance = 0.01,
                             int minMatches = 5);

} // namespace nukex
