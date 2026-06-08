// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXProgress_h
#define __NukeXProgress_h

#include <pcl/Console.h>
#include <pcl/StandardStatus.h>
#include <pcl/StatusMonitor.h>

#include "nukex/core/progress_observer.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pcl
{

/// Bridges nukex::ProgressObserver to PI's Console + StatusMonitor.
/// The outermost begin_phase() drives the progress bar.
/// Inner phases and advance() detail strings write indented text to Console.
///
/// In addition to PI's Process Console (which does not reach the shell under
/// --automation-mode), this class emits real-time progress via three sideband
/// channels so headless harnesses can observe liveness:
///   1. std::cerr line per event (reaches the shell via stderr).
///   2. Append log at /tmp/nukex_progress.log (shell: `tail -f`).
///   3. Heartbeat file at /tmp/nukex_heartbeat.txt, overwritten every 10 s by
///      a watchdog thread, even during long silent compute loops (Phase B
///      inside a single GPU batch can go 30+ minutes without a callback).
class NukeXProgress : public nukex::ProgressObserver
{
public:
   NukeXProgress();
   ~NukeXProgress() override;

   void begin_phase( const std::string& name, int total_steps ) override;
   void advance( int steps, const std::string& detail ) override;
   void end_phase() override;
   void message( const std::string& text ) override;
   bool is_cancelled() const override;

private:
   struct Phase {
      std::string name;
      int total;
      int current;
   };

   Console            console_;
   StandardStatus     status_;
   StatusMonitor      monitor_;
   std::vector<Phase> stack_;
   int                depth_ = 0;  // nesting depth for indentation

   // Sideband state (accessed by both the caller thread and the watchdog).
   std::mutex                                     state_mtx_;
   std::string                                    last_phase_name_;
   std::string                                    last_detail_;
   int                                            last_step_   = 0;
   int                                            last_total_  = 0;
   std::chrono::steady_clock::time_point          start_time_;

   // Watchdog thread that ticks the heartbeat file every 10 seconds.
   std::thread                                    watchdog_;
   std::atomic<bool>                              stop_watchdog_{false};
   std::mutex                                     watchdog_mtx_;
   std::condition_variable                        watchdog_cv_;

   void emit_sideband( const char* event_kind, const std::string& payload );
   void write_heartbeat_locked( const char* reason );
   void watchdog_loop();
};

} // namespace pcl

#endif // __NukeXProgress_h
