//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// FrameStreamer - Row-based streaming reader for FITS/XISF frames
// Reads one row at a time from all frames to avoid loading full images into RAM

#ifndef __FrameStreamer_h
#define __FrameStreamer_h

#include <pcl/Image.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/FileFormat.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Console.h>
#include <pcl/String.h>

#include <vector>
#include <memory>

#include "BayerDemosaic.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// FrameStreamer - Incremental row-based reader for frame stacks
// ----------------------------------------------------------------------------
//
// Reads pixel data one row at a time from all opened frames using
// FileFormatInstance::ReadSamples(). This avoids loading entire frame
// stacks into memory, which is critical for large images or many frames.
//
// Usage:
//    FrameStreamer streamer;
//    if ( !streamer.Initialize( framePaths ) )
//       return;
//
//    std::vector<std::vector<float>> rowData;
//    for ( int y = 0; y < streamer.Height(); ++y )
//    {
//       streamer.ReadRow( y, channel, rowData );
//       // rowData[frameIndex][x] = pixel value
//    }
//    streamer.Close();
// ----------------------------------------------------------------------------

class FrameStreamer
{
public:

   /// Default constructor
   FrameStreamer() = default;

   /// Destructor - closes all open file handles
   ~FrameStreamer();

   /// Non-copyable
   FrameStreamer( const FrameStreamer& ) = delete;
   FrameStreamer& operator=( const FrameStreamer& ) = delete;

   /// Non-moveable
   FrameStreamer( FrameStreamer&& ) = delete;
   FrameStreamer& operator=( FrameStreamer&& ) = delete;

   // ----------------------------------------------------------------------------
   // Initialization
   // ----------------------------------------------------------------------------

   /// Open all frame files, validate dimensions, extract FITS keywords.
   /// All frames must have identical width, height, and channel count.
   /// @param paths File paths to open (FITS, XISF, or any PCL-supported format)
   /// @return false on failure (dimension mismatch, unreadable files, etc.)
   bool Initialize( const std::vector<String>& paths );

   // ----------------------------------------------------------------------------
   // Frame information
   // ----------------------------------------------------------------------------

   /// Number of frames successfully opened
   int NumFrames() const { return static_cast<int>( m_frames.size() ); }

   /// Image width (all frames match)
   int Width() const { return m_width; }

   /// Image height (all frames match)
   int Height() const { return m_height; }

   /// Number of channels (all frames match)
   int Channels() const { return m_channels; }

   /// Returns true if the opened frames contain CFA/Bayer data that will be demosaiced
   bool IsCFA() const { return m_isCFA; }

   // ----------------------------------------------------------------------------
   // Row-based streaming reads
   // ----------------------------------------------------------------------------

   /// Read a single row from ALL frames for a given channel.
   /// Results are cached; re-reading the same (y, channel) is a no-op.
   /// @param y Row index (0 to Height()-1)
   /// @param channel Channel index (0 to Channels()-1)
   /// @param[out] rowData Resized to [numFrames][width], populated with pixel values
   /// @return false on I/O error
   bool ReadRow( int y, int channel, std::vector<std::vector<float>>& rowData );

   // ----------------------------------------------------------------------------
   // FITS keyword access
   // ----------------------------------------------------------------------------

   /// Get FITS keywords for a specific frame
   /// @param frameIndex Frame index (0 to NumFrames()-1)
   const FITSKeywordArray& GetKeywords( int frameIndex ) const
   {
      return m_frames[frameIndex].keywords;
   }

   /// Get all keyword arrays (one per frame)
   std::vector<FITSKeywordArray> GetAllKeywords() const;

   // ----------------------------------------------------------------------------
   // Path access
   // ----------------------------------------------------------------------------

   /// Get the file path for a specific frame
   /// @param frameIndex Frame index (0 to NumFrames()-1)
   const String& GetPath( int frameIndex ) const
   {
      return m_frames[frameIndex].path;
   }

   // ----------------------------------------------------------------------------
   // Lifecycle
   // ----------------------------------------------------------------------------

   /// Close all open file handles. Called automatically by destructor.
   void Close();

   /// Close and reopen all frame files to reset their internal read state.
   /// Needed between passes (e.g., median reference -> pixel selection)
   /// and when the read position must seek backward or change channels.
   /// Preserves all extracted FITS keywords (stored in FrameInfo, not file handles).
   /// @return false if any frame fails to reopen
   bool ResetAllFiles();

private:

   // Per-frame tracking structure
   struct FrameInfo
   {
      String                            path;
      FITSKeywordArray                  keywords;
      std::unique_ptr<FileFormat>       format;
      std::unique_ptr<FileFormatInstance> file;
      bool                              isOpen = false;
      int                               hduIndex = 0;          // Which HDU/image index to use in multi-extension files
   };

   std::vector<FrameInfo> m_frames;

   int m_width    = 0;
   int m_height   = 0;
   int m_channels = 0;

   // Row cache: avoids redundant re-reads of the same row
   int m_cachedRow     = -1;
   int m_cachedChannel = -1;
   std::vector<std::vector<float>> m_rowCache;

   // Tracking for backward-seek and channel-change detection
   int m_lastReadRow     = -1;
   int m_lastReadChannel = -1;

   bool m_isMultiExtension = false;  // True if files are multi-extension FITS (separate HDUs per channel)

   bool m_isCFA = false;
   BayerPattern m_bayerPattern = BayerPattern::Unknown;
   int m_physicalChannels = 0;  // Actual channels in file (1 for CFA, expanded to 3 logically)

   // Per-frame rolling raw row buffer (3 rows for bilinear demosaic)
   struct CfaRawBuffer
   {
      std::vector<float> rows[3];  // [0]=prev, [1]=cur, [2]=next
      int nextRawY = 0;            // Next raw row to read from file
   };
   std::vector<CfaRawBuffer> m_cfaBuffers;

   /// Open a single frame file for streaming reads
   /// @param frame FrameInfo to populate with open file handle
   /// @return false on failure
   bool OpenFrame( FrameInfo& frame );

   /// Read a single row for CFA data, demosaicing on the fly
   bool ReadRowCFA( int y, int channel, std::vector<std::vector<float>>& rowData );

   /// Read the next sequential raw CFA row from a frame file
   bool ReadRawCfaRow( int frameIndex, float* dest );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __FrameStreamer_h
