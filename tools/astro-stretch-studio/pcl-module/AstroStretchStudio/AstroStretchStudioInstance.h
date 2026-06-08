// ----------------------------------------------------------------------------
// AstroStretchStudio Instance Header
// ----------------------------------------------------------------------------

#ifndef __AstroStretchStudioInstance_h
#define __AstroStretchStudioInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

#include "AstroStretchStudioParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class AstroStretchStudioInstance : public ProcessImplementation
{
public:

   AstroStretchStudioInstance( const MetaProcess* );
   AstroStretchStudioInstance( const AstroStretchStudioInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool IsHistoryUpdater( const View& ) const override;
   UndoFlags UndoMode( const View& ) const override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter* p, size_type tableRow ) const override;

   // Default initialization
   void SetDefaultParameters();

   // Real-time preview
   void Preview( UInt16Image& image ) const;

   // Algorithm selection
   pcl_enum p_algorithm;

   // OTS Parameters
   pcl_enum p_otsObjectType;
   double   p_otsBackgroundTarget;
   double   p_otsStretchIntensity;
   double   p_otsProtectHighlights;
   pcl_bool p_otsPreserveColor;

   // OTS HDR Parameters
   pcl_bool p_otsHDREnabled;
   double   p_otsHDRAmount;
   double   p_otsHDRThreshold;
   double   p_otsStarProtection;

   // SAS Parameters
   int32    p_sasNumScales;
   double   p_sasBackgroundTarget;
   double   p_sasFineScaleGain;
   double   p_sasMidScaleGain;
   double   p_sasCoarseScaleGain;
   double   p_sasCompressionAlpha;
   double   p_sasHighlightProtection;
   double   p_sasNoiseThreshold;
   pcl_bool p_sasFlattenBackground;
   pcl_bool p_sasPreserveColor;

   // Zone HDR Parameters
   pcl_bool p_zoneHDREnabled;
   pcl_enum p_zonePreviewMode;
   int32    p_zoneCount;
   double   p_zoneIntensity[8];    // Per-zone intensity adjustment (-1 to +1)
   double   p_zoneSaturation[8];   // Per-zone saturation adjustment (-1 to +1)
   int32    p_zoneSelectedIndex;   // For solo preview mode

private:

   // Internal processing methods
   void ApplyOTS( Image& image ) const;
   void ApplySAS( Image& image ) const;
   void ApplyHDR( Image& L ) const;
   void ApplyZoneHDR( Image& image ) const;

   // OTS helpers
   void GenerateTargetCDF( FVector& cdf, int objectType, double bgTarget ) const;
   void ComputeHistogramCDF( const Image& image, FVector& cdf ) const;
   void ComputeTransportMap( FVector& tmap, const FVector& srcCDF, const FVector& tgtCDF ) const;

   // SAS helpers
   void StarletDecompose( const Image& image, Array<Image>& scales, int numScales ) const;
   void StarletReconstruct( Image& output, const Array<Image>& scales ) const;
   double EstimateNoise( const Image& fineScale ) const;
   double ComputeScaleGain( int scale ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __AstroStretchStudioInstance_h

// ----------------------------------------------------------------------------
