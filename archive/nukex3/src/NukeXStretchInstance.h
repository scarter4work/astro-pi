//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStretch Instance

#ifndef __NukeXStretchInstance_h
#define __NukeXStretchInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

#include "NukeXStretchParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStretchInstance : public ProcessImplementation
{
public:

   NukeXStretchInstance( const MetaProcess* );
   NukeXStretchInstance( const NukeXStretchInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool Validate( String& info ) override;
   UndoFlags UndoMode( const View& ) const override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

private:

   // Process parameters
   pcl_enum p_stretchAlgorithm;
   pcl_bool p_autoBlackPoint;
   float    p_contrast;
   float    p_saturation;
   float    p_blackPoint;
   float    p_whitePoint;
   float    p_gamma;
   float    p_stretchStrength;

   friend class NukeXStretchProcess;
   friend class NukeXStretchInterface;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStretchInstance_h
