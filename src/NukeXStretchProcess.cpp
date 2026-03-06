//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStretchProcess.h"
#include "NukeXStretchInstance.h"
#include "NukeXStretchInterface.h"
#include "NukeXStretchParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStretchProcess* TheNukeXStretchProcess = nullptr;

// ----------------------------------------------------------------------------

NukeXStretchProcess::NukeXStretchProcess()
{
   TheNukeXStretchProcess = this;

   // Register parameters
   new NXSStretchAlgorithm( this );
   new NXSAutoBlackPoint( this );
   new NXSContrast( this );
   new NXSSaturation( this );
   new NXSBlackPoint( this );
   new NXSWhitePoint( this );
   new NXSGamma( this );
   new NXSStretchStrength( this );
}

// ----------------------------------------------------------------------------

IsoString NukeXStretchProcess::Id() const
{
   return "NukeXStretch";
}

// ----------------------------------------------------------------------------

IsoString NukeXStretchProcess::Category() const
{
   return "IntensityTransformations";
}

// ----------------------------------------------------------------------------

uint32 NukeXStretchProcess::Version() const
{
   return 0x100; // Version 1.0.0
}

// ----------------------------------------------------------------------------

String NukeXStretchProcess::Description() const
{
   return
      "<html>"
      "<p><b>NukeXStretch</b> - Intelligent Region-Aware Stretch</p>"
      "<p>NukeXStretch uses AI-driven semantic segmentation to identify distinct "
      "regions in astrophotography images and apply optimally-selected stretch "
      "algorithms to each region independently.</p>"
      "<p>Unlike traditional global stretches, NukeXStretch understands that star "
      "cores, faint nebulosity, dust lanes, and galaxy halos each require "
      "different treatment.</p>"
      "</html>";
}

// ----------------------------------------------------------------------------

String NukeXStretchProcess::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

ProcessInterface* NukeXStretchProcess::DefaultInterface() const
{
   return TheNukeXStretchInterface;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStretchProcess::Create() const
{
   return new NukeXStretchInstance( this );
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStretchProcess::Clone( const ProcessImplementation& p ) const
{
   const NukeXStretchInstance* instance = dynamic_cast<const NukeXStretchInstance*>( &p );
   return ( instance != nullptr ) ? new NukeXStretchInstance( *instance ) : nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXStretchProcess::CanProcessViews() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchProcess::CanProcessGlobal() const
{
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXStretchProcess::IsAssignable() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchProcess::NeedsInitialization() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchProcess::NeedsValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchProcess::PrefersGlobalExecution() const
{
   return false;
}

// ----------------------------------------------------------------------------

} // namespace pcl
