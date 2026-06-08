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
   Close();
}

// ----------------------------------------------------------------------------

void FrameStreamer::Close()
{
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
   m_lastReadRow = -1;
   m_lastReadChannel = -1;
   m_width = 0;
   m_height = 0;
   m_channels = 0;
   m_isMultiExtension = false;
   m_isCFA = false;
   m_bayerPattern = BayerPattern::Unknown;
   m_physicalChannels = 0;
   m_cfaBuffers.clear();
}

// ----------------------------------------------------------------------------

bool FrameStreamer::ResetAllFiles()
{
   Console console;

   for ( int f = 0; f < static_cast<int>( m_frames.size() ); ++f )
   {
      try
      {
         if ( m_frames[f].isOpen && m_frames[f].file )
         {
            m_frames[f].file->Close();
            m_frames[f].isOpen = false;
         }
         m_frames[f].file.reset();

         // Reopen - keywords are preserved in FrameInfo, not the file handle
         if ( !OpenFrame( m_frames[f] ) )
         {
            console.CriticalLn( String().Format(
               "FrameStreamer::ResetAllFiles: Failed to reopen frame %d: %s",
               f, IsoString( m_frames[f].path ).c_str() ) );
            return false;
         }
      }
      catch ( const Exception& e )
      {
         console.CriticalLn( "FrameStreamer::ResetAllFiles: PCL exception: " + e.Message() );
         return false;
      }
      catch ( ... )
      {
         console.CriticalLn( String().Format(
            "FrameStreamer::ResetAllFiles: Unknown exception for frame %d", f ) );
         return false;
      }
   }

   // Invalidate cache and tracking state
   m_cachedRow = -1;
   m_cachedChannel = -1;
   m_lastReadRow = -1;
   m_lastReadChannel = -1;

   // Reset CFA buffer state
   for ( auto& buf : m_cfaBuffers )
      buf.nextRawY = 0;

   return true;
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

         // Detect multi-extension FITS and determine effective dimensions/channels
         int w = 0, h = 0, c = 0;
         int primaryHDU = 0;

         if ( i == 0 )
         {
            console.WriteLn( String().Format( "  File has %d image description(s)", images.Length() ) );
            for ( int d = 0; d < images.Length(); ++d )
            {
               console.WriteLn( String().Format( "    [%d] %dx%d, %d channel(s), %d bps",
                  d, images[d].info.width, images[d].info.height,
                  images[d].info.numberOfChannels, images[d].options.bitsPerSample ) );
            }
         }

         // Count valid HDUs (non-empty image data)
         std::vector<int> validHDUs;
         for ( int d = 0; d < images.Length(); ++d )
         {
            if ( images[d].info.width > 0 && images[d].info.height > 0 &&
                 images[d].info.numberOfChannels >= 1 )
               validHDUs.push_back( d );
         }

         if ( validHDUs.empty() )
         {
            console.CriticalLn( "FrameStreamer: No valid image data in file: " + path );
            file->Close();
            Close();
            return false;
         }

         // Check for multi-extension RGB: multiple valid single-channel HDUs with same dimensions
         bool isMultiExt = false;
         if ( validHDUs.size() > 1 )
         {
            bool allSingleChannel = true;
            bool allSameDims = true;
            int refW = images[validHDUs[0]].info.width;
            int refH = images[validHDUs[0]].info.height;

            for ( int vi : validHDUs )
            {
               if ( images[vi].info.numberOfChannels != 1 )
                  allSingleChannel = false;
               if ( images[vi].info.width != refW || images[vi].info.height != refH )
                  allSameDims = false;
            }

            if ( allSingleChannel && allSameDims )
            {
               isMultiExt = true;
               w = refW;
               h = refH;
               c = static_cast<int>( validHDUs.size() );
               primaryHDU = validHDUs[0];

               if ( i == 0 )
                  console.WriteLn( String().Format(
                     "  Multi-extension FITS: %d channels across %d HDUs", c, images.Length() ) );
            }
            else
            {
               // Multiple HDUs but not uniform - use first valid
               int vi = validHDUs[0];
               w = images[vi].info.width;
               h = images[vi].info.height;
               c = images[vi].info.numberOfChannels;
               primaryHDU = vi;
            }
         }
         else
         {
            // Single valid HDU
            int vi = validHDUs[0];
            w = images[vi].info.width;
            h = images[vi].info.height;
            c = images[vi].info.numberOfChannels;
            primaryHDU = vi;
         }

         if ( i == 0 )
            m_isMultiExtension = isMultiExt;

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

         // Check for CFA/Bayer pattern (first frame only)
         if ( i == 0 )
         {
            for ( const FITSHeaderKeyword& kw : keywords )
            {
               if ( kw.name.Uppercase().Trimmed() == "BAYERPAT" )
               {
                  BayerPattern bp = ParseBayerPattern( IsoString( kw.value.Trimmed() ) );
                  if ( bp != BayerPattern::Unknown )
                  {
                     m_isCFA = true;
                     m_bayerPattern = bp;
                     console.WriteLn( String().Format(
                        "  CFA/Bayer data detected (pattern: %s) - will demosaic to RGB",
                        IsoString( kw.value.Trimmed() ).c_str() ) );
                  }
                  break;
               }
            }
         }

         // Verify incremental read support (required for row-based streaming)
         if ( !file->CanReadIncrementally() )
         {
            console.CriticalLn( String().Format(
               "FrameStreamer: File does not support incremental reads: %s",
               IsoString( path ).c_str() ) );
            file->Close();
            Close();
            return false;
         }

         // First frame sets reference dimensions
         if ( i == 0 )
         {
            m_width = w;
            m_height = h;
            m_physicalChannels = c;
            m_channels = m_isCFA ? 3 : c;  // CFA reports 3 logical channels
         }
         else
         {
            // Subsequent frames must match physical dimensions
            if ( w != m_width || h != m_height || c != m_physicalChannels )
            {
               console.CriticalLn( String().Format(
                  "FrameStreamer: Dimension mismatch in frame %d: %dx%dx%d vs %dx%dx%d",
                  i, w, h, c, m_width, m_height, m_physicalChannels ) );
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
         info.hduIndex = primaryHDU;

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
   m_lastReadRow = -1;
   m_lastReadChannel = -1;

   // Allocate CFA demosaic buffers if needed
   if ( m_isCFA )
   {
      m_cfaBuffers.resize( numFrames );
      for ( auto& buf : m_cfaBuffers )
      {
         for ( int r = 0; r < 3; r++ )
            buf.rows[r].resize( m_width );
         buf.nextRawY = 0;
      }
   }

   // Log format summary
   String colorType;
   if ( m_isCFA )
      colorType = "CFA->RGB (demosaiced)";
   else if ( m_isMultiExtension )
      colorType = String().Format( "%d-channel multi-extension", m_channels );
   else if ( m_channels >= 3 )
      colorType = "RGB";
   else
      colorType = "mono";

   console.WriteLn( String().Format(
      "FrameStreamer: Initialized %d frames (%dx%dx%d, %s) for streaming",
      numFrames, m_width, m_height, m_channels,
      IsoString( colorType ).c_str() ) );

   return true;
}

// ----------------------------------------------------------------------------

bool FrameStreamer::ReadRow( int y, int channel,
                             std::vector<std::vector<float>>& rowData )
{
   // Bounds check
   if ( y < 0 || y >= m_height || channel < 0 || channel >= m_channels )
   {
      Console().CriticalLn( String().Format(
         "FrameStreamer: Row/channel out of bounds (row=%d/%d, channel=%d/%d)",
         y, m_height, channel, m_channels ) );
      return false;
   }

   // CFA data: use demosaic path
   if ( m_isCFA )
      return ReadRowCFA( y, channel, rowData );

   // Check cache
   if ( y == m_cachedRow && channel == m_cachedChannel && !m_rowCache.empty() )
   {
      rowData = m_rowCache;
      return true;
   }

   // Detect backward seek or channel change that requires file reset.
   // PCL incremental reads may not support seeking backward, so we must
   // close and reopen all files to reset their internal read position.
   bool needReset = false;
   if ( m_lastReadChannel >= 0 && channel != m_lastReadChannel )
      needReset = true;  // Channel changed - must reset
   else if ( m_lastReadRow >= 0 && y < m_lastReadRow )
      needReset = true;  // Backward seek within same channel

   if ( needReset )
   {
      if ( !ResetAllFiles() )
      {
         Console().CriticalLn( "FrameStreamer: Failed to reset files for seek/channel change" );
         return false;
      }
   }

   int numFrames = static_cast<int>( m_frames.size() );

   // Resize output
   rowData.resize( numFrames );
   for ( int f = 0; f < numFrames; ++f )
      rowData[f].resize( m_width );

   // Read from each frame
   for ( int f = 0; f < numFrames; ++f )
   {
      if ( !m_frames[f].isOpen )
      {
         if ( !OpenFrame( m_frames[f] ) )
         {
            Console().CriticalLn( String().Format(
               "FrameStreamer: Failed to reopen frame %d", f ) );
            return false;
         }
      }

      // Verify incremental read support
      if ( !m_frames[f].file->CanReadIncrementally() )
      {
         Console().CriticalLn( String().Format(
            "FrameStreamer: Frame %d does not support incremental reads", f ) );
         return false;
      }

      // For multi-extension FITS, select the correct image/HDU for this channel
      int readChannel = channel;
      if ( m_isMultiExtension )
      {
         // Each channel is a separate HDU; select it, then read channel 0
         if ( !m_frames[f].file->SelectImage( channel ) )
         {
            Console().CriticalLn( String().Format(
               "FrameStreamer: SelectImage(%d) failed for frame %d", channel, f ) );
            return false;
         }
         readChannel = 0;  // Within each HDU, data is channel 0
      }

      FImage::sample* dest = rowData[f].data();
      try
      {
         if ( !m_frames[f].file->ReadSamples( dest, y, 1, readChannel ) )
         {
            Console().CriticalLn( String().Format(
               "FrameStreamer: ReadSamples failed for frame %d, row %d, channel %d",
               f, y, channel ) );
            return false;
         }
      }
      catch ( ... )
      {
         Console().CriticalLn( String().Format(
            "FrameStreamer: Exception reading frame %d, row %d, channel %d",
            f, y, channel ) );
         return false;
      }
   }

   // Update cache
   m_cachedRow = y;
   m_cachedChannel = channel;
   m_rowCache = rowData;

   // Update tracking for backward-seek/channel-change detection
   m_lastReadRow = y;
   m_lastReadChannel = channel;

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

      // For multi-extension files, select the correct starting HDU
      if ( m_isMultiExtension && frame.hduIndex > 0 )
      {
         if ( !frame.file->SelectImage( frame.hduIndex ) )
         {
            Console().CriticalLn( "FrameStreamer::OpenFrame: SelectImage failed for: " + frame.path );
            return false;
         }
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

bool FrameStreamer::ReadRawCfaRow( int frameIndex, float* dest )
{
   FrameInfo& frame = m_frames[frameIndex];
   CfaRawBuffer& buf = m_cfaBuffers[frameIndex];

   if ( buf.nextRawY >= m_height )
      return false;

   if ( !frame.isOpen )
   {
      if ( !OpenFrame( frame ) )
         return false;
   }

   try
   {
      if ( !frame.file->ReadSamples( dest, buf.nextRawY, 1, 0 ) )
      {
         Console().CriticalLn( String().Format(
            "FrameStreamer: ReadRawCfaRow failed for frame %d, row %d",
            frameIndex, buf.nextRawY ) );
         return false;
      }
   }
   catch ( ... )
   {
      Console().CriticalLn( String().Format(
         "FrameStreamer: Exception in ReadRawCfaRow for frame %d, row %d",
         frameIndex, buf.nextRawY ) );
      return false;
   }

   buf.nextRawY++;
   return true;
}

// ----------------------------------------------------------------------------

bool FrameStreamer::ReadRowCFA( int y, int channel,
                                 std::vector<std::vector<float>>& rowData )
{
   // Check cache
   if ( y == m_cachedRow && channel == m_cachedChannel && !m_rowCache.empty() )
   {
      rowData = m_rowCache;
      return true;
   }

   int numFrames = static_cast<int>( m_frames.size() );

   // Resize output
   rowData.resize( numFrames );
   for ( int f = 0; f < numFrames; ++f )
      rowData[f].resize( m_width );

   for ( int f = 0; f < numFrames; ++f )
   {
      CfaRawBuffer& buf = m_cfaBuffers[f];

      // Advance the rolling buffer to cover rows y-1, y, y+1
      // On first call (y=0), we need to read rows 0 and 1
      if ( y == 0 )
      {
         // Need a file reset if we previously read past row 0
         if ( buf.nextRawY > 0 )
         {
            // Reset this frame's file to start from row 0
            if ( m_frames[f].isOpen && m_frames[f].file )
            {
               m_frames[f].file->Close();
               m_frames[f].isOpen = false;
            }
            m_frames[f].file.reset();
            if ( !OpenFrame( m_frames[f] ) )
               return false;
            buf.nextRawY = 0;
         }

         // Read row 0 into slot [1] (current)
         if ( !ReadRawCfaRow( f, buf.rows[1].data() ) )
            return false;

         // Replicate for top edge: prev = current
         std::copy( buf.rows[1].begin(), buf.rows[1].end(), buf.rows[0].begin() );

         // Read row 1 into slot [2] (next) if height > 1
         if ( m_height > 1 )
         {
            if ( !ReadRawCfaRow( f, buf.rows[2].data() ) )
               return false;
         }
         else
         {
            // Single-row image: next = current
            std::copy( buf.rows[1].begin(), buf.rows[1].end(), buf.rows[2].begin() );
         }
      }
      else
      {
         // Rotate buffer: prev = old cur, cur = old next
         std::swap( buf.rows[0], buf.rows[1] );
         std::swap( buf.rows[1], buf.rows[2] );

         // Read the new next row
         if ( y + 1 < m_height )
         {
            if ( !ReadRawCfaRow( f, buf.rows[2].data() ) )
               return false;
         }
         else
         {
            // Bottom edge: replicate current row
            std::copy( buf.rows[1].begin(), buf.rows[1].end(), buf.rows[2].begin() );
         }
      }

      // Demosaic this row for the requested channel
      BayerDemosaic::DemosaicRow(
         buf.rows[0].data(), buf.rows[1].data(), buf.rows[2].data(),
         y, m_width, m_bayerPattern, channel, rowData[f].data() );
   }

   // Update cache
   m_cachedRow = y;
   m_cachedChannel = channel;
   m_rowCache = rowData;

   // Update tracking
   m_lastReadRow = y;
   m_lastReadChannel = channel;

   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
