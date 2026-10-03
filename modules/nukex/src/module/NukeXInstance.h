// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXInstance_h
#define __NukeXInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

#include "nukex/stretch/image_stats.hpp"
#include "nukex/stacker/cache_paths.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace pcl
{

// Forward declaration — full definition lives in RatingDialog.h. Task 19
// replaces SaveRatingFromLastRun's body with a real atomic-write impl that
// uses the fields on this struct.
struct RatingResult;

class NukeXInstance : public ProcessImplementation
{
public:
   NukeXInstance( const MetaProcess* );
   NukeXInstance( const NukeXInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool IsHistoryUpdater( const View& ) const override;
   UndoFlags UndoMode( const View& ) const override;

   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
   bool Validate( String& whyNot ) override;

   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   // ── Parameter storage ─────────────────────────────────────────

   struct FrameItem {
      String path;
      bool   enabled = true;
   };

   typedef Array<FrameItem> frame_list;

   frame_list  lightFrames;
   frame_list  flatFrames;
   pcl_enum    primaryStretch    = 0;  // NXPrimaryStretch::Auto
   pcl_enum    finishingStretch  = 0;  // NXFinishingStretch::None
   // Where the auto-stretch puts the sky. 0.25 is the screen-autostretch
   // convention and it was the default until v5.0.4.1, but a screen stretch is
   // a thing you look through, not a thing you keep: it spends a quarter of
   // the range on sky and leaves the subject sitting on a bright grey
   // pedestal. Measured on the composed output of four real corpora, dropping
   // it to 0.12 raises signal saturation 1.34x to 1.72x and clips nothing --
   // black stays at 0.0000% and white unmoved, because the shadow point is
   // bounded independently.
   float       backgroundTarget = 0.12f;
   // Remove the fixed-pattern sky tilt from the stack (plane only; the sky
   // level is untouched). See StackingEngine::Config::remove_sky_gradient.
   pcl_bool    removeSkyGradient = true;
   // Phase B estimator: 0 = distribution model race, 1 = Huber M-estimator
   // (the default since 5.0.6.0; measured less noisy on every corpus).
   pcl_enum    estimator = 1;
   pcl_bool    enableGPU       = true;
   String      cacheDirectory  = String( nukex::default_cache_dir().c_str() );
   String      qeOverridePath;  // optional path to qe_overrides.json; empty = none

   // Output (populated by ExecuteGlobal, readable from PJSR).
   int32       nFramesProcessed        = 0;
   int32       nFramesFailedAlignment  = 0;  // real alignment misses only
   int32       nFramesRejectedFilter   = 0;  // unknown FILTER on Bayer (Task 12)

   // Phase 8: after Execute we stash enough state to save a rating row later.
   // Populated unconditionally at the end of ExecuteGlobal (even when the
   // popup is suppressed by env var / user opt-out), so Task 18's "Rate
   // last run" button can reopen the dialog without re-running the stack.
   struct LastRunState {
      bool                             valid            = false;
      nukex::ImageStats                stats;
      std::string                      stretch_name;
      int                              filter_class     = 0;
      int                              target_class     = 0;
      std::string                      params_json_applied;
      std::array<std::uint8_t, 16>     run_id           {};
      std::int64_t                     created_at_unix  = 0;

      // Resolved at Execute time so Task 19's SaveRatingFromLastRun can reach
      // the same files the predict path read. Re-deriving would risk drift if
      // HOME changes between the run and the Save click.
      std::string                      user_db_path;
      std::string                      user_model_json_path;
   };
   LastRunState lastRun;

   // Phase 8: persist the most recent rating. Task 17 stubbed; Task 19
   // implements atomic tmp + fsync + rename into the per-user rating DB.
   // Caller must ensure lastRun.valid == true before calling.
   // Public so NukeXInterface's "Rate last run" button (Task 18) can reach it.
   void SaveRatingFromLastRun( const pcl::RatingResult& res );
};

// No singleton — PCL creates instances per-use via Process::Create()/Clone()

} // namespace pcl

#endif // __NukeXInstance_h
