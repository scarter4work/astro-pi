//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack - Intelligent pixel selection for subframe integration

#ifndef __NukeXStackInstance_h
#define __NukeXStackInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>
#include <pcl/Image.h>

#include "NukeXStackParameters.h"
#include "engine/PixelSelector.h"
#include "engine/TransitionChecker.h"
#include "engine/FrameStreamer.h"
#include "engine/FrameRegistration.h"

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
// Integration result summary
// ----------------------------------------------------------------------------

struct IntegrationSummary
{
   int totalFrames = 0;
   int enabledFrames = 0;
   int processedPixels = 0;
   int outlierPixels = 0;
   int smoothedTransitions = 0;
   double segmentationTimeMs = 0.0;
   double selectionTimeMs = 0.0;
   double smoothingTimeMs = 0.0;
   double totalTimeMs = 0.0;
   String targetObject;
};

// ----------------------------------------------------------------------------

class NukeXStackInstance : public ProcessImplementation
{
public:

   NukeXStackInstance( const MetaProcess* );
   NukeXStackInstance( const NukeXStackInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool Validate( String& info ) override;
   UndoFlags UndoMode( const View& ) const override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   // Frame management
   void AddInputFrame( const String& path, bool enabled = true );
   void ClearInputFrames();
   size_type InputFrameCount() const { return p_inputFrames.size(); }

private:

   // Input frames table
   std::vector<InputFrameData> p_inputFrames;

   // Selection parameters
   pcl_enum p_selectionStrategy;
   pcl_bool p_enableMLSegmentation;
   pcl_bool p_enableTransitionSmoothing;
   pcl_bool p_useSpatialContext;
   pcl_bool p_useTargetContext;
   pcl_bool p_generateMetadata;
   pcl_bool p_enableAutoStretch = true;
   pcl_bool p_enableRegistration = true;

   // Numeric parameters
   float    p_outlierSigmaThreshold;
   float    p_smoothingStrength;
   float    p_transitionThreshold;
   int32    p_tileSize;
   int32    p_smoothingRadius;

   // Helper methods
   PixelSelectorConfig BuildSelectorConfig() const;
   TransitionCheckerConfig BuildTransitionConfig() const;
   StackAnalysisConfig BuildStackConfig() const;

   // Load a FITS file and return the image
   bool LoadFrame( const String& path, Image& image, FITSKeywordArray& keywords ) const;

   // Run the integration pipeline
   bool RunIntegration( std::vector<Image>& frames,
                        const FITSKeywordArray& keywords,
                        Image& output,
                        IntegrationSummary& summary ) const;

   // Create a median reference image from the stack for segmentation
   Image CreateMedianReference( const std::vector<Image>& frames ) const;

   // Streaming variants for large stacks
   Image CreateMedianReferenceStreaming( FrameStreamer& streamer ) const;

   bool RunIntegrationStreaming( FrameStreamer& streamer,
                                const FITSKeywordArray& keywords,
                                Image& output,
                                IntegrationSummary& summary ) const;

   // Run ML segmentation on the reference image and populate maps
   bool RunSegmentation( const Image& referenceImage,
                         std::vector<std::vector<int>>& classMap,
                         std::vector<std::vector<float>>& confidenceMap,
                         double& segmentationTimeMs ) const;

   // Register all frames to reference using star alignment
   bool RegisterAllFrames( std::vector<Image>& frames ) const;

   friend class NukeXStackProcess;
   friend class NukeXStackInterface;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackInstance_h
