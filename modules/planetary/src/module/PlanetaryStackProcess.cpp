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
   new PSTarget( this );
   new PSMineralMoon( this );
   new PSMineralStrength( this );
   new PSChromaBlur( this );
   new PSMineralSharpen( this );
   new PSPlanetProcess( this );
   new PSPlanetSaturation( this );
   new PSPlanetChromaBlur( this );
   new PSPlanetSharpen( this );
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
      "<p><i>target</i> says what the capture is of. Auto reads it from an ASIAIR capture name "
      "(<i>date</i>-<i>time</i>-<i>Target</i>-Bin1 <i>Filter</i> <i>temp</i>F); any other value is your choice. It "
      "names the stack's OBJECT keyword. With <i>mineralMoon</i> on, a Moon capture also gives a mineral moon, "
      "opened as a second window (<i>name</i>_mineral): the stack colour-calibrated, stretched for display and "
      "sharpened, with the Moon's faint colour differences amplified by <i>mineralStrength</i> (0 to 15, default 8) "
      "and their noise smoothed by <i>chromaBlur</i> (px, 0.5 to 6, default 2.5); <i>mineralSharpen</i> (0 to 4, "
      "default 1.5) sets how much its luminance is sharpened (0 none). It needs the Moon, a colour capture "
      "not through a narrowband or IR-pass filter (as the file name names it; an unrecognised or missing filter "
      "name is allowed): anything else fails before stacking, saying why. The stack window stays linear; the "
      "mineral image is display-ready, every sample within [0, 1].</p>"
      "<p>With <i>planetProcess</i> on, a planet capture (Mercury to Neptune) also gives its planet processing, opened "
      "as a second window (<i>name</i>_planet): the stack colour-calibrated to the planet's own catalogue colour "
      "(Mallama, Krobusek &amp; Pavlov 2017), its red and blue aligned to green against atmospheric dispersion, shown "
      "linear and sharpened, with its natural colour scaled by <i>planetSaturation</i> (0 grey, 1 natural), its "
      "colour noise smoothed by <i>planetChromaBlur</i> (px) and its luminance sharpened by <i>planetSharpen</i> (0 "
      "none). It needs a planet target (Other is refused: pick the planet), a colour capture, and a broadband "
      "filter; anything else fails before stacking, saying why. Its image is display-ready, every sample within "
      "[0, 1].</p>"
      "<p>When the capture has no usable frames (no target, the target leaving the frame, a saturated target, "
      "out of focus, poor seeing, frames that do not line up) no image is made: execution fails with the "
      "diagnosis and its numbers on the Process Console.</p>"
      "<p>Reads only the chosen file and its sidecar; no network, no external programs. Stacking writes "
      "nothing to disk; Save pattern... in its window writes the AVI's .txt sidecar.</p>"
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
