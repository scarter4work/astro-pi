// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXInstance_h
#define __NukeXInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

#include "nukex/stretch/image_stats.hpp"

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
   pcl_bool    enableGPU       = true;
   String      cacheDirectory  = "/tmp";

   // Output (populated by ExecuteGlobal, readable from PJSR).
   int32       nFramesProcessed        = 0;
   int32       nFramesFailedAlignment  = 0;

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
