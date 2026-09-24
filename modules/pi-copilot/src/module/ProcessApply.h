// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessApply_h
#define PICopilot_ProcessApply_h

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

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
 *   2. refuse global-only processes (!CanProcessViews(); no ExecuteGlobal yet),
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
 *      against the table's length limits. Scalars are set at row 0. Every
 *      value is READ BACK and must match,
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

// "id = value" lines (tables as compact JSON), for the Guided confirm dialog.
// "(all parameters at their defaults)" when nothing is set. Cut to maxChars.
String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars );

} // namespace pcl

#endif // PICopilot_ProcessApply_h
