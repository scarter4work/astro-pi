//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXProcess.h"
#include "NukeXParameters.h"
#include "NukeXInstance.h"
#include "NukeXInterface.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXProcess* TheNukeXProcess = nullptr;

// ----------------------------------------------------------------------------

NukeXProcess::NukeXProcess()
{
   TheNukeXProcess = this;

   // Instantiate all process parameters
   new NXProcessingMode( this );
   new NXPreviewMode( this );
   new NXStretchAlgorithm( this );
   new NXAutoSegment( this );
   new NXAutoSelect( this );
   new NXEnableLRGB( this );
   new NXEnableToneMapping( this );
   new NXAutoBlackPoint( this );
   new NXGlobalContrast( this );
   new NXSaturationBoost( this );
   new NXBlendRadius( this );
   new NXStretchStrength( this );
   new NXBlackPoint( this );
   new NXWhitePoint( this );
   new NXGamma( this );

   // Region enable parameters
   new NXEnableStarCores( this );
   new NXEnableStarHalos( this );
   new NXEnableNebulaBright( this );
   new NXEnableNebulaFaint( this );
   new NXEnableDustLanes( this );
   new NXEnableGalaxyCore( this );
   new NXEnableGalaxyHalo( this );
   new NXEnableGalaxyArms( this );
   new NXEnableBackground( this );
}

// ----------------------------------------------------------------------------

IsoString NukeXProcess::Id() const
{
   return "NukeX";
}

// ----------------------------------------------------------------------------

IsoString NukeXProcess::Category() const
{
   return "IntensityTransformations";
}

// ----------------------------------------------------------------------------

uint32 NukeXProcess::Version() const
{
   return 0x100; // Version 1.0.0
}

// ----------------------------------------------------------------------------

String NukeXProcess::Description() const
{
   return
      "<html>"
      "<p><b>NukeX</b> - Intelligent Region-Aware Stretch</p>"
      "<p>NukeX revolutionizes image stretching by using AI-driven semantic "
      "segmentation to identify distinct regions in astrophotography images "
      "and apply optimally-selected stretch algorithms to each region "
      "independently.</p>"
      "<p>Unlike traditional global stretches, NukeX understands that star "
      "cores, faint nebulosity, dust lanes, and galaxy halos each require "
      "different treatment.</p>"
      "<p><i>\"Will blow your socks off!\"</i></p>"
      "</html>";
}

// ----------------------------------------------------------------------------

String NukeXProcess::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

ProcessInterface* NukeXProcess::DefaultInterface() const
{
   return TheNukeXInterface;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXProcess::Create() const
{
   return new NukeXInstance( this );
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXProcess::Clone( const ProcessImplementation& p ) const
{
   const NukeXInstance* instance = dynamic_cast<const NukeXInstance*>( &p );
   return ( instance != nullptr ) ? new NukeXInstance( *instance ) : nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXProcess::CanProcessViews() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXProcess::CanProcessGlobal() const
{
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXProcess::IsAssignable() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXProcess::NeedsInitialization() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXProcess::NeedsValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXProcess::PrefersGlobalExecution() const
{
   return false;
}

// ----------------------------------------------------------------------------

} // namespace pcl
