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

#include "FrameStreamer.h"

#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// FrameStreamer Implementation
// ----------------------------------------------------------------------------

FrameStreamer::~FrameStreamer()
{
   StopPrefetchThread();
   Close();
}

// ----------------------------------------------------------------------------

void FrameStreamer::Close()
{
   // Stop any in-flight prefetch before closing file handles
   StopPrefetchThread();

   for ( FrameInfo& frame : m_frames )
   {
      try
      {
         if ( frame.isOpen && frame.file )
         {
            frame.file->Close();
            frame.isOpen = false;
         }
         frame.file.reset();
         frame.format.reset();
      }
      catch ( ... )
      {
         // Swallow exceptions during cleanup
      }
   }

   m_frames.clear();
   m_rowCache.clear();
   m_cachedRow = -1;
   m_cachedChannel = -1;
   m_prefetchBuffer.clear();
   m_prefetchRow = -1;
   m_prefetchChannel = -1;
   m_prefetchError = false;
   m_width = 0;
   m_height = 0;
   m_channels = 0;
}

// ----------------------------------------------------------------------------

bool FrameStreamer::Initialize( const std::vector<String>& paths )
{
   Console console;

   // Clean up any prior state
   Close();

   if ( paths.empty() )
   {
      console.CriticalLn( "FrameStreamer: No frame paths provided" );
      return false;
   }

   int numFrames = static_cast<int>( paths.size() );
   m_frames.reserve( numFrames );

   for ( int i = 0; i < numFrames; ++i )
   {
      const String& path = paths[i];

      try
      {
         // Determine file format from extension
         String extension = File::ExtractExtension( path ).Lowercase();

         auto format = std::make_unique<FileFormat>( extension, true/*read*/, false/*write*/ );
         auto file = std::make_unique<FileFormatInstance>( *format );

         // Open the file
         ImageDescriptionArray images;
         if ( !file->Open( images, path ) )
         {
            console.CriticalLn( "FrameStreamer: Failed to open file: " + path );
            Close();
            return false;
         }

         // Validate image descriptions
         if ( images.IsEmpty() )
         {
            console.CriticalLn( "FrameStreamer: No images in file: " + path );
            file->Close();
            Close();
            return false;
         }

         int w = images[0].info.width;
         int h = images[0].info.height;
         int c = images[0].info.numberOfChannels;

         if ( w <= 0 || h <= 0 || c <= 0 )
         {
            console.CriticalLn( "FrameStreamer: Invalid dimensions in file: " + path );
            file->Close();
            Close();
            return false;
         }

         // Extract FITS keywords if supported
         FITSKeywordArray keywords;
         if ( format->CanStoreKeywords() )
            file->ReadFITSKeywords( keywords );

         // First frame sets reference dimensions
         if ( i == 0 )
         {
            m_width = w;
            m_height = h;
            m_channels = c;
         }
         else
         {
            // Subsequent frames must match
            if ( w != m_width || h != m_height || c != m_channels )
            {
               console.CriticalLn( String().Format(
                  "FrameStreamer: Dimension mismatch in frame %d: %dx%dx%d vs %dx%dx%d",
                  i, w, h, c, m_width, m_height, m_channels ) );
               file->Close();
               Close();
               return false;
            }
         }

         // Store frame info - file stays open for streaming
         FrameInfo info;
         info.path     = path;
         info.keywords = std::move( keywords );
         info.format   = std::move( format );
         info.file     = std::move( file );
         info.isOpen   = true;

         m_frames.push_back( std::move( info ) );
      }
      catch ( const Exception& e )
      {
         console.CriticalLn( "FrameStreamer: PCL exception opening frame " +
            String( i ) + ": " + e.Message() );
         Close();
         return false;
      }
      catch ( const std::exception& e )
      {
         console.CriticalLn( "FrameStreamer: Exception opening frame " +
            String( i ) + ": " + String( e.what() ) );
         Close();
         return false;
      }
      catch ( ... )
      {
         console.CriticalLn( "FrameStreamer: Unknown exception opening frame " + String( i ) );
         Close();
         return false;
      }
   }

   // Allocate row cache
   m_rowCache.resize( numFrames );
   for ( int f = 0; f < numFrames; ++f )
      m_rowCache[f].resize( m_width );

   m_cachedRow = -1;
   m_cachedChannel = -1;

   console.WriteLn( String().Format(
      "FrameStreamer: Initialized %d frames (%dx%dx%d) for streaming",
      numFrames, m_width, m_height, m_channels ) );

   return true;
}

// ----------------------------------------------------------------------------

bool FrameStreamer::ReadRowInternal( int y, int channel,
                                     std::vector<std::vector<float>>& buffer )
{
   int numFrames = static_cast<int>( m_frames.size() );

   // Resize buffer to [numFrames][width]
   buffer.resize( numFrames );
   for ( int f = 0; f < numFrames; ++f )
      buffer[f].resize( m_width );

   // Read one row from each frame
   for ( int f = 0; f < numFrames; ++f )
   {
      if ( !m_frames[f].isOpen )
         if ( !OpenFrame( m_frames[f] ) )
            return false;

      FImage::sample* dest = buffer[f].data();
      if ( !m_frames[f].file->ReadSamples( dest, y, 1, channel ) )
         return false;
   }

   return true;
}

