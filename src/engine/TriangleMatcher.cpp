#include "TriangleMatcher.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>
#include <utility>

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
    // Each side has a length and the index of the vertex opposite to it
    struct SideInfo {
        double length;
        int oppositeIdx;
    };

    SideInfo sideInfo[3] = {
        {dist(s1, s2), idx3},  // side s1-s2, opposite vertex is s3
        {dist(s2, s3), idx1},  // side s2-s3, opposite vertex is s1
        {dist(s1, s3), idx2}   // side s1-s3, opposite vertex is s2
    };

    // Sort by length ascending: [0]=shortest, [1]=middle, [2]=longest
    std::sort(sideInfo, sideInfo + 3,
              [](const SideInfo& a, const SideInfo& b) { return a.length < b.length; });

    double a = sideInfo[2].length; // longest
    double b = sideInfo[1].length; // middle
    double c = sideInfo[0].length; // shortest

    TriangleDescriptor desc;
    desc.ratioBA = (a > 0) ? b / a : 0;
    desc.ratioCA = (a > 0) ? c / a : 0;
    desc.idx0 = idx1;
    desc.idx1 = idx2;
    desc.idx2 = idx3;

    // vertOpposite[i] = vertex opposite the i-th sorted side
    // This establishes a canonical ordering: when two triangles match,
    // vertOpposite[k] in ref corresponds to vertOpposite[k] in target.
    desc.vertOpposite[0] = sideInfo[0].oppositeIdx;  // opposite shortest
    desc.vertOpposite[1] = sideInfo[1].oppositeIdx;  // opposite middle
    desc.vertOpposite[2] = sideInfo[2].oppositeIdx;  // opposite longest

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

AlignmentResult matchFrames(const std::vector<StarPosition>& refStars,
                             const std::vector<StarPosition>& targetStars,
                             int maxStars,
                             double matchTolerance,
                             int minMatches) {
    // Step 1: Need at least 3 stars in each list to form triangles
    if (refStars.size() < 3 || targetStars.size() < 3)
        return AlignmentResult{0, 0, 0, 0.0, false};

    // Step 2: Build triangle descriptors for both frames
    auto refDescs = buildDescriptors(refStars, maxStars);
    auto tgtDescs = buildDescriptors(targetStars, maxStars);

    int nRef = std::min(static_cast<int>(refStars.size()), maxStars);
    int nTgt = std::min(static_cast<int>(targetStars.size()), maxStars);

    // Step 3: Vote matrix — votes[refIdx][tgtIdx] = count
    std::map<std::pair<int,int>, int> votes;

    for (const auto& td : tgtDescs) {
        for (const auto& rd : refDescs) {
            if (std::abs(td.ratioBA - rd.ratioBA) < matchTolerance &&
                std::abs(td.ratioCA - rd.ratioCA) < matchTolerance) {
                // Matching triangles — use sorted side ordering to establish
                // vertex correspondence. vertOpposite[k] in ref maps to
                // vertOpposite[k] in target (both opposite the k-th sorted side).
                for (int k = 0; k < 3; ++k)
                    votes[{rd.vertOpposite[k], td.vertOpposite[k]}]++;
            }
        }
    }

    // Step 4: For each reference star, find the target star with the most votes
    // Require at least 2 votes for a confirmed match
    std::vector<std::pair<int,int>> confirmedPairs; // (refIdx, tgtIdx)
    for (int r = 0; r < nRef; ++r) {
        int bestTgt = -1;
        int bestVotes = 1; // threshold: must have at least 2 votes
        for (int t = 0; t < nTgt; ++t) {
            auto it = votes.find({r, t});
            if (it != votes.end() && it->second > bestVotes) {
                bestVotes = it->second;
                bestTgt = t;
            }
        }
        if (bestTgt >= 0)
            confirmedPairs.push_back({r, bestTgt});
    }

    // Step 5: Compute dx/dy offsets from confirmed pairs
    if (static_cast<int>(confirmedPairs.size()) < minMatches)
        return AlignmentResult{0, 0, 0, 0.0, false};

    std::vector<double> dxVals, dyVals;
    dxVals.reserve(confirmedPairs.size());
    dyVals.reserve(confirmedPairs.size());
    for (const auto& [ri, ti] : confirmedPairs) {
        dxVals.push_back(targetStars[ti].x - refStars[ri].x);
        dyVals.push_back(targetStars[ti].y - refStars[ri].y);
    }

    // Step 6: Take median of dx and dy (robust to outlier matches)
    std::sort(dxVals.begin(), dxVals.end());
    std::sort(dyVals.begin(), dyVals.end());

    auto median = [](const std::vector<double>& v) -> double {
        size_t n = v.size();
        if (n % 2 == 1)
            return v[n / 2];
        else
            return (v[n / 2 - 1] + v[n / 2]) / 2.0;
    };

    double medDx = median(dxVals);
    double medDy = median(dyVals);

    // Step 7: Geometric consistency filter — reject star pairs whose offset
    // deviates more than 5 pixels from the median consensus. This removes
    // false matches that contaminate the RMS.
    std::vector<std::pair<int,int>> filteredPairs;
    filteredPairs.reserve(confirmedPairs.size());
    for (const auto& [ri, ti] : confirmedPairs) {
        double pairDx = targetStars[ti].x - refStars[ri].x;
        double pairDy = targetStars[ti].y - refStars[ri].y;
        double residX = pairDx - medDx;
        double residY = pairDy - medDy;
        if (residX * residX + residY * residY < 25.0)  // within 5 pixels
            filteredPairs.push_back({ri, ti});
    }

    // Fall back to unfiltered if too few survived
    if (static_cast<int>(filteredPairs.size()) < minMatches)
        filteredPairs = confirmedPairs;

    // Step 8: Round to integer
    int intDx = static_cast<int>(std::round(medDx));
    int intDy = static_cast<int>(std::round(medDy));

    // Step 9: Compute RMS of residuals from geometrically consistent pairs
    double sumSq = 0.0;
    for (const auto& [ri, ti] : filteredPairs) {
        double resDx = (targetStars[ti].x - refStars[ri].x) - intDx;
        double resDy = (targetStars[ti].y - refStars[ri].y) - intDy;
        sumSq += resDx * resDx + resDy * resDy;
    }
    double rms = std::sqrt(sumSq / filteredPairs.size());

    // Step 10: Valid if enough matches
    int numMatched = static_cast<int>(filteredPairs.size());
    bool valid = numMatched >= minMatches;

    return AlignmentResult{intDx, intDy, numMatched, rms, valid};
}

} // namespace nukex
