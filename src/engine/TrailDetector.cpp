// src/engine/TrailDetector.cpp
#include "engine/TrailDetector.h"
#include "engine/SubCube.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>
#include <vector>
#include <utility>

namespace nukex {

TrailDetector::TrailDetector( const TrailDetectorConfig& config )
    : m_config( config )
{
}

FrameTrailResult TrailDetector::detectFrame( const float* /*frameData*/,
                                              const uint8_t* /*alignMask*/,
                                              int /*width*/, int /*height*/ ) const
{
    return {};  // stub
}

int TrailDetector::detectAndMask( SubCube& /*cube*/, LogCallback /*log*/ ) const
{
    return 0;   // stub
}

std::vector<uint8_t> TrailDetector::findSeeds( const float* /*frameData*/,
                                                const uint8_t* /*alignMask*/,
                                                int width, int height ) const
{
    return std::vector<uint8_t>( width * height, 0 );
}

std::vector<TrailDetector::Cluster> TrailDetector::clusterSeeds(
    const std::vector<uint8_t>& /*seeds*/, int /*width*/, int /*height*/ ) const
{
    return {};
}

std::vector<uint8_t> TrailDetector::walkAndConfirm(
    const float* /*frameData*/, const uint8_t* /*alignMask*/,
    const std::vector<Cluster>& /*clusters*/,
    int width, int height,
    std::vector<TrailLine>& /*linesOut*/ ) const
{
    return std::vector<uint8_t>( width * height, 0 );
}

} // namespace nukex