// ----------------------------------------------------------------------------

void FrameStreamer::StartPrefetch( int nextRow, int channel )
{
   // Stop any in-flight prefetch first
   StopPrefetchThread();

   m_stopPrefetch.store( false );
   m_prefetchReady.store( false );
   m_prefetchRow = nextRow;
   m_prefetchChannel = channel;
   m_prefetchError = false;

   m_prefetchThread = std::make_unique<std::thread>( [this, nextRow, channel]()
   {
      if ( m_stopPrefetch.load() )
         return;

      m_prefetchError = !ReadRowInternal( nextRow, channel, m_prefetchBuffer );
      m_prefetchReady.store( true );
   } );
}

// ----------------------------------------------------------------------------

bool FrameStreamer::WaitForPrefetch()
{
   if ( m_prefetchThread && m_prefetchThread->joinable() )
      m_prefetchThread->join();
   m_prefetchThread.reset();
   return !m_prefetchError;
}

// ----------------------------------------------------------------------------

void FrameStreamer::StopPrefetchThread()
{
   m_stopPrefetch.store( true );
   if ( m_prefetchThread && m_prefetchThread->joinable() )
      m_prefetchThread->join();
   m_prefetchThread.reset();
   m_stopPrefetch.store( false );
   m_prefetchReady.store( false );
}

// ----------------------------------------------------------------------------

bool FrameStreamer::ReadRow( int y, int channel,
                             std::vector<std::vector<float>>& rowData )
{
   Console console;

   int numFrames = static_cast<int>( m_frames.size() );

   if ( numFrames == 0 )
   {
      console.CriticalLn( "FrameStreamer::ReadRow: No frames initialized" );
      return false;
   }

   if ( y < 0 || y >= m_height )
   {
      console.CriticalLn( String().Format(
         "FrameStreamer::ReadRow: Row %d out of range [0, %d)", y, m_height ) );
      return false;
   }

   if ( channel < 0 || channel >= m_channels )
   {
      console.CriticalLn( String().Format(
         "FrameStreamer::ReadRow: Channel %d out of range [0, %d)", channel, m_channels ) );
      return false;
   }

   // Check primary cache - return cached data if same row/channel
   if ( y == m_cachedRow && channel == m_cachedChannel )
   {
      rowData = m_rowCache;

      // Prefetch next row in background
      if ( y + 1 < m_height )
         StartPrefetch( y + 1, channel );

      return true;
   }

   // Check if the prefetch buffer has the row we need
   if ( m_prefetchReady.load() && y == m_prefetchRow && channel == m_prefetchChannel )
   {
      if ( !WaitForPrefetch() )
      {
         console.CriticalLn( String().Format(
            "FrameStreamer::ReadRow: Prefetch failed for row %d, channel %d",
            y, channel ) );
         return false;
      }

      // Move prefetch buffer into output
      rowData = std::move( m_prefetchBuffer );

      // Update primary cache
      m_cachedRow = y;
      m_cachedChannel = channel;
      m_rowCache = rowData;  // copy for cache

      // Start prefetching the next row
      if ( y + 1 < m_height )
         StartPrefetch( y + 1, channel );

      return true;
   }

   // No cache hit - stop any in-flight prefetch, then read directly
   StopPrefetchThread();

   if ( !ReadRowInternal( y, channel, rowData ) )
   {
      console.CriticalLn( String().Format(
         "FrameStreamer::ReadRow: ReadRowInternal failed for row %d, channel %d",
         y, channel ) );
      return false;
   }

   // Update primary cache
   m_cachedRow = y;
   m_cachedChannel = channel;
   m_rowCache = rowData;

   // Start prefetching the next row
   if ( y + 1 < m_height )
      StartPrefetch( y + 1, channel );

   return true;
}

// ----------------------------------------------------------------------------

bool FrameStreamer::OpenFrame( FrameInfo& frame )
{
   if ( frame.isOpen )
      return true;

   try
   {
      // Re-create the FileFormatInstance from the stored format
      ImageDescriptionArray images;
      frame.file = std::make_unique<FileFormatInstance>( *frame.format );

      if ( !frame.file->Open( images, frame.path ) )
      {
         Console().CriticalLn( "FrameStreamer::OpenFrame: Failed to reopen: " + frame.path );
         return false;
      }

      frame.isOpen = true;
      return true;
   }
   catch ( const Exception& e )
   {
      Console().CriticalLn( "FrameStreamer::OpenFrame: PCL exception: " + e.Message() );
      return false;
   }
   catch ( ... )
   {
      Console().CriticalLn( "FrameStreamer::OpenFrame: Unknown exception reopening: " + frame.path );
      return false;
   }
}

// ----------------------------------------------------------------------------

std::vector<FITSKeywordArray> FrameStreamer::GetAllKeywords() const
{
   std::vector<FITSKeywordArray> result;
   result.reserve( m_frames.size() );
   for ( const auto& frame : m_frames )
      result.push_back( frame.keywords );
   return result;
}

// ----------------------------------------------------------------------------

} // namespace pcl
