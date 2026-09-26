// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneySpikeProbe.h"
#include "PICopilotModule.h"
#include "ProcessActivity.h"
#include "ProcessApply.h"
#include "Utf8.h"

#include <pcl/Control.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/ImageWindow.h>
#include <pcl/Timer.h>
#include <pcl/Variant.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

namespace pcl
{

namespace
{

class SpikeTimerHost : public Control
{
public:

   SpikeTimerHost()
   {
      T.SetInterval( 0.2 );
      T.SetPeriodic( true );
      T.OnTimer( (Timer::timer_event_handler)&SpikeTimerHost::e_Tick, *this );
   }

   Timer T;

   void e_Tick( Timer& );
};

struct SpikeState
{
   bool           armed = false;
   nlohmann::json events = nlohmann::json::array();   // [kind, fullId, seconds since arm]
   int            ticks = 0;
   bool           timerCreated = false;
   bool           nestedEvalOn = true;
   std::string    timerApplyViewId;                                // pending request, "" when none
   std::string    timerApplyGoFile;                                // applied only once this file exists
   int            timerApplyTicksWaited = 0;                       // ticks deferred for want of the go file
   nlohmann::json timerApplyResult;                                // null until run
   std::string    timerError;
   int            nestedEvalOk = 0;
   int            nestedEvalFail = 0;
   std::string    nestedEvalLastError;
   nlohmann::json nestedEvalSamples = nlohmann::json::array();   // [t, historyIndex, ModifyCount, active main view id]
   std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
   std::unique_ptr<SpikeTimerHost> host;
   // Task T-hist: the forced-tick "apply while PixInsight is still executing"
   // repro (JourneySpikeProbeRequestHazardApply).
   std::string    hazardViewId;                                    // pending request, "" when none
   double         hazardArmT = 0;
   double         hazardDelayS = 0;
   bool           hazardGated = false;
   int            hazardDeferredTicks = 0;
   std::string    hazardBusyReason;                                // first reason the gate deferred for
   nlohmann::json hazardSpec;                                      // {process, parameters, tableParameters}
   nlohmann::json hazardResult;                                    // null until applied
};

SpikeState& S()
{
   static SpikeState s;
   return s;
}

double Since()
{
   return std::chrono::duration<double>( std::chrono::steady_clock::now() - S().t0 ).count();
}

// The hazard apply (Task T-hist): the production ApplyProcess( PixelMath
// $T*0.5 ) on the first tick at least delayS after arming -- when gated, only
// once the production PREVENT gate (CurrentProcessActivity) says idle.
void HazardTick( SpikeState& s )
{
   if ( s.hazardViewId.empty() || Since() - s.hazardArmT < s.hazardDelayS )
      return;
   if ( s.hazardGated )
   {
      const ProcessActivityState a = CurrentProcessActivity();
      if ( a.busy )
      {
         if ( s.hazardDeferredTicks++ == 0 )
            s.hazardBusyReason = U8( a.reason );
         return;
      }
   }
   const std::string id = s.hazardViewId;
   s.hazardViewId.clear();
   nlohmann::json r = { { "viewId", id }, { "t", Since() - s.hazardArmT }, { "gated", s.hazardGated },
                        { "deferredTicks", s.hazardDeferredTicks }, { "busyReason", s.hazardBusyReason } };
   try
   {
      const View v = View::ViewById( IsoString( id.c_str() ) );
      if ( v.IsNull() )
         r["error"] = "no such view";
      else
      {
         const ImageWindow w = v.Window();
         r["modifyCountBefore"] = uint64_t( w.ModifyCount() );
         const ApplyProcessResult a = ApplyProcess(
            IsoString( s.hazardSpec.value( "process", std::string( "PixelMath" ) ).c_str() ),
            s.hazardSpec.value( "parameters", s.hazardSpec.contains( "process" ) ? nlohmann::json::object()
                                                                                : nlohmann::json{ { "expression", "$T*0.5" } } ),
            s.hazardSpec.value( "tableParameters", nlohmann::json::object() ), v );
         r["ok"] = a.ok;
         r["error"] = U8( a.error );
         r["unrecordedChange"] = a.unrecordedChange;
         r["undo"] = U8( a.undo );
         r["modifyCountAfter"] = uint64_t( w.ModifyCount() );
      }
   }
   catch ( const pcl::Exception& x ) { r["error"] = U8( x.Message() ); }
   catch ( ... )                     { r["error"] = "unknown exception"; }
   s.hazardResult = r;
}

void SpikeTimerHost::e_Tick( Timer& )
{
   SpikeState& s = S();
   ++s.ticks;
   HazardTick( s );
   // Apply only once the top-level script has written the go file, i.e. after
   // its executeGlobal() call fully returned: a tick can fire while PixInsight
   // is still finishing that process execution (after the module's own
   // ExecuteGlobal returned), and a step applied there changes the pixels
   // but is NOT recorded in History (measured, Task 1 fix round 2).
   if ( !s.timerApplyViewId.empty() && !File::Exists( String( s.timerApplyGoFile.c_str() ) ) )
      ++s.timerApplyTicksWaited;
   else if ( !s.timerApplyViewId.empty() )
   {
      const std::string id = s.timerApplyViewId;
      s.timerApplyViewId.clear();
      nlohmann::json r = { { "viewId", id }, { "t", Since() },
                           { "ticksWaited", s.timerApplyTicksWaited } };
      try
      {
         ImageWindow w = ImageWindow::WindowById( IsoString( id.c_str() ) );
         if ( w.IsNull() )
            r["error"] = "no such window";
         else
         {
            r["modifyCountBefore"] = uint64_t( w.ModifyCount() );
            const ApplyProcessResult a = ApplyProcess( "PixelMath", { { "expression", "$T*0.5" } },
                                                       nlohmann::json::object(), w.MainView() );
            r["ok"] = a.ok;
            r["error"] = U8( a.error );
            r["modifyCountAfter"] = uint64_t( w.ModifyCount() );
         }
      }
      catch ( const pcl::Exception& x ) { r["error"] = U8( x.Message() ); }
      catch ( ... )                     { r["error"] = "unknown exception"; }
      s.timerApplyResult = r;
   }
   if ( !s.nestedEvalOn )
      return;
   // While the pre-phase script runs it pumps events, so this is an
   // EvaluateScript NESTED inside a running user script: the case a user's own
   // long script (WBPP, a PJSR tool) creates for the tracker's timer.
   try
   {
      ImageWindow w = ImageWindow::WindowById( IsoString( "pcJourneyPre" ) );
      if ( w.IsNull() )
         return;
      const Variant v = ThePICopilotModule->EvaluateScript(
         "(function(){ var v = View.viewById( \"pcJourneyPre\" ); return v.isNull ? -1 : v.historyIndex; })()",
         "JavaScript" );
      ++s.nestedEvalOk;
      if ( s.nestedEvalSamples.size() < 60 )
      {
         const ImageWindow aw = ImageWindow::ActiveWindow();
         s.nestedEvalSamples.push_back( { Since(), v.ToInt(), uint64_t( w.ModifyCount() ),
                                          aw.IsNull() ? std::string() : std::string( aw.MainView().Id().c_str() ) } );
      }
   }
   catch ( const pcl::Exception& x )
   {
      ++s.nestedEvalFail;
      s.nestedEvalLastError = U8( x.Message() );
   }
   catch ( ... )
   {
      ++s.nestedEvalFail;
      s.nestedEvalLastError = "unknown exception";
   }
}

} // namespace

void ArmJourneySpikeProbeIfSelfTest()
{
   const char* out = std::getenv( "PICOPILOT_SELFTEST_OUT" );
   if ( out == nullptr || *out == '\0' )
      return;
   SpikeState& s = S();
   s.armed = true;
   s.t0 = std::chrono::steady_clock::now();
   try
   {
      s.host.reset( new SpikeTimerHost );
      s.host->T.Start();
      s.timerCreated = true;
   }
   catch ( const pcl::Exception& x )
   {
      s.timerError = U8( x.Message() );
   }
   catch ( ... )
   {
      s.timerError = "unknown exception";
   }
}

void DisarmJourneySpikeProbe()
{
   SpikeState& s = S();
   try
   {
      if ( s.host )
      {
         s.host->T.Stop();
         s.host.reset();
      }
   }
   catch ( ... )
   {
   }
   s.armed = false;
}

bool JourneySpikeProbeArmed()
{
   return S().armed;
}

void JourneySpikeNote( const char* kind, const View& view )
{
   SpikeState& s = S();
   if ( !s.armed )
      return;
   std::string id;
   try
   {
      id = std::string( view.FullId().c_str() );
   }
   catch ( ... )
   {
      id = "?";
   }
   if ( s.events.size() < 5000 )
      s.events.push_back( { kind, id, Since() } );
}

nlohmann::json JourneySpikeProbeReport()
{
   const SpikeState& s = S();
   return { { "armed", s.armed }, { "events", s.events }, { "ticks", s.ticks },
            { "timerCreated", s.timerCreated }, { "timerError", s.timerError },
            { "nestedEvalOk", s.nestedEvalOk }, { "nestedEvalFail", s.nestedEvalFail },
            { "nestedEvalLastError", s.nestedEvalLastError }, { "nestedEvalSamples", s.nestedEvalSamples } };
}

void JourneySpikeProbeClearEvents()
{
   S().events = nlohmann::json::array();
}

void JourneySpikeProbeSetNestedEval( bool on )
{
   S().nestedEvalOn = on;
}

void JourneySpikeProbeRequestTimerApply( const std::string& viewId, const std::string& goFile )
{
   S().timerApplyResult = nlohmann::json();
   S().timerApplyTicksWaited = 0;
   S().timerApplyGoFile = goFile;
   S().timerApplyViewId = viewId;
}

void JourneySpikeProbeSetTickInterval( double seconds )
{
   SpikeState& s = S();
   if ( s.host && s.host->T.Interval() != seconds )
   {
      s.host->T.Stop();
      s.host->T.SetInterval( seconds );
      s.host->T.Start();
   }
}

void JourneySpikeProbeRequestHazardApply( const std::string& viewId, double delayS, bool gated,
                                          const nlohmann::json& spec )
{
   SpikeState& s = S();
   s.hazardSpec = spec.is_object() ? spec : nlohmann::json::object();
   s.hazardResult = nlohmann::json();
   s.hazardDeferredTicks = 0;
   s.hazardBusyReason.clear();
   s.hazardArmT = Since();
   s.hazardDelayS = delayS;
   s.hazardGated = gated;
   s.hazardViewId = viewId;
}

nlohmann::json JourneySpikeProbeHazardApplyResult()
{
   return S().hazardResult;
}

nlohmann::json JourneySpikeProbeTimerApplyResult()
{
   return S().timerApplyResult;
}

} // namespace pcl
