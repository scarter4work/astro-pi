// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessActivity.h"

#include <pcl/Console.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <chrono>

namespace pcl
{

namespace
{

using clock_type = std::chrono::steady_clock;

// Root thread only (notification handlers, ExecuteGlobal, timer ticks).
clock_type::time_point& LastActivity()
{
   static clock_type::time_point t = clock_type::time_point::min();
   return t;
}

} // namespace

void NoteProcessActivity()
{
   LastActivity() = clock_type::now();
}

ProcessActivityState CurrentProcessActivity()
{
   ProcessActivityState s;
   try
   {
      if ( Console().AbortEnabled() )
      {
         s.busy = true;
         s.reason = "a process or script is running";
         return s;
      }
      for ( const ImageWindow& w : ImageWindow::AllWindows( true/*includeIconicWindows*/ ) )
      {
         Array<View> views = w.Previews();
         views.Add( w.MainView() );
         for ( const View& v : views )
            if ( !v.CanRead() || !v.CanWrite() )
            {
               s.busy = true;
               s.reason = "image " + String( v.FullId() ) + " is locked by a running process";
               return s;
            }
      }
      const clock_type::time_point last = LastActivity();
      if ( last != clock_type::time_point::min() )
      {
         const double idle = std::chrono::duration<double>( clock_type::now() - last ).count();
         if ( idle < PICopilotApplyQuietSeconds )
         {
            s.busy = true;
            s.reason = "an image was just being processed";
            return s;
         }
      }
   }
   catch ( ... )
   {
      s.busy = true;
      s.reason = "PixInsight's state could not be read";
   }
   return s;
}

} // namespace pcl
