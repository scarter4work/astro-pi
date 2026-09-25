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
// Timer-driven production apply (review fix #1): the next timer tick runs the
// real ApplyProcess( PixelMath $T*0.5 ) on `viewId` -- the panel's execution
// context (a module Timer, no process executing) -- and records the result,
// read back by JourneySpikeProbeTimerApplyResult(). Test-only like the rest.
void JourneySpikeProbeRequestTimerApply( const std::string& viewId );
nlohmann::json JourneySpikeProbeTimerApplyResult();       // null until the tick ran

} // namespace pcl

#endif // PICopilot_JourneySpikeProbe_h
