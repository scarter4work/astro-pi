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
   new SVBayerPattern( this );
   new SVSaveSerPath( this );
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
      "<p><b>SERViewer</b> opens planetary video files: SER, and uncompressed 8-bit greyscale or raw Bayer AVI "
      "(the format is read from the file's content, not its extension).</p>"
      "<p>Execute it globally to open frame <i>frameIndex</i> of <i>filePath</i> as a new 32-bit "
      "image window, samples normalised to [0,1]. Bayer files open debayered (bilinear) unless "
      "<i>debayer</i> is false, which keeps the raw CFA and records the pattern in BAYERPAT. "
      "For an AVI, <i>bayerPattern</i> sets the CFA pattern; Auto takes it from the ASIAIR "
      "sidecar (<i>video</i>.avi.txt) and, with no sidecar, opens the raw CFA as mono with a warning.</p>"
      "<p>With <i>saveSerPath</i> set, it instead converts the AVI at <i>filePath</i> to a SER file "
      "at that path (never over an existing file) and opens no window.</p>"
      "<p>Reads only the chosen file and its sidecar; no network, no external programs. Nothing is "
      "written to disk unless <i>saveSerPath</i> is set.</p>"
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
