//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Processing Mode
// ----------------------------------------------------------------------------

NXProcessingMode* TheNXProcessingModeParameter = nullptr;

NXProcessingMode::NXProcessingMode( MetaProcess* P ) : MetaEnumeration( P )
{
   TheNXProcessingModeParameter = this;
}

IsoString NXProcessingMode::Id() const
{
   return "processingMode";
}

size_type NXProcessingMode::NumberOfElements() const
{
   return NumberOfItems;
}

IsoString NXProcessingMode::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case Manual:          return "ProcessingMode_Manual";
   case FullyAutomatic:  return "ProcessingMode_FullyAutomatic";
   }
}

int NXProcessingMode::ElementValue( size_type i ) const
{
   return int( i );
}

size_type NXProcessingMode::DefaultValueIndex() const
{
   return Default;
}

// ----------------------------------------------------------------------------
// Preview Mode
// ----------------------------------------------------------------------------

NXPreviewMode* TheNXPreviewModeParameter = nullptr;

NXPreviewMode::NXPreviewMode( MetaProcess* P ) : MetaEnumeration( P )
{
   TheNXPreviewModeParameter = this;
}

IsoString NXPreviewMode::Id() const
{
   return "previewMode";
}

size_type NXPreviewMode::NumberOfElements() const
{
   return NumberOfItems;
}

IsoString NXPreviewMode::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case BeforeAfter:       return "PreviewMode_BeforeAfter";
   case RegionMap:         return "PreviewMode_RegionMap";
   case IndividualRegions: return "PreviewMode_IndividualRegions";
   case StretchedResult:   return "PreviewMode_StretchedResult";
   }
}

int NXPreviewMode::ElementValue( size_type i ) const
{
   return int( i );
}

size_type NXPreviewMode::DefaultValueIndex() const
{
   return Default;
}

// ----------------------------------------------------------------------------
// Stretch Algorithm
// ----------------------------------------------------------------------------

NXStretchAlgorithm* TheNXStretchAlgorithmParameter = nullptr;

NXStretchAlgorithm::NXStretchAlgorithm( MetaProcess* P ) : MetaEnumeration( P )
{
   TheNXStretchAlgorithmParameter = this;
}

IsoString NXStretchAlgorithm::Id() const
{
   return "stretchAlgorithm";
}

size_type NXStretchAlgorithm::NumberOfElements() const
{
   return NumberOfItems;
}

IsoString NXStretchAlgorithm::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case MTF:         return "Algorithm_MTF";
   case Histogram:   return "Algorithm_Histogram";
   case GHS:         return "Algorithm_GHS";
   case ArcSinh:     return "Algorithm_ArcSinh";
   case Log:         return "Algorithm_Log";
   case Lumpton:     return "Algorithm_Lumpton";
   case RNC:         return "Algorithm_RNC";
   case Photometric: return "Algorithm_Photometric";
   case OTS:         return "Algorithm_OTS";
   case SAS:         return "Algorithm_SAS";
   case Veralux:     return "Algorithm_Veralux";
   case Auto:        return "Algorithm_Auto";
   }
}

int NXStretchAlgorithm::ElementValue( size_type i ) const
{
   return int( i );
}

size_type NXStretchAlgorithm::DefaultValueIndex() const
{
   return Default;
}

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

NXAutoSegment* TheNXAutoSegmentParameter = nullptr;

NXAutoSegment::NXAutoSegment( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXAutoSegmentParameter = this;
}

IsoString NXAutoSegment::Id() const
{
   return "autoSegment";
}

bool NXAutoSegment::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXAutoSelect* TheNXAutoSelectParameter = nullptr;

NXAutoSelect::NXAutoSelect( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXAutoSelectParameter = this;
}

IsoString NXAutoSelect::Id() const
{
   return "autoSelect";
}

bool NXAutoSelect::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableLRGB* TheNXEnableLRGBParameter = nullptr;

NXEnableLRGB::NXEnableLRGB( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableLRGBParameter = this;
}

