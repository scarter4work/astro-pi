// ----------------------------------------------------------------------------
// AstroStretchStudio Parameters Header
// ----------------------------------------------------------------------------

#ifndef __AstroStretchStudioParameters_h
#define __AstroStretchStudioParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------

PCL_BEGIN_LOCAL

// ----------------------------------------------------------------------------
// Algorithm Selection
// ----------------------------------------------------------------------------

class ASSAlgorithm : public MetaEnumeration
{
public:
   enum { OTS,    // Optimal Transport Stretch
          SAS,    // Starlet Arctan Stretch
          NumberOfItems,
          Default = OTS };

   ASSAlgorithm( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern ASSAlgorithm* TheASSAlgorithmParameter;

// ----------------------------------------------------------------------------
// OTS Parameters
// ----------------------------------------------------------------------------

class ASSOTSObjectType : public MetaEnumeration
{
public:
   enum { Nebula,
          Galaxy,
          StarCluster,
          DarkNebula,
          Custom,
          NumberOfItems,
          Default = Nebula };

   ASSOTSObjectType( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern ASSOTSObjectType* TheASSOTSObjectTypeParameter;

// ----------------------------------------------------------------------------

class ASSOTSBackgroundTarget : public MetaFloat
{
public:
   ASSOTSBackgroundTarget( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSOTSBackgroundTarget* TheASSOTSBackgroundTargetParameter;

// ----------------------------------------------------------------------------

class ASSOTSStretchIntensity : public MetaFloat
{
public:
   ASSOTSStretchIntensity( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSOTSStretchIntensity* TheASSOTSStretchIntensityParameter;

// ----------------------------------------------------------------------------

class ASSOTSProtectHighlights : public MetaFloat
{
public:
   ASSOTSProtectHighlights( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSOTSProtectHighlights* TheASSOTSProtectHighlightsParameter;

// ----------------------------------------------------------------------------

class ASSOTSPreserveColor : public MetaBoolean
{
public:
   ASSOTSPreserveColor( MetaProcess* );

   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern ASSOTSPreserveColor* TheASSOTSPreserveColorParameter;

// ----------------------------------------------------------------------------
// OTS HDR Parameters
// ----------------------------------------------------------------------------

class ASSOTSHDREnabled : public MetaBoolean
{
public:
   ASSOTSHDREnabled( MetaProcess* );

   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern ASSOTSHDREnabled* TheASSOTSHDREnabledParameter;

// ----------------------------------------------------------------------------

class ASSOTSHDRAmount : public MetaFloat
{
public:
   ASSOTSHDRAmount( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSOTSHDRAmount* TheASSOTSHDRAmountParameter;

// ----------------------------------------------------------------------------

class ASSOTSHDRThreshold : public MetaFloat
{
public:
   ASSOTSHDRThreshold( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSOTSHDRThreshold* TheASSOTSHDRThresholdParameter;

// ----------------------------------------------------------------------------

class ASSOTSStarProtection : public MetaFloat
{
public:
   ASSOTSStarProtection( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSOTSStarProtection* TheASSOTSStarProtectionParameter;

// ----------------------------------------------------------------------------
// SAS Parameters
// ----------------------------------------------------------------------------

class ASSSASNumScales : public MetaInt32
{
public:
   ASSSASNumScales( MetaProcess* );

   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASNumScales* TheASSSASNumScalesParameter;

// ----------------------------------------------------------------------------

class ASSSASBackgroundTarget : public MetaFloat
{
public:
   ASSSASBackgroundTarget( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASBackgroundTarget* TheASSSASBackgroundTargetParameter;

// ----------------------------------------------------------------------------

class ASSSASFineScaleGain : public MetaFloat
{
public:
   ASSSASFineScaleGain( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASFineScaleGain* TheASSSASFineScaleGainParameter;

// ----------------------------------------------------------------------------

class ASSSASMidScaleGain : public MetaFloat
{
public:
   ASSSASMidScaleGain( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASMidScaleGain* TheASSSASMidScaleGainParameter;

// ----------------------------------------------------------------------------

class ASSSASCoarseScaleGain : public MetaFloat
{
public:
   ASSSASCoarseScaleGain( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASCoarseScaleGain* TheASSSASCoarseScaleGainParameter;

// ----------------------------------------------------------------------------

class ASSSASCompressionAlpha : public MetaFloat
{
public:
   ASSSASCompressionAlpha( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASCompressionAlpha* TheASSSASCompressionAlphaParameter;

// ----------------------------------------------------------------------------

class ASSSASHighlightProtection : public MetaFloat
{
public:
   ASSSASHighlightProtection( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASHighlightProtection* TheASSSASHighlightProtectionParameter;

// ----------------------------------------------------------------------------

class ASSSASNoiseThreshold : public MetaFloat
{
public:
   ASSSASNoiseThreshold( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSSASNoiseThreshold* TheASSSASNoiseThresholdParameter;

// ----------------------------------------------------------------------------

class ASSSASFlattenBackground : public MetaBoolean
{
public:
   ASSSASFlattenBackground( MetaProcess* );

   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern ASSSASFlattenBackground* TheASSSASFlattenBackgroundParameter;

// ----------------------------------------------------------------------------

class ASSSASPreserveColor : public MetaBoolean
{
public:
   ASSSASPreserveColor( MetaProcess* );

   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern ASSSASPreserveColor* TheASSSASPreserveColorParameter;

// ----------------------------------------------------------------------------
// Zone HDR Parameters
// ----------------------------------------------------------------------------

class ASSZoneHDREnabled : public MetaBoolean
{
public:
   ASSZoneHDREnabled( MetaProcess* );

   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern ASSZoneHDREnabled* TheASSZoneHDREnabledParameter;

// ----------------------------------------------------------------------------

class ASSZonePreviewMode : public MetaEnumeration
{
public:
   enum { Off,
          MaskOverlay,
          SoloZone,
          NumberOfItems,
          Default = Off };

   ASSZonePreviewMode( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern ASSZonePreviewMode* TheASSZonePreviewModeParameter;

// ----------------------------------------------------------------------------

class ASSZoneCount : public MetaInt32
{
public:
   ASSZoneCount( MetaProcess* );

   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSZoneCount* TheASSZoneCountParameter;

// ----------------------------------------------------------------------------

class ASSZoneIntensity : public MetaFloat
{
public:
   ASSZoneIntensity( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSZoneIntensity* TheASSZoneIntensityParameter;

// ----------------------------------------------------------------------------

class ASSZoneSaturation : public MetaFloat
{
public:
   ASSZoneSaturation( MetaProcess* );

   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSZoneSaturation* TheASSZoneSaturationParameter;

// ----------------------------------------------------------------------------

class ASSZoneSelectedIndex : public MetaInt32
{
public:
   ASSZoneSelectedIndex( MetaProcess* );

   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern ASSZoneSelectedIndex* TheASSZoneSelectedIndexParameter;

// ----------------------------------------------------------------------------

PCL_END_LOCAL

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __AstroStretchStudioParameters_h

// ----------------------------------------------------------------------------
