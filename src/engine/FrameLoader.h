//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX3 - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// FrameLoader - Reads FITS/XISF frames into a SubCube

#pragma once

#include "SubCube.h"
#include "FrameAligner.h"

#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/StatusMonitor.h>
#include <pcl/StandardStatus.h>
#include <pcl/Console.h>
#include <pcl/Image.h>
#include <pcl/File.h>
#include <pcl/StarDetector.h>
#include <pcl/PSFFit.h>

#include <vector>

namespace nukex {

// Describes a single frame path with an enabled/disabled flag.
struct FramePath {
    pcl::String path;
    bool enabled = true;
};

// Raw frame data for alignment pipeline (before building SubCube).
struct LoadedFrames {
    std::vector<std::vector<std::vector<float>>> pixelData;  // [frame][channel][pixels]
    std::vector<SubMetadata> metadata;
    int width, height, numChannels;
};

// Bayer CFA pattern (2x2 tile describing the color filter layout)
enum class BayerPattern { RGGB, GRBG, GBRG, BGGR, None };

class FrameLoader {
public:
    // Load all enabled frames into a SubCube.
    static SubCube Load( const std::vector<FramePath>& frames );

    // Load raw frame data without building SubCube (for alignment pipeline).
    // Automatically debayers single-channel CFA images into 3-channel RGB.
    static LoadedFrames LoadRaw( const std::vector<FramePath>& frames );

private:
    static SubMetadata ExtractMetadata( const pcl::FITSKeywordArray& keywords );

    static double GetKeywordValue( const pcl::FITSKeywordArray& keywords,
                                   const pcl::IsoString& name,
                                   double defaultValue );

    static pcl::String GetKeywordString( const pcl::FITSKeywordArray& keywords,
                                         const pcl::IsoString& name,
                                         const pcl::String& defaultValue );

    // Detect Bayer pattern from FITS keywords
    static BayerPattern DetectBayerPattern( const pcl::FITSKeywordArray& keywords );

    // Debayer a single-channel CFA image into 3-channel RGB (bilinear interpolation)
    static void DebayerBilinear( const float* cfa, int width, int height,
                                  BayerPattern pattern,
                                  std::vector<float>& outR,
                                  std::vector<float>& outG,
                                  std::vector<float>& outB );

    // Compute FWHM, eccentricity, HFR, and sky background using PCL star
    // detection + PSF fitting. Called when FITS headers lack quality metrics.
    static void ComputeFrameMetrics( const pcl::Image& img, SubMetadata& meta );
};

} // namespace nukex
