// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessSafety_h
#define PICopilot_ProcessSafety_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

namespace pcl
{

// What PI Copilot may do with one process run (data/process-safety.json).
struct SafetyVerdict
{
   enum Kind { Allow, Confirm, Deny };
   Kind   kind = Allow;
   String reason;   // Confirm/Deny: plain words, shown to the user and the model
};

// The compiled policy (or the self-test override).
const nlohmann::json& CompiledProcessSafety();

// deny -> Deny; confirmAlways -> Confirm; confirmWhen -> Confirm when a
// rule's parameter has the rule's `equals` value (or, for a `notEquals` rule,
// any other value) -- the value given in `parameters`,
// else the process default. A given key is matched to the rule's parameter by
// the core's own resolution (so a parameter ALIAS id cannot slip past a rule),
// and enumerations compare by element id, also when the model passed the
// element's integer value or an element alias; several matching rules join
// their reasons with "; ". The process id is resolved the same way (an alias
// of a denied process is denied). An unknown process id is Allow here (ApplyProcess /
// RunGlobalProcess then fail with their precise "unknown process" error).
// Root thread only. Never throws.
SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters );

// Installed processes with a parameter (or table column) id matching the
// side-effect heuristic (output|directory|dir$|file|path|overwrite|write|save|
// log|cache|delete|remove|close|script|command|url|exec, case-insensitive) that
// are in none of deny / confirmAlways / confirmWhen / reviewedSafe.
// [{process, parameters, canProcessViews, canProcessGlobal}]. Root thread.
nlohmann::json UnclassifiedSideEffectCandidates();

// Policy ids that are not installed processes under their canonical id, and
// confirmWhen parameters the process does not have (catches typos).
nlohmann::json UnknownPolicyProcessIds();

// Self-test only: evaluate against `policy` instead (nullptr restores).
void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy );

} // namespace pcl

#endif // PICopilot_ProcessSafety_h
