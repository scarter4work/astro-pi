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
    // Using a map of pair<int,int> -> int for sparse storage
    std::map<std::pair<int,int>, int> votes;

    for (const auto& td : tgtDescs) {
        for (const auto& rd : refDescs) {
            if (std::abs(td.ratioBA - rd.ratioBA) < matchTolerance &&
                std::abs(td.ratioCA - rd.ratioCA) < matchTolerance) {
                // Matching triangle pair — vote for all 3x3 correspondences
                int tgtIdxs[3] = {td.idx0, td.idx1, td.idx2};
                int refIdxs[3] = {rd.idx0, rd.idx1, rd.idx2};
                for (int ri = 0; ri < 3; ++ri)
                    for (int ti = 0; ti < 3; ++ti)
                        votes[{refIdxs[ri], tgtIdxs[ti]}]++;
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

    // Step 7: Round to integer
    int intDx = static_cast<int>(std::round(medDx));
    int intDy = static_cast<int>(std::round(medDy));

    // Step 8: Compute RMS of residuals
    double sumSq = 0.0;
    for (const auto& [ri, ti] : confirmedPairs) {
        double resDx = (targetStars[ti].x - refStars[ri].x) - intDx;
        double resDy = (targetStars[ti].y - refStars[ri].y) - intDy;
        sumSq += resDx * resDx + resDy * resDy;
    }
    double rms = std::sqrt(sumSq / confirmedPairs.size());

    // Step 9: Valid if enough matches
    int numMatched = static_cast<int>(confirmedPairs.size());
    bool valid = numMatched >= minMatches;

    return AlignmentResult{intDx, intDy, numMatched, rms, valid};
}

} // namespace nukex
