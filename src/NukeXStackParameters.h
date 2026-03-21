//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack - Per-pixel statistical inference stacking

#ifndef __NukeXStackParameters_h
#define __NukeXStackParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// Input Frame List (table parameter)
// ----------------------------------------------------------------------------

class NXSInputFrames : public MetaTable
{
public:
   NXSInputFrames( MetaProcess* );
   IsoString Id() const override;
   size_type MinLength() const override;
};

extern NXSInputFrames* TheNXSInputFramesParameter;

// Frame path within the table
class NXSInputFramePath : public MetaString
{
public:
   NXSInputFramePath( MetaTable* );
   IsoString Id() const override;
};

extern NXSInputFramePath* TheNXSInputFramePathParameter;

// Frame enabled flag within the table
class NXSInputFrameEnabled : public MetaBoolean
{
public:
   NXSInputFrameEnabled( MetaTable* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSInputFrameEnabled* TheNXSInputFrameEnabledParameter;

// ----------------------------------------------------------------------------
// Flat Frame List (table parameter)
// ----------------------------------------------------------------------------

class NXSFlatFrames : public MetaTable
{
public:
   NXSFlatFrames( MetaProcess* );
   IsoString Id() const override;
};

extern NXSFlatFrames* TheNXSFlatFramesParameter;

class NXSFlatFramePath : public MetaString
{
public:
   NXSFlatFramePath( MetaTable* );
   IsoString Id() const override;
};

extern NXSFlatFramePath* TheNXSFlatFramePathParameter;

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

class NXSGenerateProvenance : public MetaBoolean
{
public:
   NXSGenerateProvenance( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSGenerateProvenance* TheNXSGenerateProvenanceParameter;

class NXSGenerateDistMetadata : public MetaBoolean
{
public:
   NXSGenerateDistMetadata( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSGenerateDistMetadata* TheNXSGenerateDistMetadataParameter;

class NXSEnableMetadataTiebreaker : public MetaBoolean
{
public:
   NXSEnableMetadataTiebreaker( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableMetadataTiebreaker* TheNXSEnableMetadataTiebreakerParameter;

class NXSEnableAutoStretch : public MetaBoolean
{
public:
   NXSEnableAutoStretch( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableAutoStretch* TheNXSEnableAutoStretchParameter;

class NXSUseGPU : public MetaBoolean
{
public:
   NXSUseGPU( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSUseGPU* TheNXSUseGPUParameter;

class NXSAdaptiveModels : public MetaBoolean
{
public:
   NXSAdaptiveModels( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSAdaptiveModels* TheNXSAdaptiveModelsParameter;

class NXSEnableRemediation : public MetaBoolean
{
public:
   NXSEnableRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableRemediation* TheNXSEnableRemediationParameter;

class NXSEnableTrailRemediation : public MetaBoolean
{
public:
   NXSEnableTrailRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableTrailRemediation* TheNXSEnableTrailRemediationParameter;

class NXSEnableDustRemediation : public MetaBoolean
{
public:
   NXSEnableDustRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableDustRemediation* TheNXSEnableDustRemediationParameter;

class NXSEnableVignettingRemediation : public MetaBoolean
{
public:
   NXSEnableVignettingRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableVignettingRemediation* TheNXSEnableVignettingRemediationParameter;

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

class NXSTrailDilateRadius : public MetaFloat
{
public:
   NXSTrailDilateRadius( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSTrailDilateRadius* TheNXSTrailDilateRadiusParameter;

class NXSTrailOutlierSigma : public MetaFloat
{
public:
   NXSTrailOutlierSigma( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSTrailOutlierSigma* TheNXSTrailOutlierSigmaParameter;

class NXSDustCircularityMin : public MetaFloat
{
public:
   NXSDustCircularityMin( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSDustCircularityMin* TheNXSDustCircularityMinParameter;

class NXSDustDetectionSigma : public MetaFloat
{
public:
   NXSDustDetectionSigma( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSDustDetectionSigma* TheNXSDustDetectionSigmaParameter;

class NXSDustMaxCorrectionRatio : public MetaFloat
{
public:
   NXSDustMaxCorrectionRatio( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSDustMaxCorrectionRatio* TheNXSDustMaxCorrectionRatioParameter;

// ----------------------------------------------------------------------------
// Integer Parameters
// ----------------------------------------------------------------------------

class NXSDustMinDiameter : public MetaInt32
{
public:
   NXSDustMinDiameter( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSDustMinDiameter* TheNXSDustMinDiameterParameter;

class NXSDustMaxDiameter : public MetaInt32
{
public:
   NXSDustMaxDiameter( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSDustMaxDiameter* TheNXSDustMaxDiameterParameter;

class NXSDustNeighborRadius : public MetaInt32
{
public:
   NXSDustNeighborRadius( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSDustNeighborRadius* TheNXSDustNeighborRadiusParameter;

class NXSVignettingPolyOrder : public MetaInt32
{
public:
   NXSVignettingPolyOrder( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSVignettingPolyOrder* TheNXSVignettingPolyOrderParameter;

class NXSBortleNumber : public MetaInt32
{
public:
   NXSBortleNumber( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSBortleNumber* TheNXSBortleNumberParameter;

class NXSVignettingMaxCorrection : public MetaFloat
{
public:
   NXSVignettingMaxCorrection( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSVignettingMaxCorrection* TheNXSVignettingMaxCorrectionParameter;

class NXSOutlierSigmaThreshold : public MetaFloat
{
public:
   NXSOutlierSigmaThreshold( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSOutlierSigmaThreshold* TheNXSOutlierSigmaThresholdParameter;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackParameters_h
