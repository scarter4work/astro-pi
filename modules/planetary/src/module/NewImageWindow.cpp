// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NewImageWindow.h"

#include <pcl/ErrorHandler.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <cstring>
#include <exception>

namespace pcl
{

IsoString ShowNewImageWindow( const Image& image, const IsoString& id, const FITSKeywordArray& keywords, const String& what )
{
   const int width = image.Width(), height = image.Height(), channels = image.NumberOfChannels();
   if ( channels != 1 && channels != 3 )
      throw Error( what + ": internal error - an image of " + String( channels ) + " channels" );

   // On the heap, and never deleted after a ForceClose (see the header).
   ImageWindow* window = new ImageWindow( width, height, channels, 32, true/*float*/, channels == 3,
                                          true/*initialProcessing*/, id );
   if ( window->IsNull() )
   {
      delete window; // a null handle: its destructor never calls the core
      throw Error( what + ": PixInsight could not create an image window" );
   }

   std::exception_ptr failure;
   IsoString viewId;
   try
   {
      View view = window->MainView();
      ImageVariant v = view.Image();
      if ( !v.IsFloatSample() || v.BitsPerSample() != 32 || v.NumberOfChannels() != channels ||
           v.Width() != width || v.Height() != height )
         throw Error( what + ": internal error - the new window is not a 32-bit float image of the expected size" );
      Image& target = static_cast<Image&>( *v );
      const size_t bytes = size_t( width ) * size_t( height ) * sizeof( float );
      for ( int c = 0; c < channels; ++c )
         std::memcpy( target.PixelData( c ), image.PixelData( c ), bytes );
      window->SetKeywords( keywords );
      viewId = view.Id();
   } // every View and ImageVariant of the window is gone here, before any close
   catch ( ... )
   {
      failure = std::current_exception();
   }

   if ( failure )
   {
      try
      {
         window->ForceClose();
      }
      catch ( ... )
      {
      }
      // `window` is not deleted: its handle now names a destroyed object.
      std::rethrow_exception( failure );
   }

   try
   {
      window->Show();
   }
   catch ( ... )
   {
      delete window; // the window is alive (hidden): releasing the wrapper is safe
      throw;
   }
   delete window; // the window is alive and shown; the core owns it
   return viewId;
}

} // namespace pcl
