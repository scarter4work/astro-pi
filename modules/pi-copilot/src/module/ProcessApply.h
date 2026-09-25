// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessApply_h
#define PICopilot_ProcessApply_h

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

struct ApplyProcessResult
{
   bool           ok = false;
   String         error;                                     // precise, model-facing; empty when ok
   nlohmann::json parametersSet = nlohmann::json::object();  // id -> value exactly as applied (tables: the rows)
   double         elapsedMs = 0;                             // ExecuteOn() wall time
   String         processId;                                 // canonical id (Process::Id()), once resolved
   String         viewId;                                    // target FullId, once resolved
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
 *      String length limits and allowed characters; Enumeration <- element id
 *      / alias string or element value integer (ids via EnumerationInfoOf(),
 *      the same source describe_process uses); table row shape and row count
 *      against the table's length limits; no string may contain NUL; two keys
 *      naming the same parameter (id + alias) are refused. Scalars are set at
 *      row 0. Every value is READ BACK and must match,
 *   5. Validate(whyNot), then CanExecuteOn(view, whyNot),
 *   6. ExecuteOn(view) with swap data (undoable, recorded in History); the
 *      busy probe is repeated right before it. A failure found only while
 *      running (e.g. a PixelMath syntax error) is ExecuteOn() == false.
 * Every failure is ok=false + a message naming the process/parameter and the
 * fix; nothing after the failing step runs, so a failure never touches the
 * image. Root thread only. Never throws.
 */
ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view );

constexpr size_type PICopilotMaxDescribedWindows = 4;    // windows described in detail in one tool_result
constexpr size_type PICopilotMinIntegrationFrames = 3;   // documentation only: the fileTables policy is authoritative

struct GlobalRunResult
{
   bool                     ok = false;
   String                   error;                                     // precise, model-facing
   nlohmann::json           parametersSet = nlohmann::json::object();
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
// range, read-back). "" when fine. Root thread.
String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters,
                          const nlohmann::json& tableParameters );

// PrecheckGlobalRun, then a DEFAULT instance with the checked parameters
// (SetParameters, exactly as ApplyProcess), Validate, CanExecuteGlobal,
// ExecuteGlobal. The windows it opened are found by diffing the open main
// views before and after (even on failure, so the model can mention them).
// Never modifies an open image; never throws. Root thread only.
GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters );

// "id = value" lines (tables as compact JSON), for the Guided confirm dialog.
// "(all parameters at their defaults)" when nothing is set. When the text
// would exceed maxChars, whole lines are dropped from the end and replaced by
// "… and N more parameter(s) not shown" (N exact).
String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars );

} // namespace pcl

#endif // PICopilot_ProcessApply_h
