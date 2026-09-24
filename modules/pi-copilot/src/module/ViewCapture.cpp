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
const char* const kPICopilotViewBusyNote =
   "(view is busy \xE2\x80\x94 locked by a running process; sending text only)";

AnthropicMessage CaptureViewTurn( const String& prompt, const View* view, StringList& notes )
{
   if ( view == nullptr )
   {
      notes.Add( String::UTF8ToUTF16( kPICopilotNoActiveImageNote ) );
      return ComposeUserTurn( prompt, nullptr, IsoString() );
   }

   // A process or script that is running on this view holds its lock, and
   // processes run on the root thread while pumping events, so Send is
   // clickable mid-process. Both BuildViewContext() and RenderViewPreview()
   // take an AutoViewWriteLock (View::LockForWrite), which must never be
   // attempted on a locked view from the GUI thread: it hangs PixInsight
   // (measured: the headless self-test never returned). So probe first with
   // the non-waiting queries View::CanRead() / View::CanWrite() (View.h:317-325,
   // "not locked for reading/writing"; both are API->View->GetViewLocks,
   // View.cpp:136-150) and skip the whole capture if either lock is held.
   // Everything here runs on the root thread, so no other root-thread code
   // can lock the view between this check and our own lock.
   if ( !view->IsNull() )
   {
      bool busy = true;
      try
      {
         busy = !view->CanRead() || !view->CanWrite();
      }
      catch ( ... )
      {
      }
      if ( busy )
      {
         notes.Add( String::UTF8ToUTF16( kPICopilotViewBusyNote ) );
         return ComposeUserTurn( prompt, nullptr, IsoString() );
      }
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
