// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessApply_h
#define PICopilot_ProcessApply_h

#include "ProcessSafety.h"   // PinnedParameter

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pcl
{

struct ApplyProcessResult
{
   bool           ok = false;
   String         error;                                     // precise, model-facing; empty when ok
   nlohmann::json parametersSet = nlohmann::json::object();  // id -> value exactly as applied (tables: the rows)
   nlohmann::json pinnedSet = nlohmann::json::object();      // pinned parameters PI Copilot set (ProcessSafety.h)
   double         elapsedMs = 0;                             // ExecuteOn() wall time
   String         processId;                                 // canonical id (Process::Id()), once resolved
   String         viewId;                                    // target FullId, once resolved
   // DETECT (Task T-hist): the process ran to completion and changed the
   // image, but PixInsight recorded no History step for it (ok == false,
   // error says so). The image IS modified; Undo cannot revert it.
   bool           unrecordedChange = false;
   // The process ran to completion and changed the image, but whether it was
   // recorded could not be checked (a History read was busy or failed; step
   // 7c). ok == false with a distinct error; never reported as ok.
   bool           unverifiedChange = false;
   // When ok: what is known about undoing it, stated only as far as it was
   // VERIFIED (model-facing): a checked History step on a main view, an
   // unverifiable preview step, or no History step on the target at all
   // (IsHistoryUpdater false: createNewImage, display-only processes, ...).
   String         undo;
};

/*
 * Runs one process on one view, the apply_process tool's engine:
 *   1. resolve the process (unknown id -> error),
 *   2. refuse processes that cannot run on views (use RunGlobalProcess),
 *   3. refuse a null or BUSY view (non-blocking CanRead()/CanWrite() probe),
 *   4. start from the process's DEFAULT instance and set only the given
 *      parameters: parameters {id: value} for scalars, tableParameters
 *      {id: [[row values in TableColumns() order], ...]} replacing whole tables.
 *      EVERYTHING is checked before the core sees it, because some core
 *      rejections are uncatchable MODAL dialogs (bad row index, bad table
 *      length): Boolean <- bool; numbers range-checked against
 *      GetNumericRange and the storage type, integral for integer types;
 *      String length limits and declared characters (StringParameterRules.h;
 *      view identifiers must be valid PixInsight identifiers); Enumeration <- element id
 *      / alias string or element value integer (ids via EnumerationInfoOf(),
 *      the same source describe_process uses); table row shape and row count
 *      against the table's length limits; no string may contain NUL; two keys
 *      naming the same parameter (id + alias) are refused; a string value
 *      must be allowed by its policy parameterValues rule, if any. Scalars are set at
 *      row 0. Pinned parameters (ProcessSafety.h) are refused from the model
 *      and set LAST from their trusted source: `pinned` when given (the
 *      values resolved ONCE before the dialog -- checked for completeness,
 *      never re-read), else resolved here. Every value is READ BACK and
 *      must match,
 *   5. Validate(whyNot), then CanExecuteOn(view, whyNot),
 *   6. ExecuteOn(view) with swap data (undoable, recorded in History); the
 *      busy probe is repeated right before it. A failure found only while
 *      running (e.g. a PixelMath syntax error) is ExecuteOn() == false,
 *   7. DETECT (Task T-hist): on a MAIN view, when the instance says it
 *      updates history (IsHistoryUpdater), the step must be verifiably in
 *      History. A process run while PixInsight is still executing another
 *      one changes the image but records nothing (measured: History +0 and
 *      ModifyCount +0 in every such apply). Signals, in order:
 *        a. ModifyCount advanced -> recorded (every pixel/keyword/geometry/
 *           ICC step measured: PixelMath incl. a no-op $T, FITSHeader, Crop,
 *           Resample, AssignICCProfile, Invert);
 *        b. it did not -> the History list decides (ReadViewHistory before
 *           and after): a NEW active step of this process -> recorded
 *           (measured: ImageIdentifier and RGBWorkingSpace record a step with
 *           ModifyCount +0); none -> NOT recorded;
 *        c. a history read busy (EvalGuard) or failed -> ok=false,
 *           unverifiedChange=true, a DISTINCT error saying it ran and changed
 *           the image but could NOT be verified, and why (never guessed).
 *      A PREVIEW never moves ModifyCount; its History list decides
 *      (CheckNewPreviewStep: a preview holds ONE step that each new step
 *      replaces -- measured, round 2; the main image is never changed).
 *      Not recorded: ok=false, unrecordedChange=true and the error says the
 *      image was changed outside History. `undo` states only what was
 *      verified.
 * Every failure is ok=false + a message naming the process/parameter and the
 * fix; nothing after the failing step runs, so a failure never touches the
 * image -- except step 7, which is reported as exactly that. Root thread only.
 * Never throws.
 */
ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view,
                                 const std::vector<PinnedParameter>* pinned = nullptr );

