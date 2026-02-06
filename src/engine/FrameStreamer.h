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

   /// Moveable
   FrameStreamer( FrameStreamer&& ) = default;
   FrameStreamer& operator=( FrameStreamer&& ) = default;

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

private:

   // Per-frame tracking structure
   struct FrameInfo
   {
      String                            path;
      FITSKeywordArray                  keywords;
      std::unique_ptr<FileFormat>       format;
      std::unique_ptr<FileFormatInstance> file;
      bool                              isOpen = false;
   };

   std::vector<FrameInfo> m_frames;

   int m_width    = 0;
   int m_height   = 0;
   int m_channels = 0;

   // Row cache: avoids redundant re-reads of the same row
   int m_cachedRow     = -1;
   int m_cachedChannel = -1;
   std::vector<std::vector<float>> m_rowCache;

   /// Open a single frame file for streaming reads
   /// @param frame FrameInfo to populate with open file handle
   /// @return false on failure
   bool OpenFrame( FrameInfo& frame );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __FrameStreamer_h
