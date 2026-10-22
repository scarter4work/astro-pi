// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NewImageWindow.h"

#include <pcl/ErrorHandler.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <exception>

namespace pcl
{

IsoString ShowNewImageWindow( int width, int height, int channels, const IsoString& id, const FITSKeywordArray& keywords,
                              const std::function<void( Image& )>& fill, const String& what )
{
   std::exception_ptr failure; // the first error while the window is still hidden
   IsoString viewId;
   try
   {
      ImageWindow window( width, height, channels, 32, true/*float*/, channels == 3, true/*initialProcessing*/, id );
      if ( window.IsNull() )
         throw Error( what + ": PixInsight could not create an image window" );
      try
      {
         View view = window.MainView();
         ImageVariant v = view.Image();
         if ( !v.IsFloatSample() || v.BitsPerSample() != 32 || v.NumberOfChannels() != channels )
            throw Error( what + ": internal error - the new window is not a 32-bit float image" );
         fill( static_cast<Image&>( *v ) );
         window.SetKeywords( keywords );
         viewId = view.Id();
      }
      catch ( ... )
      {
         failure = std::current_exception();
      }
      if ( failure )
      {
         try
         {
            window.ForceClose();
         }
         catch ( ... )
         {
         }
      }
      else
         window.Show();
   } // the window's wrapper goes here, straight after a close
   catch ( ... )
   {
      // Creating or showing the window failed; or, after a failure, releasing
      // the closed window's wrapper did -- then the first error wins.
      if ( !failure )
         throw;
   }
   if ( failure )
      std::rethrow_exception( failure );
   return viewId;
}

} // namespace pcl
