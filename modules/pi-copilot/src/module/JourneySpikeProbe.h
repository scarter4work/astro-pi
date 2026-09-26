// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneySpikeProbe_h
#define PICopilot_JourneySpikeProbe_h

#include <pcl/View.h>

#include <nlohmann/json.hpp>

namespace pcl
{

// Test-only recorder for plan Task 1 (and kept as evidence for later runs).
// Armed ONLY when PICOPILOT_SELFTEST_OUT is set, i.e. under test/run-selftest.sh;
// in a shipped install every function below is a no-op. Root thread only.
void ArmJourneySpikeProbeIfSelfTest();                    // PICopilotModule::OnLoad()
void DisarmJourneySpikeProbe();                           // PICopilotModule::OnUnload()
bool JourneySpikeProbeArmed();
void JourneySpikeNote( const char* kind, const View& view ); // "created" | "updated" | "renamed" | "deleted" | "saved" | "focused"
nlohmann::json JourneySpikeProbeReport();                 // events, timer ticks, nested-eval results
void JourneySpikeProbeClearEvents();
// The timer's nested EvaluateScript can be paused (J0 pauses it after measuring
// whether it clobbers an outer EvaluateScript's result). Ticks still count.
void JourneySpikeProbeSetNestedEval( bool on );
// Timer-driven production apply (review fix #1): the first timer tick after
// `goFile` exists runs the real ApplyProcess( PixelMath $T*0.5 ) on `viewId`
// -- the panel's execution context (a module Timer, no process executing) --
// and records the result, read back by JourneySpikeProbeTimerApplyResult().
// The go file is written by the top-level script after the arming
// executeGlobal() returned (fix round 2). Test-only like the rest.
void JourneySpikeProbeRequestTimerApply( const std::string& viewId, const std::string& goFile );
nlohmann::json JourneySpikeProbeTimerApplyResult();       // null until the tick ran

// Task T-hist (test-only like the rest). The timer's period; the hazard repro
// runs it at 10 ms so a tick lands at the first moment PixInsight pumps
// events (it must ALREADY run at that period when a request is armed:
// restarting it delays the first tick past the window). 0.2 s otherwise.
void JourneySpikeProbeSetTickInterval( double seconds );
// On the first tick at least delayS after this call, run the production
// ApplyProcess on the view viewId (a full id) -- when `gated`, only once
// CurrentProcessActivity() (ProcessActivity.h, the tool loop's PREVENT gate)
// reports idle, counting the ticks it deferred. spec {process, parameters,
// tableParameters}; missing members default to PixelMath $T*0.5.
void JourneySpikeProbeRequestHazardApply( const std::string& viewId, double delayS, bool gated,
                                          const nlohmann::json& spec = nlohmann::json::object() );
nlohmann::json JourneySpikeProbeHazardApplyResult();      // null until applied

} // namespace pcl

#endif // PICopilot_JourneySpikeProbe_h
