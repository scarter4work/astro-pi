//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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

   // Boolean parameters
   new NXSGenerateProvenance( this );
   new NXSGenerateDistMetadata( this );
   new NXSEnableMetadataTiebreaker( this );
   new NXSEnableAutoStretch( this );
   new NXSUseGPU( this );
   new NXSAdaptiveModels( this );
   new NXSEnableRemediation( this );
   new NXSEnableTrailRemediation( this );
   new NXSEnableDustRemediation( this );
   new NXSEnableVignettingRemediation( this );

   // Floating point parameters
   new NXSOutlierSigmaThreshold( this );
   new NXSTrailDilateRadius( this );
   new NXSTrailOutlierSigma( this );
   new NXSDustCircularityMin( this );
   new NXSDustDetectionSigma( this );
   new NXSDustMaxCorrectionRatio( this );
   new NXSVignettingMaxCorrection( this );

   // Integer parameters
   new NXSDustMinDiameter( this );
   new NXSDustMaxDiameter( this );
   new NXSDustNeighborRadius( this );
   new NXSVignettingPolyOrder( this );
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
      "<p><b>NukeXStack</b> — Per-Pixel Statistical Inference Stacking</p>"
      "<p>NukeXStack performs image integration by fitting statistical distributions "
      "to pixel stacks and using maximum-likelihood estimation to select optimal "
      "pixel values.</p>"
      "<p><b>Key Features:</b></p>"
      "<ul>"
      "<li><b>Distribution fitting</b> — MLE-based fitting of Gaussian, Poisson, "
      "Skew-Normal, and Bimodal mixture models to each pixel stack</li>"
      "<li><b>AIC model selection</b> — Akaike Information Criterion selects the "
      "best-fit distribution per pixel</li>"
      "<li><b>Outlier detection</b> — Generalized ESD test identifies outlier "
      "frames per pixel for robust pixel selection</li>"
      "<li><b>Metadata tiebreaker</b> — When multiple frames are statistically "
      "indistinguishable, prefer the one with better seeing and tracking</li>"
      "<li><b>Frame alignment</b> — Triangle asterism matching with integer "
      "pixel shifts and autocrop to common overlap</li>"
      "</ul>"
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
