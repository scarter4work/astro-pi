// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SERViewerProcess.h"
#include "SERViewerInstance.h"
#include "SERViewerInterface.h"
#include "SERViewerParameters.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

SERViewerProcess* TheSERViewerProcess = nullptr;

SERViewerProcess::SERViewerProcess()
{
   TheSERViewerProcess = this;

   new SVFilePath( this );
   new SVFrameIndex( this );
   new SVDebayer( this );
}

IsoString SERViewerProcess::Id() const
{
   return "SERViewer";
}

IsoString SERViewerProcess::Categories() const
{
   return "Planetary";
}

uint32 SERViewerProcess::Version() const
{
   return 0x100;
}

String SERViewerProcess::Description() const
{
   return
      "<html>"
      "<p><b>SERViewer</b> opens SER video files (the format planetary capture programs write).</p>"
      "<p>Execute it globally to open frame <i>frameIndex</i> of <i>filePath</i> as a new 32-bit "
      "image window, samples normalised to [0,1]. Bayer files open debayered (bilinear) unless "
      "<i>debayer</i> is false, which keeps the raw CFA and records the pattern in BAYERPAT.</p>"
      "<p>Reads only the chosen file; no network, no external programs, nothing written to disk.</p>"
      "</html>";
}

ProcessInterface* SERViewerProcess::DefaultInterface() const
{
   return TheSERViewerInterface;
}

ProcessImplementation* SERViewerProcess::Create() const
{
   return new SERViewerInstance( this );
}

ProcessImplementation* SERViewerProcess::Clone( const ProcessImplementation& p ) const
{
   const SERViewerInstance* instance = dynamic_cast<const SERViewerInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "SERViewer: internal error - cannot clone a non-SERViewer instance" );
   return new SERViewerInstance( *instance );
}

bool SERViewerProcess::CanProcessViews() const
{
   return false;
}

bool SERViewerProcess::CanProcessGlobal() const
{
   return true;
}

} // namespace pcl
