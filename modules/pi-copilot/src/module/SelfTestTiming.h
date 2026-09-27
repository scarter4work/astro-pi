// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_SelfTestTiming_h
#define PICopilot_SelfTestTiming_h

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>

namespace pcl
{

// Self-test only: per-section wall-clock timings, so a slow run is
// self-diagnosing. Each section starts with SelfTestSectionMark( "<label>" ),
// which closes the section still open (if any) and opens the new one;
// SelfTestSectionMark( nullptr ) only closes. RunSelfTest() resets the list at
// its start and writes it to the result JSON as "sectionTimings"
// ([{ "section", "startS" (since the reset), "ms" }]); test/run-selftest.sh
// prints one line per entry. Every mark also rewrites
// $PICOPILOT_SELFTEST_SECTION_TIMINGS (a file in the harness's private 0700
// handoff dir) with { "done", "open" }, so a run that hangs or times out still
// names the section it was in. Root thread only (the self-test runs there).

struct SelfTestTimingState
{
   using clock = std::chrono::steady_clock;
   clock::time_point origin = clock::now();
   clock::time_point openSince;
   std::string       openLabel;
   bool              open = false;
   nlohmann::json    done = nlohmann::json::array();
};

inline SelfTestTimingState& SelfTestTiming()
{
   static SelfTestTimingState state;
   return state;
}

inline void SelfTestTimingReset()
{
   SelfTestTiming() = SelfTestTimingState();
}

inline void SelfTestSectionMark( const char* label )
{
   using clock = SelfTestTimingState::clock;
   SelfTestTimingState& s = SelfTestTiming();
   const clock::time_point now = clock::now();
   if ( s.open )
      s.done.push_back( {
         { "section", s.openLabel },
         { "startS", std::chrono::duration<double>( s.openSince - s.origin ).count() },
         { "ms", std::chrono::duration<double, std::milli>( now - s.openSince ).count() } } );
   s.open = label != nullptr;
   if ( s.open )
   {
      s.openLabel = label;
      s.openSince = now;
   }
   const char* path = std::getenv( "PICOPILOT_SELFTEST_SECTION_TIMINGS" );
   if ( path != nullptr && *path != '\0' )
   {
      nlohmann::json open = nullptr;
      if ( s.open )
         open = { { "section", s.openLabel },
                  { "startS", std::chrono::duration<double>( s.openSince - s.origin ).count() } };
      std::ofstream( path, std::ios::out | std::ios::trunc ) << nlohmann::json{ { "done", s.done }, { "open", open } }.dump();
   }
}

} // namespace pcl

#endif // PICopilot_SelfTestTiming_h
