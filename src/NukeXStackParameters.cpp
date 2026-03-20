//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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
// Boolean Parameters
// ----------------------------------------------------------------------------

NXSGenerateProvenance* TheNXSGenerateProvenanceParameter = nullptr;

NXSGenerateProvenance::NXSGenerateProvenance( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSGenerateProvenanceParameter = this;
}

IsoString NXSGenerateProvenance::Id() const
{
   return "generateProvenance";
}

bool NXSGenerateProvenance::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSGenerateDistMetadata* TheNXSGenerateDistMetadataParameter = nullptr;

NXSGenerateDistMetadata::NXSGenerateDistMetadata( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSGenerateDistMetadataParameter = this;
}

IsoString NXSGenerateDistMetadata::Id() const
{
   return "generateDistMetadata";
}

bool NXSGenerateDistMetadata::DefaultValue() const
{
   return false;
}

// ----------------------------------------------------------------------------

NXSEnableMetadataTiebreaker* TheNXSEnableMetadataTiebreakerParameter = nullptr;

NXSEnableMetadataTiebreaker::NXSEnableMetadataTiebreaker( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableMetadataTiebreakerParameter = this;
}

IsoString NXSEnableMetadataTiebreaker::Id() const
{
   return "enableMetadataTiebreaker";
}

