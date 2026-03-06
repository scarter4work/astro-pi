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

#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/StatusMonitor.h>
#include <pcl/StandardStatus.h>
#include <pcl/Console.h>
#include <pcl/Image.h>
#include <pcl/File.h>

#include <vector>

namespace nukex {

// Describes a single frame path with an enabled/disabled flag.
struct FramePath {
    pcl::String path;
    bool enabled = true;
};

class FrameLoader {
public:
    // Load all enabled frames into a SubCube.
    //
    // 1. Filter to enabled frames only
    // 2. Open first frame to get dimensions (width, height, channels)
    // 3. Allocate SubCube(nEnabled, height, width)
    // 4. For each enabled frame: read image data, copy into cube, extract FITS metadata
    // 5. Report progress via Console
    //
    // Throws: if no enabled frames, if dimension mismatch, if file I/O error
    static SubCube Load( const std::vector<FramePath>& frames );

private:
    // Extract SubMetadata from FITS keywords
    static SubMetadata ExtractMetadata( const pcl::FITSKeywordArray& keywords );

    // Helper: get numeric value from FITS keyword, or default if not found
    static double GetKeywordValue( const pcl::FITSKeywordArray& keywords,
                                   const pcl::IsoString& name,
                                   double defaultValue );

    // Helper: get string value from FITS keyword, or default
    static pcl::String GetKeywordString( const pcl::FITSKeywordArray& keywords,
                                         const pcl::IsoString& name,
                                         const pcl::String& defaultValue );
};

} // namespace nukex
