// EZTools -- EZHazeKill process.
// Copyright and licence: see src/EZToolsLicense.h.

#include "EZHazeKillProcess.h"
#include "EZHazeKillInstance.h"
#include "EZHazeKillInterface.h"
#include "EZHazeKillParameters.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

EZHazeKillProcess* TheEZHazeKillProcess = nullptr;

EZHazeKillProcess::EZHazeKillProcess()
{
   TheEZHazeKillProcess = this;

   new EZHKAmount( this );
   new EZHKNeutralize( this );
   new EZHKTargetLevel( this );
}

IsoString EZHazeKillProcess::Id() const
{
   return "EZHazeKill";
}

IsoString EZHazeKillProcess::Categories() const
{
   return "BackgroundModelization";
}

uint32 EZHazeKillProcess::Version() const
{
   return 0x100;
}

String EZHazeKillProcess::Description() const
{
   return
      "<html>"
      "<p><b>EZHazeKill</b> removes the flat background haze that remains after gradient extraction (GraXpert, "
      "DBE, GradientCorrection): a residual sky pedestal and a colour cast. Run it after the gradient is gone; "
      "it does not model a gradient. It works on linear and on stretched images.</p>"
      "<p>The sky is measured on the target image itself, at full resolution: the darkest 10% of its 64 px "
      "tiles (ranked by luminance) give each channel's sky level and noise. Then, per channel, it subtracts "
      "<i>neutralize</i> times the colour cast (the channel's sky minus the mean sky) and <i>amount</i> times the "
      "pedestal above the target. The target is the larger of <i>targetLevel</i> and 4 sigma of the sky noise, "
      "and a sky already below it is never raised. Dark pixels roll off smoothly (a soft toe starting at 2 sigma) "
      "instead of clipping to zero.</p>"
      "<p>The Real-Time Preview shows exactly what Execute writes: it applies the same correction, measured on "
      "the full image (never on the reduced preview). On a preview, the preview's own pixels are measured. An "
      "image with NaN or infinite samples is refused and left unchanged.</p>"
      "<p>No network, no external programs, no files written.</p>"
      "</html>";
}

ProcessInterface* EZHazeKillProcess::DefaultInterface() const
{
   return TheEZHazeKillInterface;
}

ProcessImplementation* EZHazeKillProcess::Create() const
{
   return new EZHazeKillInstance( this );
}

ProcessImplementation* EZHazeKillProcess::Clone( const ProcessImplementation& p ) const
{
   const EZHazeKillInstance* instance = dynamic_cast<const EZHazeKillInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "EZHazeKill: internal error - cannot clone a non-EZHazeKill instance" );
   return new EZHazeKillInstance( *instance );
}

bool EZHazeKillProcess::CanProcessViews() const
{
   return true;
}

bool EZHazeKillProcess::CanProcessGlobal() const
{
   return false;
}

} // namespace pcl