bool NXSEnableMetadataTiebreaker::DefaultValue() const
{
   return true;
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

NXSUseGPU* TheNXSUseGPUParameter = nullptr;

NXSUseGPU::NXSUseGPU( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSUseGPUParameter = this;
}

IsoString NXSUseGPU::Id() const
{
   return "useGPU";
}

bool NXSUseGPU::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSAdaptiveModels* TheNXSAdaptiveModelsParameter = nullptr;

NXSAdaptiveModels::NXSAdaptiveModels( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSAdaptiveModelsParameter = this;
}

IsoString NXSAdaptiveModels::Id() const
{
   return "adaptiveModels";
}

bool NXSAdaptiveModels::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableRemediation* TheNXSEnableRemediationParameter = nullptr;

NXSEnableRemediation::NXSEnableRemediation( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableRemediationParameter = this;
}

IsoString NXSEnableRemediation::Id() const
{
   return "enableRemediation";
}

bool NXSEnableRemediation::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableTrailRemediation* TheNXSEnableTrailRemediationParameter = nullptr;

NXSEnableTrailRemediation::NXSEnableTrailRemediation( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableTrailRemediationParameter = this;
}

IsoString NXSEnableTrailRemediation::Id() const
{
   return "enableTrailRemediation";
}

bool NXSEnableTrailRemediation::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableDustRemediation* TheNXSEnableDustRemediationParameter = nullptr;

NXSEnableDustRemediation::NXSEnableDustRemediation( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableDustRemediationParameter = this;
}

IsoString NXSEnableDustRemediation::Id() const
{
   return "enableDustRemediation";
}

bool NXSEnableDustRemediation::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------

NXSEnableVignettingRemediation* TheNXSEnableVignettingRemediationParameter = nullptr;

NXSEnableVignettingRemediation::NXSEnableVignettingRemediation( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableVignettingRemediationParameter = this;
}

IsoString NXSEnableVignettingRemediation::Id() const
{
   return "enableVignettingRemediation";
}

bool NXSEnableVignettingRemediation::DefaultValue() const
{
   return false;
}

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

NXSTrailDilateRadius* TheNXSTrailDilateRadiusParameter = nullptr;

NXSTrailDilateRadius::NXSTrailDilateRadius( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSTrailDilateRadiusParameter = this;
}

IsoString NXSTrailDilateRadius::Id() const
{
   return "trailDilateRadius";
}

int NXSTrailDilateRadius::Precision() const
{
   return 1;
}

double NXSTrailDilateRadius::MinimumValue() const
{
   return 1.0;
}

double NXSTrailDilateRadius::MaximumValue() const
{
   return 20.0;
}

double NXSTrailDilateRadius::DefaultValue() const
{
   return 5.0;
}

// ----------------------------------------------------------------------------

NXSTrailOutlierSigma* TheNXSTrailOutlierSigmaParameter = nullptr;

NXSTrailOutlierSigma::NXSTrailOutlierSigma( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSTrailOutlierSigmaParameter = this;
}

IsoString NXSTrailOutlierSigma::Id() const
{
   return "trailOutlierSigma";
}

int NXSTrailOutlierSigma::Precision() const
{
   return 1;
}

double NXSTrailOutlierSigma::MinimumValue() const
{
   return 1.5;
}

double NXSTrailOutlierSigma::MaximumValue() const
{
   return 6.0;
}

double NXSTrailOutlierSigma::DefaultValue() const
{
   return 3.0;
}

// ----------------------------------------------------------------------------

NXSDustCircularityMin* TheNXSDustCircularityMinParameter = nullptr;

NXSDustCircularityMin::NXSDustCircularityMin( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSDustCircularityMinParameter = this;
}

IsoString NXSDustCircularityMin::Id() const
{
   return "dustCircularityMin";
}

int NXSDustCircularityMin::Precision() const
{
   return 2;
}

double NXSDustCircularityMin::MinimumValue() const
{
   return 0.3;
}

double NXSDustCircularityMin::MaximumValue() const
{
   return 1.0;
}

double NXSDustCircularityMin::DefaultValue() const
{
   return 0.5;
}

// ----------------------------------------------------------------------------

NXSDustDetectionSigma* TheNXSDustDetectionSigmaParameter = nullptr;

NXSDustDetectionSigma::NXSDustDetectionSigma( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSDustDetectionSigmaParameter = this;
}

IsoString NXSDustDetectionSigma::Id() const
{
   return "dustDetectionSigma";
}

int NXSDustDetectionSigma::Precision() const
{
   return 1;
}

double NXSDustDetectionSigma::MinimumValue() const
{
   return 1.0;
}

double NXSDustDetectionSigma::MaximumValue() const
{
   return 5.0;
}

double NXSDustDetectionSigma::DefaultValue() const
{
   return 4.0;
}

// ----------------------------------------------------------------------------

NXSDustMaxCorrectionRatio* TheNXSDustMaxCorrectionRatioParameter = nullptr;

NXSDustMaxCorrectionRatio::NXSDustMaxCorrectionRatio( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSDustMaxCorrectionRatioParameter = this;
}

IsoString NXSDustMaxCorrectionRatio::Id() const
{
   return "dustMaxCorrectionRatio";
}

int NXSDustMaxCorrectionRatio::Precision() const
{
   return 1;
}

double NXSDustMaxCorrectionRatio::MinimumValue() const
{
   return 2.0;
}

double NXSDustMaxCorrectionRatio::MaximumValue() const
{
   return 50.0;
}

double NXSDustMaxCorrectionRatio::DefaultValue() const
{
   return 10.0;
}

NXSVignettingMaxCorrection* TheNXSVignettingMaxCorrectionParameter = nullptr;

NXSVignettingMaxCorrection::NXSVignettingMaxCorrection( MetaProcess* P )
   : MetaFloat( P )
{
   TheNXSVignettingMaxCorrectionParameter = this;
}

IsoString NXSVignettingMaxCorrection::Id() const
{
   return "vignettingMaxCorrection";
}

int NXSVignettingMaxCorrection::Precision() const
{
   return 1;
}

double NXSVignettingMaxCorrection::MinimumValue() const
{
   return 1.0;
}

double NXSVignettingMaxCorrection::MaximumValue() const
{
   return 10.0;
}

double NXSVignettingMaxCorrection::DefaultValue() const
{
   return 1.5;
}

// ----------------------------------------------------------------------------
// Integer Parameters
// ----------------------------------------------------------------------------

NXSDustMinDiameter* TheNXSDustMinDiameterParameter = nullptr;

NXSDustMinDiameter::NXSDustMinDiameter( MetaProcess* P )
   : MetaInt32( P )
{
   TheNXSDustMinDiameterParameter = this;
}

IsoString NXSDustMinDiameter::Id() const
{
   return "dustMinDiameter";
}

double NXSDustMinDiameter::MinimumValue() const
{
   return 3;
}

double NXSDustMinDiameter::MaximumValue() const
{
   return 200;
}

double NXSDustMinDiameter::DefaultValue() const
{
   return 10;
}

// ----------------------------------------------------------------------------

NXSDustMaxDiameter* TheNXSDustMaxDiameterParameter = nullptr;

NXSDustMaxDiameter::NXSDustMaxDiameter( MetaProcess* P )
   : MetaInt32( P )
{
   TheNXSDustMaxDiameterParameter = this;
}

IsoString NXSDustMaxDiameter::Id() const
{
   return "dustMaxDiameter";
}

double NXSDustMaxDiameter::MinimumValue() const
{
   return 10;
}

double NXSDustMaxDiameter::MaximumValue() const
{
   return 500;
}

double NXSDustMaxDiameter::DefaultValue() const
{
   return 150;
}

// ----------------------------------------------------------------------------

NXSDustNeighborRadius* TheNXSDustNeighborRadiusParameter = nullptr;

NXSDustNeighborRadius::NXSDustNeighborRadius( MetaProcess* P )
   : MetaInt32( P )
{
   TheNXSDustNeighborRadiusParameter = this;
}

IsoString NXSDustNeighborRadius::Id() const
{
   return "dustNeighborRadius";
}

double NXSDustNeighborRadius::MinimumValue() const
{
   return 3;
}

double NXSDustNeighborRadius::MaximumValue() const
{
   return 100;
}

double NXSDustNeighborRadius::DefaultValue() const
{
   return 85;
}

// ----------------------------------------------------------------------------

NXSVignettingPolyOrder* TheNXSVignettingPolyOrderParameter = nullptr;

NXSVignettingPolyOrder::NXSVignettingPolyOrder( MetaProcess* P )
   : MetaInt32( P )
{
   TheNXSVignettingPolyOrderParameter = this;
}

IsoString NXSVignettingPolyOrder::Id() const
{
   return "vignettingPolyOrder";
}

double NXSVignettingPolyOrder::MinimumValue() const
{
   return 1;
}

double NXSVignettingPolyOrder::MaximumValue() const
{
   return 6;
}

double NXSVignettingPolyOrder::DefaultValue() const
{
   return 3;
}

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
   return 1;
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

} // namespace pcl
