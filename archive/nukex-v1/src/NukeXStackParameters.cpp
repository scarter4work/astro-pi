//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Input Frame List
// ----------------------------------------------------------------------------

NXSInputFrames* TheNXSInputFramesParameter = nullptr;

NXSInputFrames::NXSInputFrames( MetaProcess* P )
   : MetaTable( P )
{
   TheNXSInputFramesParameter = this;
}

IsoString NXSInputFrames::Id() const
{
   return "inputFrames";
}

size_type NXSInputFrames::MinLength() const
{
   return 2; // Need at least 2 frames to stack
}

// ----------------------------------------------------------------------------

NXSInputFramePath* TheNXSInputFramePathParameter = nullptr;

NXSInputFramePath::NXSInputFramePath( MetaTable* T )
   : MetaString( T )
{
   TheNXSInputFramePathParameter = this;
}

IsoString NXSInputFramePath::Id() const
{
   return "path";
}

// ----------------------------------------------------------------------------

NXSInputFrameEnabled* TheNXSInputFrameEnabledParameter = nullptr;

NXSInputFrameEnabled::NXSInputFrameEnabled( MetaTable* T )
   : MetaBoolean( T )
{
   TheNXSInputFrameEnabledParameter = this;
}

IsoString NXSInputFrameEnabled::Id() const
{
   return "enabled";
}

bool NXSInputFrameEnabled::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------
// Selection Strategy
// ----------------------------------------------------------------------------

NXSSelectionStrategy* TheNXSSelectionStrategyParameter = nullptr;

NXSSelectionStrategy::NXSSelectionStrategy( MetaProcess* P )
   : MetaEnumeration( P )
{
   TheNXSSelectionStrategyParameter = this;
}

IsoString NXSSelectionStrategy::Id() const
{
   return "selectionStrategy";
}

size_type NXSSelectionStrategy::NumberOfElements() const
{
   return NumberOfItems;
}

IsoString NXSSelectionStrategy::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case Distribution:   return "Distribution";
   case WeightedMedian: return "WeightedMedian";
   case MLGuided:       return "MLGuided";
   case Hybrid:         return "Hybrid";
   }
}

int NXSSelectionStrategy::ElementValue( size_type i ) const
{
   return int( i );
}

size_type NXSSelectionStrategy::DefaultValueIndex() const
{
   return Default;
}

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

NXSEnableMLSegmentation* TheNXSEnableMLSegmentationParameter = nullptr;

NXSEnableMLSegmentation::NXSEnableMLSegmentation( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableMLSegmentationParameter = this;
}

IsoString NXSEnableMLSegmentation::Id() const
{
   return "enableMLSegmentation";
}

bool NXSEnableMLSegmentation::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableTransitionSmoothing* TheNXSEnableTransitionSmoothingParameter = nullptr;

NXSEnableTransitionSmoothing::NXSEnableTransitionSmoothing( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableTransitionSmoothingParameter = this;
}

IsoString NXSEnableTransitionSmoothing::Id() const
{
   return "enableTransitionSmoothing";
}

bool NXSEnableTransitionSmoothing::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSUseSpatialContext* TheNXSUseSpatialContextParameter = nullptr;

NXSUseSpatialContext::NXSUseSpatialContext( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSUseSpatialContextParameter = this;
}

IsoString NXSUseSpatialContext::Id() const
{
   return "useSpatialContext";
}

bool NXSUseSpatialContext::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSUseTargetContext* TheNXSUseTargetContextParameter = nullptr;

NXSUseTargetContext::NXSUseTargetContext( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSUseTargetContextParameter = this;
}

IsoString NXSUseTargetContext::Id() const
{
   return "useTargetContext";
}

bool NXSUseTargetContext::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSGenerateMetadata* TheNXSGenerateMetadataParameter = nullptr;

NXSGenerateMetadata::NXSGenerateMetadata( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSGenerateMetadataParameter = this;
}

IsoString NXSGenerateMetadata::Id() const
{
   return "generateMetadata";
}

bool NXSGenerateMetadata::DefaultValue() const
{
   return false;
}

// ----------------------------------------------------------------------------

NXSEnableAutoStretch* TheNXSEnableAutoStretchParameter = nullptr;

NXSEnableAutoStretch::NXSEnableAutoStretch( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableAutoStretchParameter = this;
}

IsoString NXSEnableAutoStretch::Id() const
{
   return "enableAutoStretch";
}

bool NXSEnableAutoStretch::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableRegistration* TheNXSEnableRegistrationParameter = nullptr;