IsoString NXEnableLRGB::Id() const
{
   return "enableLRGB";
}

bool NXEnableLRGB::DefaultValue() const
{
   return false;
}

// ----------------------------------------------------------------------------

NXEnableToneMapping* TheNXEnableToneMappingParameter = nullptr;

NXEnableToneMapping::NXEnableToneMapping( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableToneMappingParameter = this;
}

IsoString NXEnableToneMapping::Id() const
{
   return "enableToneMapping";
}

bool NXEnableToneMapping::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXAutoBlackPoint* TheNXAutoBlackPointParameter = nullptr;

NXAutoBlackPoint::NXAutoBlackPoint( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXAutoBlackPointParameter = this;
}

IsoString NXAutoBlackPoint::Id() const
{
   return "autoBlackPoint";
}

bool NXAutoBlackPoint::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

NXGlobalContrast* TheNXGlobalContrastParameter = nullptr;

NXGlobalContrast::NXGlobalContrast( MetaProcess* P ) : MetaFloat( P )
{
   TheNXGlobalContrastParameter = this;
}

IsoString NXGlobalContrast::Id() const
{
   return "globalContrast";
}

int NXGlobalContrast::Precision() const
{
   return 3;
}

double NXGlobalContrast::MinimumValue() const
{
   return 0.0;
}

double NXGlobalContrast::MaximumValue() const
{
   return 2.0;
}

double NXGlobalContrast::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------

NXSaturationBoost* TheNXSaturationBoostParameter = nullptr;

NXSaturationBoost::NXSaturationBoost( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSaturationBoostParameter = this;
}

IsoString NXSaturationBoost::Id() const
{
   return "saturationBoost";
}

int NXSaturationBoost::Precision() const
{
   return 3;
}

double NXSaturationBoost::MinimumValue() const
{
   return 0.0;
}

double NXSaturationBoost::MaximumValue() const
{
   return 2.0;
}

double NXSaturationBoost::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------

NXBlendRadius* TheNXBlendRadiusParameter = nullptr;

NXBlendRadius::NXBlendRadius( MetaProcess* P ) : MetaFloat( P )
{
   TheNXBlendRadiusParameter = this;
}

IsoString NXBlendRadius::Id() const
{
   return "blendRadius";
}

int NXBlendRadius::Precision() const
{
   return 1;
}

double NXBlendRadius::MinimumValue() const
{
   return 0.0;
}

double NXBlendRadius::MaximumValue() const
{
   return 50.0;
}

double NXBlendRadius::DefaultValue() const
{
   return 5.0;
}

// ----------------------------------------------------------------------------

NXStretchStrength* TheNXStretchStrengthParameter = nullptr;

NXStretchStrength::NXStretchStrength( MetaProcess* P ) : MetaFloat( P )
{
   TheNXStretchStrengthParameter = this;
}

IsoString NXStretchStrength::Id() const
{
   return "stretchStrength";
}

int NXStretchStrength::Precision() const
{
   return 3;
}

double NXStretchStrength::MinimumValue() const
{
   return 0.0;
}

double NXStretchStrength::MaximumValue() const
{
   return 2.0;
}

double NXStretchStrength::DefaultValue() const
{
   return 0.5;  // Conservative default - users can increase if needed
}

// ----------------------------------------------------------------------------

NXBlackPoint* TheNXBlackPointParameter = nullptr;

NXBlackPoint::NXBlackPoint( MetaProcess* P ) : MetaFloat( P )
{
   TheNXBlackPointParameter = this;
}

IsoString NXBlackPoint::Id() const
{
   return "blackPoint";
}

int NXBlackPoint::Precision() const
{
   return 6;
}

double NXBlackPoint::MinimumValue() const
{
   return 0.0;
}

double NXBlackPoint::MaximumValue() const
{
   return 0.5;
}

double NXBlackPoint::DefaultValue() const
{
   return 0.0;
}

// ----------------------------------------------------------------------------

