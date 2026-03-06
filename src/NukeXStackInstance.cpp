//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackInstance.h"
#include "NukeXStackParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackInstance::NukeXStackInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_qualityWeightMode( NXSQualityWeightMode::Default )
   , p_generateProvenance( TheNXSGenerateProvenanceParameter->DefaultValue() )
   , p_generateDistMetadata( TheNXSGenerateDistMetadataParameter->DefaultValue() )
   , p_enableQualityWeighting( TheNXSEnableQualityWeightingParameter->DefaultValue() )
   , p_outlierSigmaThreshold( static_cast<float>( TheNXSOutlierSigmaThresholdParameter->DefaultValue() ) )
   , p_fwhmWeight( static_cast<float>( TheNXSFWHMWeightParameter->DefaultValue() ) )
   , p_eccentricityWeight( static_cast<float>( TheNXSEccentricityWeightParameter->DefaultValue() ) )
   , p_skyBackgroundWeight( static_cast<float>( TheNXSSkyBackgroundWeightParameter->DefaultValue() ) )
   , p_hfrWeight( static_cast<float>( TheNXSHFRWeightParameter->DefaultValue() ) )
   , p_altitudeWeight( static_cast<float>( TheNXSAltitudeWeightParameter->DefaultValue() ) )
{
}

// ----------------------------------------------------------------------------

NukeXStackInstance::NukeXStackInstance( const NukeXStackInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

// ----------------------------------------------------------------------------

void NukeXStackInstance::Assign( const ProcessImplementation& p )
{
   const NukeXStackInstance* x = dynamic_cast<const NukeXStackInstance*>( &p );
   if ( x != nullptr )
   {
      p_inputFrames             = x->p_inputFrames;
      p_qualityWeightMode       = x->p_qualityWeightMode;
      p_generateProvenance      = x->p_generateProvenance;
      p_generateDistMetadata    = x->p_generateDistMetadata;
      p_enableQualityWeighting  = x->p_enableQualityWeighting;
      p_outlierSigmaThreshold   = x->p_outlierSigmaThreshold;
      p_fwhmWeight              = x->p_fwhmWeight;
      p_eccentricityWeight      = x->p_eccentricityWeight;
      p_skyBackgroundWeight     = x->p_skyBackgroundWeight;
      p_hfrWeight               = x->p_hfrWeight;
      p_altitudeWeight          = x->p_altitudeWeight;
   }
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::CanExecuteGlobal( String& whyNot ) const
{
   int enabledCount = 0;
   for ( const auto& frame : p_inputFrames )
      if ( frame.enabled )
         ++enabledCount;

   if ( enabledCount < 2 )
   {
      whyNot = "At least 2 input frames must be enabled.";
      return false;
   }

   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::ExecuteGlobal()
{
   // Stub - full implementation in Task 3.1
   return false;
}

// ----------------------------------------------------------------------------

void* NukeXStackInstance::LockParameter( const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXSInputFramePathParameter )
   {
      if ( tableRow >= p_inputFrames.size() )
         return nullptr;
      return p_inputFrames[tableRow].path.Begin();
   }
   if ( p == TheNXSInputFrameEnabledParameter )
   {
      if ( tableRow >= p_inputFrames.size() )
         return nullptr;
      return &p_inputFrames[tableRow].enabled;
   }
   if ( p == TheNXSQualityWeightModeParameter )
      return &p_qualityWeightMode;
   if ( p == TheNXSGenerateProvenanceParameter )
      return &p_generateProvenance;
   if ( p == TheNXSGenerateDistMetadataParameter )
      return &p_generateDistMetadata;
   if ( p == TheNXSEnableQualityWeightingParameter )
      return &p_enableQualityWeighting;
   if ( p == TheNXSOutlierSigmaThresholdParameter )
      return &p_outlierSigmaThreshold;
   if ( p == TheNXSFWHMWeightParameter )
      return &p_fwhmWeight;
   if ( p == TheNXSEccentricityWeightParameter )
      return &p_eccentricityWeight;
   if ( p == TheNXSSkyBackgroundWeightParameter )
      return &p_skyBackgroundWeight;
   if ( p == TheNXSHFRWeightParameter )
      return &p_hfrWeight;
   if ( p == TheNXSAltitudeWeightParameter )
      return &p_altitudeWeight;

   return nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXSInputFramesParameter )
   {
      p_inputFrames.clear();
      if ( sizeOrLength > 0 )
         p_inputFrames.resize( sizeOrLength );
      return true;
   }

   if ( p == TheNXSInputFramePathParameter )
   {
      p_inputFrames[tableRow].path.Clear();
      if ( sizeOrLength > 0 )
         p_inputFrames[tableRow].path.SetLength( sizeOrLength );
      return true;
   }

   return false;
}

// ----------------------------------------------------------------------------

size_type NukeXStackInstance::ParameterLength( const MetaParameter* p, size_type tableRow ) const
{
   if ( p == TheNXSInputFramesParameter )
      return p_inputFrames.size();

   if ( p == TheNXSInputFramePathParameter )
   {
      if ( tableRow >= p_inputFrames.size() )
         return 0;
      return p_inputFrames[tableRow].path.Length();
   }

   return 0;
}

// ----------------------------------------------------------------------------

} // namespace pcl
