// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXProgress.h"
#include "NukeXConsoleText.hpp"
#include <pcl/File.h>
#include <pcl/String.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace pcl
{

namespace {

constexpr auto kHeartbeatInterval = std::chrono::seconds(10);

std::string iso_timestamp_ms() {
   auto now = std::chrono::system_clock::now();
   auto t = std::chrono::system_clock::to_time_t(now);
   auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                 now.time_since_epoch() ).count() % 1000;
   std::tm tm_val;
   localtime_r( &t, &tm_val );
   char buf[32];
   std::strftime( buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_val );
   std::ostringstream os;
   os << buf << '.' << std::setfill('0') << std::setw(3) << ms;
   return os.str();
}

} // anonymous namespace

NukeXProgress::NukeXProgress()
   : status_()
   , monitor_()
   , run_dir_( File::SystemTempDirectory().ToUTF8().c_str() )
   , progress_log_path_( run_dir_.file( "nukex_progress.log" ) )
   , heartbeat_path_( run_dir_.file( "nukex_heartbeat.txt" ) )
   , start_time_( std::chrono::steady_clock::now() )
{
   // The progress files live in a fresh private directory (mkdtemp, 0700), so
   // no other local user can pre-create or symlink them, and they go away with
   // the run. Say where they are so a shell can `tail -f` them meanwhile.
   if ( run_dir_.ok() )
   {
      std::ofstream create( progress_log_path_, std::ios::trunc );
      std::cerr << "NukeX progress files (removed when the run ends): "
                << run_dir_.path() << '\n';
      std::cerr.flush();
      console_.WriteLn( "NukeX progress files (removed when the run ends): "
                        + String::UTF8ToUTF16( run_dir_.path().c_str() ) );
   }

   // Launch watchdog thread AFTER all state is initialised. The thread will
   // only read state_mtx_-protected members.
   watchdog_ = std::thread( [this]{ this->watchdog_loop(); } );
}

NukeXProgress::~NukeXProgress()
{
   {
      std::lock_guard<std::mutex> lock( watchdog_mtx_ );
      stop_watchdog_.store( true, std::memory_order_release );
   }
   watchdog_cv_.notify_all();
   if ( watchdog_.joinable() )
      watchdog_.join();
}

void NukeXProgress::emit_sideband( const char* event_kind, const std::string& payload )
{
   // stderr — reaches the shell via 2>&1 | tee even under --automation-mode.
   std::cerr << '[' << iso_timestamp_ms() << "] " << event_kind << ' '
             << payload << '\n';
   std::cerr.flush();

   // Append log — stable, can be `tail -f`'d from another terminal.
   if ( progress_log_path_.empty() ) return;
   std::ofstream ap( progress_log_path_, std::ios::app );
   if ( ap )
      ap << '[' << iso_timestamp_ms() << "] " << event_kind << ' '
         << payload << '\n';
}

void NukeXProgress::write_heartbeat_locked( const char* reason )
{
   // state_mtx_ must already be held by the caller.
   auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - start_time_ ).count();
   if ( heartbeat_path_.empty() ) return;
   std::ofstream hb( heartbeat_path_, std::ios::trunc );
   if ( hb )
      hb << "ts=" << iso_timestamp_ms() << '\n'
         << "reason=" << reason << '\n'
         << "phase=" << last_phase_name_ << '\n'
         << "step=" << last_step_ << '\n'
         << "total=" << last_total_ << '\n'
         << "detail=" << last_detail_ << '\n'
         << "elapsed_s=" << elapsed << '\n';
}

void NukeXProgress::watchdog_loop()
{
   std::unique_lock<std::mutex> lock( watchdog_mtx_ );
   while ( !stop_watchdog_.load( std::memory_order_acquire ) ) {
      watchdog_cv_.wait_for( lock, kHeartbeatInterval, [this]{
         return stop_watchdog_.load( std::memory_order_acquire );
      } );
      if ( stop_watchdog_.load( std::memory_order_acquire ) ) break;
      {
         std::lock_guard<std::mutex> s( state_mtx_ );
         write_heartbeat_locked( "tick" );
      }
   }
}

