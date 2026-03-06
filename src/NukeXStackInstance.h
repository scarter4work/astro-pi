//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack Instance - Stub for compilation

#ifndef __NukeXStackInstance_h
#define __NukeXStackInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

#include "NukeXStackParameters.h"

#include <vector>

namespace pcl
{

// ----------------------------------------------------------------------------
// Input frame descriptor
// ----------------------------------------------------------------------------

struct InputFrameData
{
   String   path;
   pcl_bool enabled = true;

   InputFrameData() = default;
   InputFrameData( const String& p, bool e = true ) : path( p ), enabled( e ) {}
};

// ----------------------------------------------------------------------------

class NukeXStackInstance : public ProcessImplementation
{
public:

   NukeXStackInstance( const MetaProcess* );
   NukeXStackInstance( const NukeXStackInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

private:

   // Input frames table
   std::vector<InputFrameData> p_inputFrames;

   // Quality weight mode enumeration
   pcl_enum p_qualityWeightMode;

   // Boolean parameters
   pcl_bool p_generateProvenance;
   pcl_bool p_generateDistMetadata;
   pcl_bool p_enableQualityWeighting;

   // Floating point parameters
   float    p_outlierSigmaThreshold;
   float    p_fwhmWeight;
   float    p_eccentricityWeight;
   float    p_skyBackgroundWeight;
   float    p_hfrWeight;
   float    p_altitudeWeight;

   friend class NukeXStackProcess;
   friend class NukeXStackInterface;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackInstance_h
