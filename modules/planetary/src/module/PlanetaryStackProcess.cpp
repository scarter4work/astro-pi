// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryStackProcess.h"
#include "PlanetaryStackInstance.h"
#include "PlanetaryStackInterface.h"
#include "PlanetaryStackParameters.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

PlanetaryStackProcess* ThePlanetaryStackProcess = nullptr;

PlanetaryStackProcess::PlanetaryStackProcess()
{
   ThePlanetaryStackProcess = this;

   new PSFilePath( this );
   new PSBayerPattern( this );
}

IsoString PlanetaryStackProcess::Id() const
{
   return "PlanetaryStack";
}

IsoString PlanetaryStackProcess::Categories() const
{
   return "Planetary";
}

uint32 PlanetaryStackProcess::Version() const
{
   return 0x100;
}

String PlanetaryStackProcess::Description() const
{
   return
      "<html>"
      "<p><b>PlanetaryStack</b> stacks a planetary or lunar video (SER, or uncompressed 8-bit greyscale or raw "
      "Bayer AVI) into one image, automatically: every frame is measured, the sharpest are selected from the "
      "capture's own statistics, aligned to the target and region by region (lucky imaging), and stacked with "
      "outlier rejection. There is no percentage to choose and no alignment point to place.</p>"
      "<p>Execute it globally to stack <i>filePath</i> and open the stack as a new 32-bit float image window, "
      "linear: RGB for a Bayer or colour capture, mono for a mono one. For an AVI, <i>bayerPattern</i> sets the "
      "CFA pattern; Auto takes it from the ASIAIR sidecar (<i>video</i>.avi.txt). An AVI with no known pattern is "
      "stacked as a mono image of the raw colour mosaic, with a warning: it cannot be debayered afterwards, so "
      "choose the pattern (or save it with Save pattern... in the PlanetaryStack or SERViewer window) and stack "
      "again for colour.</p>"
      "<p>When the capture has no usable frames (no target, the target leaving the frame, a saturated target, "
      "out of focus, poor seeing, frames that do not line up) no image is made: execution fails with the "
      "diagnosis and its numbers on the Process Console.</p>"
      "<p>Reads only the chosen file and its sidecar; no network, no external programs; writes nothing to "
      "disk.</p>"
      "</html>";
}

ProcessInterface* PlanetaryStackProcess::DefaultInterface() const
{
   return ThePlanetaryStackInterface;
}

ProcessImplementation* PlanetaryStackProcess::Create() const
{
   return new PlanetaryStackInstance( this );
}

ProcessImplementation* PlanetaryStackProcess::Clone( const ProcessImplementation& p ) const
{
   const PlanetaryStackInstance* instance = dynamic_cast<const PlanetaryStackInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "PlanetaryStack: internal error - cannot clone a non-PlanetaryStack instance" );
   return new PlanetaryStackInstance( *instance );
}

bool PlanetaryStackProcess::CanProcessViews() const
{
   return false;
}

bool PlanetaryStackProcess::CanProcessGlobal() const
{
   return true;
}

} // namespace pcl
