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
                             int frameWidth,
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

    // Step 5: Need enough confirmed pairs to solve transform
    if (static_cast<int>(confirmedPairs.size()) < minMatches)
        return AlignmentResult{};

    // Step 6: Solve similarity transform from confirmed pairs.
    // Try both normal and horizontally flipped orientations, take lower RMS.
    auto solveTransform = [&](const std::vector<std::pair<int,int>>& pairs,
                              bool flip, int frameWidth) -> AlignmentResult
    {
        // Build least-squares system for similarity transform:
        //   ref_x = a * tgt_x - b * tgt_y + tx
        //   ref_y = b * tgt_x + a * tgt_y + ty
        // Normal equations: A^T A x = A^T b  (4x4 system)
        int N = static_cast<int>(pairs.size());
        double AtA[4][4] = {};
        double Atb[4] = {};

        for (const auto& [ri, ti] : pairs) {
            double rx = refStars[ri].x;
            double ry = refStars[ri].y;
            double tgtX = flip ? (frameWidth - 1 - targetStars[ti].x) : targetStars[ti].x;
            double tgtY = targetStars[ti].y;

            // Row 1: ref_x = a*tgtX - b*tgtY + tx
            double row1[4] = { tgtX, -tgtY, 1.0, 0.0 };
            // Row 2: ref_y = b*tgtX + a*tgtY + ty
            double row2[4] = { tgtY, tgtX, 0.0, 1.0 };

            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    AtA[i][j] += row1[i] * row1[j] + row2[i] * row2[j];
                }
                Atb[i] += row1[i] * rx + row2[i] * ry;
            }
        }

        // Solve 4x4 system via Gaussian elimination with partial pivoting
        double aug[4][5];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) aug[i][j] = AtA[i][j];
            aug[i][4] = Atb[i];
        }

        for (int col = 0; col < 4; ++col) {
            int pivot = col;
            for (int row = col + 1; row < 4; ++row)
                if (std::abs(aug[row][col]) > std::abs(aug[pivot][col]))
                    pivot = row;
            if (pivot != col)
                for (int j = 0; j < 5; ++j)
                    std::swap(aug[col][j], aug[pivot][j]);
            if (std::abs(aug[col][col]) < 1e-12)
                return AlignmentResult{};  // singular
            for (int row = col + 1; row < 4; ++row) {
                double factor = aug[row][col] / aug[col][col];
                for (int j = col; j < 5; ++j)
                    aug[row][j] -= factor * aug[col][j];
            }
        }

        double x[4];
        for (int i = 3; i >= 0; --i) {
            x[i] = aug[i][4];
            for (int j = i + 1; j < 4; ++j)
                x[i] -= aug[i][j] * x[j];
            x[i] /= aug[i][i];
        }

        double sa = x[0], sb = x[1], stx = x[2], sty = x[3];

        // Compute RMS residual
        double sumSq = 0.0;
        for (const auto& [ri, ti] : pairs) {
            double tgtX = flip ? (frameWidth - 1 - targetStars[ti].x) : targetStars[ti].x;
            double tgtY = targetStars[ti].y;
            double predX = sa * tgtX - sb * tgtY + stx;
            double predY = sb * tgtX + sa * tgtY + sty;
            double ex = predX - refStars[ri].x;
            double ey = predY - refStars[ri].y;
            sumSq += ex * ex + ey * ey;
        }
        double rms = std::sqrt(sumSq / N);

        // Build result
        AlignmentResult res;
        res.a = sa;
        res.b = sb;
        res.tx = stx;
        res.ty = sty;
        res.flipped = flip;
        res.scale = std::sqrt(sa * sa + sb * sb);
        res.rotation = std::atan2(sb, sa) * (180.0 / 3.14159265358979323846);
        // dx/dy use old convention (target - ref) for logging and crop compat
        res.dx = -static_cast<int>(std::round(stx));
        res.dy = -static_cast<int>(std::round(sty));
        res.numMatchedStars = N;
        res.convergenceRMS = rms;
        res.valid = true;
        return res;
    };

    // Frame width for flip detection
    int estFrameWidth = frameWidth;
    if (estFrameWidth <= 0) {
        // Estimate from max target star x coordinate
        double maxTgtX = 0;
        for (const auto& [ri, ti] : confirmedPairs)
            maxTgtX = std::max(maxTgtX, targetStars[ti].x);
        estFrameWidth = static_cast<int>(maxTgtX) + 100;
    }

    // Step 7: Geometric consistency filter using initial translation estimate.
    // Compute median dx/dy for a rough filter before fitting the full transform.
    std::vector<double> dxVals, dyVals;
    dxVals.reserve(confirmedPairs.size());
    dyVals.reserve(confirmedPairs.size());
    for (const auto& [ri, ti] : confirmedPairs) {
        dxVals.push_back(targetStars[ti].x - refStars[ri].x);
        dyVals.push_back(targetStars[ti].y - refStars[ri].y);
    }
    std::sort(dxVals.begin(), dxVals.end());
    std::sort(dyVals.begin(), dyVals.end());

    auto median = [](const std::vector<double>& v) -> double {
        size_t n = v.size();
        return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    };

    double medDx = median(dxVals);
    double medDy = median(dyVals);

    std::vector<std::pair<int,int>> filteredPairs;
    filteredPairs.reserve(confirmedPairs.size());
    for (const auto& [ri, ti] : confirmedPairs) {
        double residX = (targetStars[ti].x - refStars[ri].x) - medDx;
        double residY = (targetStars[ti].y - refStars[ri].y) - medDy;
        if (residX * residX + residY * residY < 25.0)
            filteredPairs.push_back({ri, ti});
    }
    if (static_cast<int>(filteredPairs.size()) < minMatches)
        filteredPairs = confirmedPairs;

    // Step 8: Solve for normal orientation
    AlignmentResult normalResult = solveTransform(filteredPairs, false, estFrameWidth);

    // Step 9: If normal RMS is bad, try flipped orientation with all confirmed pairs
    AlignmentResult flippedResult;
    if (!normalResult.valid || normalResult.convergenceRMS > 2.0)
        flippedResult = solveTransform(confirmedPairs, true, estFrameWidth);

    // Step 10: Pick the best result
    AlignmentResult best;
    if (normalResult.valid && flippedResult.valid)
        best = (normalResult.convergenceRMS <= flippedResult.convergenceRMS) ? normalResult : flippedResult;
    else if (normalResult.valid)
        best = normalResult;
    else if (flippedResult.valid)
        best = flippedResult;
    else
        return AlignmentResult{};

    return best;
}

} // namespace nukex
