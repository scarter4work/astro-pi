// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ViewCapture.h"
#include "ViewContext.h"
#include "ViewPreview.h"
#include "VisionTurn.h"

#include <pcl/Exception.h>

#include <exception>

namespace pcl
{

const char* const kPICopilotNoActiveImageNote = "(no active image \xE2\x80\x94 sending text only)";

AnthropicMessage CaptureViewTurn( const String& prompt, const View* view, StringList& notes )
{
   if ( view == nullptr )
   {
      notes.Add( String::UTF8ToUTF16( kPICopilotNoActiveImageNote ) );
      return ComposeUserTurn( prompt, nullptr, IsoString() );
   }

   nlohmann::json ctx;
   bool haveCtx = false;
   try
   {
      ctx = BuildViewContext( *view );
      haveCtx = true;
   }
   catch ( const pcl::Exception& x )
   {
      notes.Add( "View context failed: " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      notes.Add( String( "View context failed: " ) + String( x.what() ) );
   }
   catch ( ... )
   {
      notes.Add( String( "View context failed: unknown exception" ) );
   }

   // RenderViewPreview() never throws (ok=false + error instead).
   const ViewPreviewResult p = RenderViewPreview( *view );
   if ( p.ok )
   {
      String id;
      try
      {
         id = view->FullId();
      }
      catch ( ... )
      {
         id = "(unnamed view)";
      }
      notes.Add( "(attached " + id + ": auto-stretched preview "
                 + String( p.width ) + "x" + String( p.height ) + ", "
                 + String( int( p.jpegBytes/1024 ) ) + " KiB)" );
   }
   else
      notes.Add( "Preview failed: " + p.error
                 + String::UTF8ToUTF16( " \xE2\x80\x94 sending without an image" ) );

   return ComposeUserTurn( prompt, haveCtx ? &ctx : nullptr, p.ok ? p.base64 : IsoString() );
}

} // namespace pcl
