// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_FileReferences_h
#define PICopilot_FileReferences_h

// Module-resolved file references (fix/replay-file-params).
//
// The model never sees a directory (GC privacy) and so can never pass a path
// it was only shown by name. Instead a FILE parameter (JourneyExport.h,
// IsFileParameter) can be given as a reference -- {"file": "<name>"} or, from
// a replay_journey page, {"recorded_file": "<name>", ...} -- and PI Copilot
// substitutes the full path of that exact file name from where the user
// already keeps it: the recorded step being replayed, the workspace's process
// icons, the process's default settings, other recorded journey steps. Only
// an existing, readable regular file (links followed) whose file name is
// exactly the one asked for counts; nothing is searched on disk.

#include "JourneyExport.h"   // FileParameterValue

#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

class JourneyStore;

// A place a file is known to be: its full path, and where PI Copilot found it,
// in words the model may see ("its recorded location", "workspace icon MGC1").
struct FileCandidate
{
   std::string path;
   std::string foundIn;
};

// path when it is absolute and names an existing, readable regular file
// (symbolic links followed); "" otherwise. Any thread.
std::string UsableFilePath( const std::string& path );

// The first candidate whose file name is exactly `name` and that is usable.
bool FindKnownFile( const std::string& name, const std::vector<FileCandidate>& candidates, FileCandidate& out );

// Appends one candidate per value (non-absolute values are skipped).
void AddFileCandidates( std::vector<FileCandidate>& to, const std::vector<FileParameterValue>& values,
                        const std::string& foundIn );

// The FILE-parameter values of the process's DEFAULT instance ("<P>'s default
// settings"). Root thread. Never throws (a failure adds nothing).
void AddDefaultInstanceFileCandidates( std::vector<FileCandidate>& to, const std::string& processId );

// The FILE-parameter values of every recorded step of the library (newest
// journeys first, at most 500 journeys): "a step of journey #<id>". Root
// thread (store). Never throws (a step whose values cannot be read is skipped).
void AddLibraryFileCandidates( std::vector<FileCandidate>& to, JourneyStore& store );

// {"<key>": name, "available": true/false[, "foundIn": ...]} -- what the model
// sees in place of a file value. key: "file" or "recorded_file".
nlohmann::json FileReferenceJson( const char* key, const std::string& name, bool available, const std::string& foundIn );

// A reference object ({"file": name} or {"recorded_file": name}): true + name.
bool IsFileReference( const nlohmann::json& v, std::string& name );

// One file value PI Copilot set in place of a reference or an omitted
// recorded file parameter.
struct FileSubstitution
{
   std::string where;     // "marsDatabaseFiles[0].path"
   std::string name;      // file name (what the model may see)
   std::string path;      // full path (chat log and dialog only)
   std::string foundIn;
};

// Resolves every file reference in parameters / tableParameters of processId
// (canonical id) in place. When `recorded` is given (a replay step of the
// same process, with its raw recorded tableParameters): every recorded file
// parameter the model OMITTED is set from the recording (a table: the whole
// recorded table, its file cells resolved), and a plain string the model gave
// for such a parameter must be a bare file name (resolved like a reference) --
// a value with a directory is refused. Outside a replay, plain strings are
// left as they are (the existing path rules apply). "" when fine; else the
// error for the model (file names only), and nothing may run. Root thread.
String SubstituteFileReferences( const std::string& processId, nlohmann::json& parameters, nlohmann::json& tableParameters,
                                 const std::vector<FileParameterValue>* recorded, const nlohmann::json& recordedTables,
                                 const std::vector<FileCandidate>& known, std::vector<FileSubstitution>& subs );

// " [marsDatabaseFiles[0].path = /full/x.xmars, the file from its recorded
// location, set by PI Copilot]" per substitution: for the chat log.
String FileSubstitutionLogText( const std::vector<FileSubstitution>& subs );

// text with every substituted full path replaced by its file name (and any
// other absolute path reduced by the generic scrubber): for the model.
String WithoutSubstitutedPaths( const String& text, const std::vector<FileSubstitution>& subs );

// Every string equal to a substituted full path becomes its file name.
nlohmann::json RedactSubstitutedPaths( const nlohmann::json& v, const std::vector<FileSubstitution>& subs );

// The canonical id of a process id or alias; "" when unknown. Root thread.
std::string CanonicalProcessIdOf( const std::string& processId );

} // namespace pcl

#endif // PICopilot_FileReferences_h
