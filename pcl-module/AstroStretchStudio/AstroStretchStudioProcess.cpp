// ----------------------------------------------------------------------------
// AstroStretchStudio Process Implementation
// ----------------------------------------------------------------------------

#include "AstroStretchStudioProcess.h"
#include "AstroStretchStudioInstance.h"
#include "AstroStretchStudioInterface.h"
#include "AstroStretchStudioParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------

AstroStretchStudioProcess* TheAstroStretchStudioProcess = nullptr;

// ----------------------------------------------------------------------------

AstroStretchStudioProcess::AstroStretchStudioProcess()
{
   TheAstroStretchStudioProcess = this;

   // Register parameters
   new ASSAlgorithm( this );
   new ASSOTSObjectType( this );
   new ASSOTSBackgroundTarget( this );
   new ASSOTSStretchIntensity( this );
   new ASSOTSProtectHighlights( this );
   new ASSOTSPreserveColor( this );
   new ASSOTSHDREnabled( this );
   new ASSOTSHDRAmount( this );
   new ASSOTSHDRThreshold( this );
   new ASSOTSStarProtection( this );
   new ASSSASNumScales( this );
   new ASSSASBackgroundTarget( this );
   new ASSSASFineScaleGain( this );
   new ASSSASMidScaleGain( this );
   new ASSSASCoarseScaleGain( this );
   new ASSSASCompressionAlpha( this );
   new ASSSASHighlightProtection( this );
   new ASSSASNoiseThreshold( this );
   new ASSSASFlattenBackground( this );
   new ASSSASPreserveColor( this );
   new ASSZoneHDREnabled( this );
   new ASSZonePreviewMode( this );
   new ASSZoneCount( this );
   new ASSZoneIntensity( this );
   new ASSZoneSaturation( this );
   new ASSZoneSelectedIndex( this );
}

// ----------------------------------------------------------------------------

IsoString AstroStretchStudioProcess::Id() const
{
   return "AstroStretchStudio";
}

// ----------------------------------------------------------------------------

IsoString AstroStretchStudioProcess::Category() const
{
   return "IntensityTransformations";
}

// ----------------------------------------------------------------------------

uint32 AstroStretchStudioProcess::Version() const
{
   return 0x100; // 1.0.0
}

// ----------------------------------------------------------------------------

String AstroStretchStudioProcess::Description() const
{
   return "<html>"
          "<p>AstroStretch Studio provides advanced image stretching algorithms "
          "specifically designed for astrophotography:</p>"
          "<ul>"
          "<li><b>Optimal Transport Stretch (OTS)</b>: Uses optimal transport theory "
          "to find the mathematically optimal mapping between your image histogram "
          "and a target distribution optimized for different object types.</li>"
          "<li><b>Starlet Arctan Stretch (SAS)</b>: Multiscale stretching using "
          "wavelet decomposition with scale-dependent gain control and arctangent "
          "dynamic range compression.</li>"
          "</ul>"
          "</html>";
}

// ----------------------------------------------------------------------------

IsoString AstroStretchStudioProcess::IconImageSVG() const
{
   return
      "<svg width=\"32\" height=\"32\" viewBox=\"0 0 32 32\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<defs>"
      "<linearGradient id=\"grad\" x1=\"0\" y1=\"0\" x2=\"32\" y2=\"32\">"
      "<stop offset=\"0%\" stop-color=\"#818cf8\"/>"
      "<stop offset=\"100%\" stop-color=\"#ec4899\"/>"
      "</linearGradient>"
      "</defs>"
      "<circle cx=\"16\" cy=\"16\" r=\"14\" stroke=\"url(#grad)\" stroke-width=\"2\" fill=\"none\"/>"
      "<circle cx=\"16\" cy=\"16\" r=\"8\" fill=\"url(#grad)\" opacity=\"0.6\"/>"
      "<circle cx=\"16\" cy=\"16\" r=\"3\" fill=\"white\"/>"
      "</svg>";
}

// ----------------------------------------------------------------------------

ProcessInterface* AstroStretchStudioProcess::DefaultInterface() const
{
   return TheAstroStretchStudioInterface;
}

// ----------------------------------------------------------------------------

ProcessImplementation* AstroStretchStudioProcess::Create() const
{
   return new AstroStretchStudioInstance( this );
}

// ----------------------------------------------------------------------------

ProcessImplementation* AstroStretchStudioProcess::Clone( const ProcessImplementation& p ) const
{
   const AstroStretchStudioInstance* instance =
      dynamic_cast<const AstroStretchStudioInstance*>( &p );
   return (instance != nullptr) ? new AstroStretchStudioInstance( *instance ) : nullptr;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioProcess::NeedsValidation() const
{
   return false;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioProcess::CanProcessViews() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioProcess::CanProcessGlobal() const
{
   return false;
}

// ----------------------------------------------------------------------------

} // namespace pcl

// ----------------------------------------------------------------------------
