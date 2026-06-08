//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// This file is part of NukeX.
//
// NukeX is free software: you can redistribute it and/or modify it under
// the terms of the MIT License.

#ifndef __NukeXInstance_h
#define __NukeXInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

#include <set>

#include "NukeXParameters.h"
#include "engine/Compositor.h"
#include "engine/RegionStatistics.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXInstance : public ProcessImplementation
{
public:

   NukeXInstance( const MetaProcess* );
   NukeXInstance( const NukeXInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool Validate( String& info ) override;
   UndoFlags UndoMode( const View& ) const override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   // Real-time preview support
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool IsRealTimePreviewEnabled( const View& view ) const;

   // Generate preview image
   Image GeneratePreview( const Image& input, int previewMode ) const;

private:

   // Process parameters
   pcl_enum p_processingMode;
   pcl_enum p_previewMode;
   pcl_enum p_stretchAlgorithm;
   pcl_bool p_autoSegment;
   pcl_bool p_autoSelect;
   pcl_bool p_enableLRGB;
   pcl_bool p_enableToneMapping;
   pcl_bool p_autoBlackPoint;
   float    p_globalContrast;
   float    p_saturationBoost;
   float    p_blendRadius;
   float    p_stretchStrength;
   float    p_blackPoint;
   float    p_whitePoint;
   float    p_gamma;

   // Region enable flags
   pcl_bool p_enableStarCores;
   pcl_bool p_enableStarHalos;
   pcl_bool p_enableNebulaBright;
   pcl_bool p_enableNebulaFaint;
   pcl_bool p_enableDustLanes;
   pcl_bool p_enableGalaxyCore;
   pcl_bool p_enableGalaxyHalo;
   pcl_bool p_enableGalaxyArms;
   pcl_bool p_enableBackground;

   // Helper methods
   CompositorConfig BuildCompositorConfig() const;
   std::set<RegionClass> GetEnabledRegions() const;
   AlgorithmType GetSelectedAlgorithm() const;

   friend class NukeXProcess;
   friend class NukeXInterface;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXInstance_h
