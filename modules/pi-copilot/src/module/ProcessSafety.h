// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessSafety_h
#define PICopilot_ProcessSafety_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

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

// How the process is about to run: on a view (apply_process) or in the global
// context (run_global_process). A global run of a process in NO section and
// not in "globalSafe" asks: a global run can change application-wide state
// (e.g. the default RGB working space), which no parameter name reveals.
enum class SafetyRunKind { OnView, Global };

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
// parameters: ..."); for run == Global, "globalConfirm" -> Confirm ("a global
// run: <reason>"), and a global-capable process in NO section that is not in
// "globalSafe"/"globalConfirm" -> Confirm ("it has not been reviewed for
// global runs, ..."); both unlisted reasons are joined with "; ". An unknown process id is Allow
// here (ApplyProcess / RunGlobalProcess then fail with their precise "unknown
// process" error); any other failure while checking fails CLOSED (Confirm,
// "this run could not be checked"). Root thread only. Never throws.
SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters,
                                  SafetyRunKind run = SafetyRunKind::OnView );

// The GLOBAL coverage gate: installed processes that can run in the global
// context (Process::CanProcessGlobal()) and are in none of deny /
// confirmAlways / confirmWhen / reviewedSafe / globalSafe / globalConfirm.
// [{process, canProcessViews}]. Deliberately does NOT probe
// CanExecuteGlobal(): a default ProcessContainer blocks there (measured).
// Root thread.
nlohmann::json UnreviewedGlobalProcesses();

// Installed processes with a parameter (or table column) id matching the
// side-effect heuristic -- case-insensitive substrings output|directory|dir$|
// file|path|overwrite|write|save|log|cache|delete|remove|close|script|command|
// url|exec|folder|destination|export|rename|database|server|program|launch, or
// the whole id words dir|dest|move|copy|db|host|app|application -- that are in
// none of deny / confirmAlways / confirmWhen / reviewedSafe. The same heuristic
// makes CheckProcessSafety() ask for such a process at runtime.
// [{process, parameters, canProcessViews, canProcessGlobal}]. Root thread.
nlohmann::json UnclassifiedSideEffectCandidates();

// Policy ids that are not installed processes under their canonical id,
// confirmWhen parameters the process does not have, globalSafe entries that
// are also in another section, fileTables tables/columns that do not exist
// (canonical ids), and pinnedParameters entries that are not canonical string
// parameters with a supported source/kind (catches typos).
nlohmann::json UnknownPolicyProcessIds();

// ---- Pinned parameters (policy "pinnedParameters") --------------------------
//
//   { "<Process>": { "<parameterId>": { "source": "globalSetting",
//                                       "key": "/<Module>/<settings key>",
//                                       "kind": "executable",
//                                       "from": "<where the value comes from, for messages>",
//                                       "setupHint": "<what the user does to set it>" } } }
//
// A pinned parameter's value is NEVER chosen by the model: PI Copilot fills it
// from a trusted source (the user's own setting for that module). Example:
// GraXpert.appPath names the program GraXpert launches, so it is pinned to
// the path the user set in GraXpert itself.
struct PinnedParameter
{
   std::string parameter;   // canonical parameter id
   String      value;       // the value to set (kind executable: the canonical realpath)
   String      from;        // e.g. "your GraXpert settings"
};

// Refuses (returns the message) when parameters or tableParameters contain ANY
// key that resolves (id or alias) to a pinned parameter, whatever its value:
// "<P>.<param> is set by PI Copilot from <from>; omit it". Else reads each
// pinned value (globalSetting: Settings::ReadGlobal(key), or the self-test
// reader) and checks it (kind executable: absolute, exists, a regular file
// after following links, executable; the value becomes its realpath). A
// missing/empty or invalid value is an error naming the value and telling the
// user what to do (setupHint) -- never a fallback to a default or PATH
// lookup. "" + out filled (possibly empty) when fine; an unknown process id
// is "" (the executor reports it). Root thread. Never throws.
String ResolvePinnedParameters( const IsoString& processId, const nlohmann::json& parameters,
                                const nlohmann::json& tableParameters, std::vector<PinnedParameter>& out );

// "appPath = /opt/x/GraXpert (set by PI Copilot from your GraXpert settings)"
// lines, for the confirm dialog and the tool log.
String DescribePinnedParameters( const std::vector<PinnedParameter>& pinned );

// Settings::ReadGlobal( key, value ), or the self-test reader when one is set.
bool ReadGlobalSetting( const IsoString& key, String& value );

// Self-test only: read "globalSetting" pinned values through `reader`
// (an empty function restores Settings::ReadGlobal).
using GlobalSettingReader = std::function<bool( const IsoString& key, String& value )>;
void SetGlobalSettingReaderForSelfTest( GlobalSettingReader reader );

// Self-test only: evaluate against `policy` instead (nullptr restores).
void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy );

// File-path rules for global runs: the policy's "fileTables" section applied
// by ValidateGlobalRunFilePaths() (GlobalRunFiles.h documents the rules).
// Returns "" or the first problem, naming <Process>.<table>[row].<column>.
// Root thread.
String ValidateProcessFilePaths( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters );

// True when the policy declares file tables for the process (it integrates
// files from disk; apply_process refers the model to run_global_process).
bool HasFileTables( const IsoString& processId );

} // namespace pcl

#endif // PICopilot_ProcessSafety_h
