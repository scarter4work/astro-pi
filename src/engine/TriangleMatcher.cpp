#include "TriangleMatcher.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace nukex {

static double dist(const StarPosition& a, const StarPosition& b) {
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return std::sqrt(dx*dx + dy*dy);
}

TriangleDescriptor makeTriangleDescriptor(const StarPosition& s1,
                                           const StarPosition& s2,
                                           const StarPosition& s3,
                                           int idx1, int idx2, int idx3) {
    double sides[3] = {dist(s1, s2), dist(s2, s3), dist(s1, s3)};
    std::sort(sides, sides + 3);
    double a = sides[2]; // longest
    double b = sides[1]; // middle
    double c = sides[0]; // shortest

    TriangleDescriptor desc;
    desc.ratioBA = (a > 0) ? b / a : 0;
    desc.ratioCA = (a > 0) ? c / a : 0;
    desc.idx0 = idx1;
    desc.idx1 = idx2;
    desc.idx2 = idx3;
    return desc;
}

std::vector<TriangleDescriptor> buildDescriptors(const std::vector<StarPosition>& stars,
                                                  int maxStars) {
    int n = std::min(static_cast<int>(stars.size()), maxStars);
    std::vector<TriangleDescriptor> descs;
    for (int i = 0; i < n - 2; ++i)
        for (int j = i + 1; j < n - 1; ++j)
            for (int k = j + 1; k < n; ++k)
                descs.push_back(makeTriangleDescriptor(stars[i], stars[j], stars[k], i, j, k));
    return descs;
}

// matchFrames placeholder — will be implemented in Task 5
AlignmentResult matchFrames(const std::vector<StarPosition>& refStars,
                             const std::vector<StarPosition>& targetStars,
                             int maxStars,
                             double matchTolerance,
                             int minMatches) {
    return AlignmentResult{0, 0, 0, 0.0, false};
}

} // namespace nukex