void NukeXProgress::begin_phase( const std::string& name, int total_steps )
{
   depth_++;
   stack_.push_back( { name, total_steps, 0 } );

   if ( depth_ == 1 )
   {
      // Outermost phase drives the progress bar
      console_.WriteLn( String( "<end><cbr>" ) );
      console_.WriteLn( String().Format( "=== %s (%d steps) ===",
                                         name.c_str(), total_steps ) );
      monitor_.SetCallback( &status_ );
      monitor_.Initialize( String( name.c_str() ), total_steps );
   }
   else
   {
      // Nested phase — console only
      String indent;
      for ( int i = 1; i < depth_; i++ ) indent += "  ";
      console_.WriteLn( indent + String( name.c_str() ) );
   }
   console_.Flush();

   {
      std::lock_guard<std::mutex> lock( state_mtx_ );
      last_phase_name_ = name;
      last_step_ = 0;
      last_total_ = total_steps;
      last_detail_.clear();
      write_heartbeat_locked( "begin_phase" );
   }
   emit_sideband( "PHASE_BEGIN", name + " (" + std::to_string( total_steps ) + " steps)" );
}

void NukeXProgress::advance( int steps, const std::string& detail )
{
   if ( stack_.empty() ) return;

   auto& phase = stack_.back();

   if ( !detail.empty() )
   {
      // <end><cbr> first, exactly as begin_phase() does.  StandardStatus
      // writes its percentage IN PLACE on the current line, so a detail
      // written without a leading break gets eaten mid-word:
      //   "frame 4: 200 stars, FWHM 3.  6%    frame 5: ..."
      console_.WriteLn( String::UTF8ToUTF16(
         nukex::console_detail_line( depth_, detail ).c_str() ) );
      console_.Flush();
   }

   if ( steps > 0 )
   {
      phase.current += steps;

      // Advance the outermost progress bar
      if ( depth_ == 1 )
      {
         for ( int i = 0; i < steps; i++ )
            ++monitor_;
      }
   }

   {
      std::lock_guard<std::mutex> lock( state_mtx_ );
      last_step_ = phase.current;
      last_total_ = phase.total;
      if ( !detail.empty() ) last_detail_ = detail;
      write_heartbeat_locked( "advance" );
   }
   if ( !detail.empty() || steps > 0 )
   {
      std::ostringstream os;
      os << phase.current << '/' << phase.total;
      if ( !detail.empty() ) os << " -- " << detail;
      emit_sideband( "PROGRESS", os.str() );
   }
}

void NukeXProgress::end_phase()
{
   if ( stack_.empty() ) return;

   const std::string ending = stack_.back().name;

   if ( depth_ == 1 )
   {
      // Close the progress bar
      monitor_.Complete();
   }

   stack_.pop_back();
   depth_--;

   {
      std::lock_guard<std::mutex> lock( state_mtx_ );
      last_detail_ = "phase ended";
      if ( !stack_.empty() ) {
         last_phase_name_ = stack_.back().name;
         last_step_  = stack_.back().current;
         last_total_ = stack_.back().total;
      }
      write_heartbeat_locked( "end_phase" );
   }
   emit_sideband( "PHASE_END", ending );
}

void NukeXProgress::message( const std::string& text )
{
   console_.WriteLn( String( text.c_str() ) );
   console_.Flush();

   {
      std::lock_guard<std::mutex> lock( state_mtx_ );
      last_detail_ = text;
      write_heartbeat_locked( "message" );
   }
   emit_sideband( "MESSAGE", text );
}

bool NukeXProgress::is_cancelled() const
{
   return monitor_.IsAborted();
}

} // namespace pcl
