// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_GlobalRunFiles_h
#define PICopilot_GlobalRunFiles_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

namespace pcl
{

/*
 * File-path rules for global runs (run_global_process), driven by a policy
 * "fileTables" section of the form
 *
 *   { "<ProcessId>": { "<tableId>": { "enabledColumn": "<columnId>",
 *                                     "minEnabledRows": N,
 *                                     "columns": { "<columnId>": "image" | "optionalFile", ... } } } }
 *
 * Declared table columns: "image" = absolute, existing, readable file that an
 * installed format can read; "optionalFile" = empty (or null), or an absolute
 * existing readable file. Symbolic links are FOLLOWED (a link to a frame is a
 * frame; a dangling one is "a broken symbolic link"); a path that cannot be
 * examined (e.g. EACCES on a parent directory) is "cannot be accessed (<why>)". Only enabled rows (enabledColumn) are checked, and
 * minEnabledRows is enforced; a declared table missing from tableParameters is
 * an error. Any other string value (scalar parameter or cell of an undeclared
 * table) that starts with '/' must exist (file or directory); one starting
 * with '~' is refused. Row SHAPE problems are left to SetParameters()'s
 * precise messages (see ProcessApply.cpp). Returns "" or the first problem,
 * naming <Process>.<table>[row].<column>. An unknown process id returns ""
 * (the executor reports it precisely); a policy entry naming a table or an
 * enabled column the process does not have is an "internal: ..." error, never
 * a silent pass.
 * Root thread (process introspection). Never throws.
 *
 * The policy section is a parameter so this unit has no opinion on where the
 * policy lives: ProcessSafety (Task 7) passes its compiled "fileTables".
 */
String ValidateGlobalRunFilePaths( const IsoString& processId, const nlohmann::json& fileTables,
                                   const nlohmann::json& parameters, const nlohmann::json& tableParameters );

// True when fileTables declares at least one file table for the process
// (resolved to its canonical id). Never throws.
bool DeclaresFileTables( const IsoString& processId, const nlohmann::json& fileTables );

} // namespace pcl

#endif // PICopilot_GlobalRunFiles_h
