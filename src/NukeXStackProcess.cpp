//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackProcess.h"
#include "NukeXStackParameters.h"
#include "NukeXStackInstance.h"
#include "NukeXStackInterface.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackProcess* TheNukeXStackProcess = nullptr;

// ----------------------------------------------------------------------------

NukeXStackProcess::NukeXStackProcess()
{
   TheNukeXStackProcess = this;

   // Input frames table (must be created first, then its columns)
   NXSInputFrames* framesTable = new NXSInputFrames( this );
   new NXSInputFramePath( framesTable );
   new NXSInputFrameEnabled( framesTable );

   // Selection strategy
   new NXSSelectionStrategy( this );

   // Boolean parameters
   new NXSEnableMLSegmentation( this );
   new NXSEnableTransitionSmoothing( this );
   new NXSUseSpatialContext( this );
   new NXSUseTargetContext( this );
   new NXSGenerateMetadata( this );
   new NXSEnableAutoStretch( this );
   new NXSEnableRegistration( this );

   // Floating point parameters
   new NXSOutlierSigmaThreshold( this );
   new NXSSmoothingStrength( this );
   new NXSTransitionThreshold( this );

   // Integer parameters
   new NXSTileSize( this );
   new NXSSmoothingRadius( this );
}

// ----------------------------------------------------------------------------

IsoString NukeXStackProcess::Id() const
{
   return "NukeXStack";
}

// ----------------------------------------------------------------------------

IsoString NukeXStackProcess::Category() const
{
   return "ImageIntegration";
}

// ----------------------------------------------------------------------------

uint32 NukeXStackProcess::Version() const
{
   return 0x100; // Version 1.0.0
}

// ----------------------------------------------------------------------------

String NukeXStackProcess::Description() const
{
   return
      "<html>"
      "<p><b>NukeXStack</b> - Intelligent Pixel Selection for Subframe Integration</p>"
      "<p>NukeXStack revolutionizes image integration by using ML-based semantic "
      "segmentation and statistical analysis to <b>select</b> the optimal pixel "
      "value from a stack of prestretched subframes, rather than averaging.</p>"
      "<p><b>Key Features:</b></p>"
      "<ul>"
      "<li><b>Per-pixel distribution fitting</b> - Analyzes pixel values across "
      "all frames to detect outliers and determine optimal selection</li>"
      "<li><b>7-class ML segmentation</b> - Understands what each pixel represents "
      "(star, nebula, background, etc.) to apply class-specific selection strategies</li>"
      "<li><b>FITS metadata context</b> - Uses target object information to inform "
      "selection (e.g., M42 expects emission nebula)</li>"
      "<li><b>Transition smoothing</b> - Post-integration detection and correction "
      "of hard boundaries between regions</li>"
      "</ul>"
      "<p><b>Workflow:</b></p>"
      "<ol>"
      "<li>Stretch each subframe individually (with NukeX or other tools)</li>"
      "<li>Run NukeXStack to intelligently select best pixels</li>"
      "<li>Automatic transition smoothing produces seamless result</li>"
      "</ol>"
      "<p><i>\"Not just averaging - true pixel selection intelligence!\"</i></p>"
      "</html>";
}

// ----------------------------------------------------------------------------

String NukeXStackProcess::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

ProcessInterface* NukeXStackProcess::DefaultInterface() const
{
   return TheNukeXStackInterface;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStackProcess::Create() const
{
   return new NukeXStackInstance( this );
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStackProcess::Clone( const ProcessImplementation& p ) const
{
   const NukeXStackInstance* instance = dynamic_cast<const NukeXStackInstance*>( &p );
   return ( instance != nullptr ) ? new NukeXStackInstance( *instance ) : nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXStackProcess::CanProcessViews() const
{
   return false; // Global execution only (file-based input)
}

// ----------------------------------------------------------------------------

bool NukeXStackProcess::CanProcessGlobal() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackProcess::IsAssignable() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackProcess::NeedsInitialization() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackProcess::NeedsValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackProcess::PrefersGlobalExecution() const
{
   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