NXSEnableRegistration::NXSEnableRegistration( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableRegistrationParameter = this;
}

IsoString NXSEnableRegistration::Id() const
{
   return "enableRegistration";
}

bool NXSEnableRegistration::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableNormalization* TheNXSEnableNormalizationParameter = nullptr;

NXSEnableNormalization::NXSEnableNormalization( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableNormalizationParameter = this;
}

IsoString NXSEnableNormalization::Id() const
{
   return "enableNormalization";
}

bool NXSEnableNormalization::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableQualityWeighting* TheNXSEnableQualityWeightingParameter = nullptr;

NXSEnableQualityWeighting::NXSEnableQualityWeighting( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableQualityWeightingParameter = this;
}

IsoString NXSEnableQualityWeighting::Id() const
{
   return "enableQualityWeighting";
}

bool NXSEnableQualityWeighting::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSExcludeFailedRegistration* TheNXSExcludeFailedRegistrationParameter = nullptr;

NXSExcludeFailedRegistration::NXSExcludeFailedRegistration( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSExcludeFailedRegistrationParameter = this;
}

IsoString NXSExcludeFailedRegistration::Id() const
{
   return "excludeFailedRegistration";
}

bool NXSExcludeFailedRegistration::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

NXSOutlierSigmaThreshold* TheNXSOutlierSigmaThresholdParameter = nullptr;

NXSOutlierSigmaThreshold::NXSOutlierSigmaThreshold( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSOutlierSigmaThresholdParameter = this;
}

IsoString NXSOutlierSigmaThreshold::Id() const
{
   return "outlierSigmaThreshold";
}

int NXSOutlierSigmaThreshold::Precision() const
{
   return 2;
}

double NXSOutlierSigmaThreshold::MinimumValue() const
{
   return 1.0;
}

double NXSOutlierSigmaThreshold::MaximumValue() const
{
   return 10.0;
}

double NXSOutlierSigmaThreshold::DefaultValue() const
{
   return 3.0;
}

// ----------------------------------------------------------------------------

NXSSmoothingStrength* TheNXSSmoothingStrengthParameter = nullptr;

NXSSmoothingStrength::NXSSmoothingStrength( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSSmoothingStrengthParameter = this;
}

IsoString NXSSmoothingStrength::Id() const
{
   return "smoothingStrength";
}

int NXSSmoothingStrength::Precision() const
{
   return 2;
}

double NXSSmoothingStrength::MinimumValue() const
{
   return 0.0;
}

double NXSSmoothingStrength::MaximumValue() const
{
   return 1.0;
}

double NXSSmoothingStrength::DefaultValue() const
{
   return 0.5;
}

// ----------------------------------------------------------------------------

NXSTransitionThreshold* TheNXSTransitionThresholdParameter = nullptr;

NXSTransitionThreshold::NXSTransitionThreshold( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSTransitionThresholdParameter = this;
}

IsoString NXSTransitionThreshold::Id() const
{
   return "transitionThreshold";
}

int NXSTransitionThreshold::Precision() const
{
   return 3;
}

double NXSTransitionThreshold::MinimumValue() const
{
   return 0.001;
}

double NXSTransitionThreshold::MaximumValue() const
{
   return 0.5;
}

double NXSTransitionThreshold::DefaultValue() const
{
   return 0.05;
}

// ----------------------------------------------------------------------------
// Integer Parameters
// ----------------------------------------------------------------------------

NXSTileSize* TheNXSTileSizeParameter = nullptr;

NXSTileSize::NXSTileSize( MetaProcess* P )
   : MetaInt32( P )
{
   TheNXSTileSizeParameter = this;
}

IsoString NXSTileSize::Id() const
{
   return "tileSize";
}

double NXSTileSize::MinimumValue() const
{
   return 8;
}

double NXSTileSize::MaximumValue() const
{
   return 128;
}

double NXSTileSize::DefaultValue() const
{
   return 16;
}

// ----------------------------------------------------------------------------

NXSSmoothingRadius* TheNXSSmoothingRadiusParameter = nullptr;

NXSSmoothingRadius::NXSSmoothingRadius( MetaProcess* P )
   : MetaInt32( P )
{
   TheNXSSmoothingRadiusParameter = this;
}

IsoString NXSSmoothingRadius::Id() const
{
   return "smoothingRadius";
}

double NXSSmoothingRadius::MinimumValue() const
{
   return 1;
}

double NXSSmoothingRadius::MaximumValue() const
{
   return 20;
}

double NXSSmoothingRadius::DefaultValue() const
{
   return 3;
}

// ----------------------------------------------------------------------------

} // namespace pcl
