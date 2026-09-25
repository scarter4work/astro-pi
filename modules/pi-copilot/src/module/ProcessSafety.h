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
// of a denied process is denied). A matching rule's reason ends with the
// triggering value: " (param = value)" or " (param = value, the process default)".
// reviewedSafe -> Allow. A process in NO section (a newer PixInsight, a
// third-party module) whose parameter/column ids match the side-effect
// heuristic -> Confirm ("it has not been reviewed and has file/output-like
// parameters: ..."). An unknown process id is Allow here (ApplyProcess /
// RunGlobalProcess then fail with their precise "unknown process" error); any
// other failure while checking fails CLOSED (Confirm, "this run could not be
// checked"). Root thread only. Never throws.
SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters );

// Installed processes with a parameter (or table column) id matching the
// side-effect heuristic -- case-insensitive substrings output|directory|dir$|
// file|path|overwrite|write|save|log|cache|delete|remove|close|script|command|
// url|exec|folder|destination|export|rename|database|server|program|launch, or
// the whole id words dir|dest|move|copy|db|host|app|application -- that are in
// none of deny / confirmAlways / confirmWhen / reviewedSafe. The same heuristic
// makes CheckProcessSafety() ask for such a process at runtime.
// [{process, parameters, canProcessViews, canProcessGlobal}]. Root thread.
nlohmann::json UnclassifiedSideEffectCandidates();

// Policy ids that are not installed processes under their canonical id, and
// confirmWhen parameters the process does not have (catches typos).
nlohmann::json UnknownPolicyProcessIds();

// Self-test only: evaluate against `policy` instead (nullptr restores).
void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy );

} // namespace pcl

#endif // PICopilot_ProcessSafety_h