// apply_process's checks before anything is asked or run: known id, can run
// on views, then a dry run of the parameter setting on a throwaway DEFAULT
// instance (duplicate keys, unknown ids, types, ranges, enumerations, table
// shapes, parameterValues rules, pinned keys, read-back) -- exactly the checks
// ApplyProcess makes, with the same messages. Validate()/CanExecuteOn() are
// NOT part of it (they need the real target). For a denied/confirmAlways
// process (NoInstanceBeforeApproval) NO instance is built: only the metadata
// checks run (not enumeration values, not read-back); ApplyProcess makes the
// rest after the user approved. "" when fine. Root thread. Never throws.
String PrecheckApplyRun( const IsoString& processId, const nlohmann::json& parameters,
                         const nlohmann::json& tableParameters, const std::vector<PinnedParameter>* pinned = nullptr );

constexpr size_type PICopilotMaxDescribedWindows = 4;    // windows described in detail in one tool_result
constexpr size_type PICopilotMinIntegrationFrames = 3;   // documentation only: the fileTables policy is authoritative

struct GlobalRunResult
{
   bool                     ok = false;
   String                   error;                                     // precise, model-facing
   nlohmann::json           parametersSet = nlohmann::json::object();
   nlohmann::json           pinnedSet = nlohmann::json::object();      // pinned parameters PI Copilot set
   double                   elapsedMs = 0;                             // ExecuteGlobal() wall time
   String                   processId;                                 // canonical id, once resolved
   std::vector<std::string> createdWindows;                            // result windows (see SplitNewWindows)
   std::vector<std::string> otherNewWindows;                           // other windows that opened during the run
   nlohmann::json           outputIds = nlohmann::json::object();      // read-only "...ImageId" outputs, non-empty only
};

// Attribution of the windows that opened during a global run. When the
// process names its outputs (non-empty outputIds values), only the new windows
// it names are results; any other new window (e.g. one a user or script
// opened meanwhile) goes to `others`. With no named outputs, every new window
// is a result. Order of newWindows is kept.
void SplitNewWindows( const std::vector<std::string>& newWindows, const nlohmann::json& outputIds,
                      std::vector<std::string>& results, std::vector<std::string>& others );

// Checks before anything is asked or run: known id, global-capable, file paths
// (ValidateProcessFilePaths: the policy's fileTables), then a dry run of
// the parameter setting on a throwaway DEFAULT instance (shape, enumeration,
// range, read-back) -- metadata-only, with no instance, for a
// denied/confirmAlways process (as PrecheckApplyRun). "" when fine. Root thread.
String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters,
                          const nlohmann::json& tableParameters, const std::vector<PinnedParameter>* pinned = nullptr );

// `pinned` as for ApplyProcess.
// PrecheckGlobalRun, then a DEFAULT instance with the checked parameters
// (SetParameters, exactly as ApplyProcess), Validate, CanExecuteGlobal,
// ExecuteGlobal. The windows it opened are found by diffing the open main
// views before and after (even on failure, so the model can mention them).
// Never modifies an open image; never throws. Root thread only.
GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters,
                                  const std::vector<PinnedParameter>* pinned = nullptr );

// "id = value" lines (tables as compact JSON), for the Guided confirm dialog.
// "(all parameters at their defaults)" when nothing is set. When the text
// would exceed maxChars, whole lines are dropped from the end and replaced by
// "… and N more parameter(s) not shown" (N exact).
String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars );

// Self-test only: called with the process id and a stage name ("precheckApply",
// "precheckGlobal", "apply", "global") right BEFORE the tool path builds a
// ProcessInstance -- the instrumentation that proves no instance of a
// confirmAlways/denied process is built before the user approved. An empty
// function removes the observer. Root thread.
using InstanceBuildObserver = std::function<void( const IsoString& processId, const char* stage )>;
void SetInstanceBuildObserverForSelfTest( InstanceBuildObserver observer );

// Self-test only. Inside the self-test's own PICopilot.executeGlobal() NO
// process is ever recorded in History (harness fact, plan Task 1), so every
// in-process ApplyProcess would trip step 7. While `on`, step 7 counts such
// an apply instead of failing it (InProcessUnrecordedAppliesForSelfTest).
// Set ONLY by the self-test runner (PICOPILOT_SELFTEST_OUT), never in a
// shipped install; the DETECT tests switch it off. Root thread.
void SetInProcessAppliesExpectedForSelfTest( bool on );
bool InProcessAppliesExpectedForSelfTest();
int  InProcessUnrecordedAppliesForSelfTest();

} // namespace pcl

#endif // PICopilot_ProcessApply_h