NXWhitePoint* TheNXWhitePointParameter = nullptr;

NXWhitePoint::NXWhitePoint( MetaProcess* P ) : MetaFloat( P )
{
   TheNXWhitePointParameter = this;
}

IsoString NXWhitePoint::Id() const
{
   return "whitePoint";
}

int NXWhitePoint::Precision() const
{
   return 6;
}

double NXWhitePoint::MinimumValue() const
{
   return 0.5;
}

double NXWhitePoint::MaximumValue() const
{
   return 1.0;
}

double NXWhitePoint::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------

NXGamma* TheNXGammaParameter = nullptr;

NXGamma::NXGamma( MetaProcess* P ) : MetaFloat( P )
{
   TheNXGammaParameter = this;
}

IsoString NXGamma::Id() const
{
   return "gamma";
}

int NXGamma::Precision() const
{
   return 3;
}

double NXGamma::MinimumValue() const
{
   return 0.1;
}

double NXGamma::MaximumValue() const
{
   return 5.0;
}

double NXGamma::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------
// Region Enable Parameters
// ----------------------------------------------------------------------------

NXEnableStarCores* TheNXEnableStarCoresParameter = nullptr;

NXEnableStarCores::NXEnableStarCores( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableStarCoresParameter = this;
}

IsoString NXEnableStarCores::Id() const
{
   return "enableStarCores";
}

bool NXEnableStarCores::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableStarHalos* TheNXEnableStarHalosParameter = nullptr;

NXEnableStarHalos::NXEnableStarHalos( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableStarHalosParameter = this;
}

IsoString NXEnableStarHalos::Id() const
{
   return "enableStarHalos";
}

bool NXEnableStarHalos::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableNebulaBright* TheNXEnableNebulaBrightParameter = nullptr;

NXEnableNebulaBright::NXEnableNebulaBright( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableNebulaBrightParameter = this;
}

IsoString NXEnableNebulaBright::Id() const
{
   return "enableNebulaBright";
}

bool NXEnableNebulaBright::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableNebulaFaint* TheNXEnableNebulaFaintParameter = nullptr;

NXEnableNebulaFaint::NXEnableNebulaFaint( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableNebulaFaintParameter = this;
}

IsoString NXEnableNebulaFaint::Id() const
{
   return "enableNebulaFaint";
}

bool NXEnableNebulaFaint::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableDustLanes* TheNXEnableDustLanesParameter = nullptr;

NXEnableDustLanes::NXEnableDustLanes( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableDustLanesParameter = this;
}

IsoString NXEnableDustLanes::Id() const
{
   return "enableDustLanes";
}

bool NXEnableDustLanes::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableGalaxyCore* TheNXEnableGalaxyCoreParameter = nullptr;

NXEnableGalaxyCore::NXEnableGalaxyCore( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableGalaxyCoreParameter = this;
}

IsoString NXEnableGalaxyCore::Id() const
{
   return "enableGalaxyCore";
}

bool NXEnableGalaxyCore::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableGalaxyHalo* TheNXEnableGalaxyHaloParameter = nullptr;

NXEnableGalaxyHalo::NXEnableGalaxyHalo( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableGalaxyHaloParameter = this;
}

IsoString NXEnableGalaxyHalo::Id() const
{
   return "enableGalaxyHalo";
}

bool NXEnableGalaxyHalo::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableGalaxyArms* TheNXEnableGalaxyArmsParameter = nullptr;

NXEnableGalaxyArms::NXEnableGalaxyArms( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableGalaxyArmsParameter = this;
}

IsoString NXEnableGalaxyArms::Id() const
{
   return "enableGalaxyArms";
}

bool NXEnableGalaxyArms::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXEnableBackground* TheNXEnableBackgroundParameter = nullptr;

NXEnableBackground::NXEnableBackground( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXEnableBackgroundParameter = this;
}

IsoString NXEnableBackground::Id() const
{
   return "enableBackground";
}

bool NXEnableBackground::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
