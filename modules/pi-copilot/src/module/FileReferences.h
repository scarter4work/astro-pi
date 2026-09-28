// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_FileReferences_h
#define PICopilot_FileReferences_h

// Module-resolved file references (fix/replay-file-params).
//
// The model never sees a directory (GC privacy) and so can never pass a path
// it was only shown by name. Instead an INPUT-file parameter (JourneyExport.h
// IsFileParameter, minus pinned and output/folder parameters: FileRoleOf) can
// be given as a reference -- {"file": "<name>"} or, from a replay_journey
// page, {"recorded_file": "<name>", ...} -- and PI Copilot substitutes the full
// path of that exact file name from where the user already keeps it, in this
// order: the recorded step being replayed, the workspace's process icons, the
// process's default settings, other recorded journey steps. Only an existing,
// readable regular file (links followed) whose file name is exactly the one
// asked for counts; nothing is searched on disk. Each source is read only when
// the ones before it did not have the file (review M3).

#include "JourneyExport.h"   // FileParameterValue

#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
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

// What a file parameter is for (review I2, M1).
//   Input  -- a file the process reads: resolvable.
//   Pinned -- set by PI Copilot from a trusted source (policy pinnedParameters,
//             e.g. GraXpert.appPath): never resolved, never shown as a file.
//   Output -- an output file or a folder (ids with output, overwrite, write,
//             save, destination, export, cache, directory, folder, or ending in
//             "dir"; or the whole word log(s) / report(s)): never resolved or
//             filled from a recording.
enum class FileRole { Input, Pinned, Output };
FileRole FileRoleOf( const std::string& processId, const FileParameterValue& v );
// The same for a parameter id or "table.column" (a table cell: the column id).
FileRole FileRoleOfId( const std::string& processId, const std::string& parameterId, const std::string& columnId );

// path when it is absolute and names an existing, readable regular file
// (symbolic links followed); "" otherwise. Any thread.
std::string UsableFilePath( const std::string& path );

// True when name is a plain file name: not empty, no '/', '\' or NUL, not
// "." or "..".
bool IsPlainFileName( const std::string& name );

// Appends one candidate per value (non-absolute values are skipped).
void AddFileCandidates( std::vector<FileCandidate>& to, const std::vector<FileParameterValue>& values,
                        const std::string& foundIn );

// The places after a step's recorded location, each built on first use and
// kept for this one tool call: the workspace icons (one PJSR script), a
// process's default settings (never for a process PI Copilot builds no
// instance of before approval: NoInstanceBeforeApproval, review I1), the
// library's recorded steps. Root thread. Never throws.
class FileSources
{
public:
   explicit FileSources( JourneyStore* store ) : m_store( store ) {}
   // First usable exact-name match: recorded, icons, processId's defaults, library.
   bool Find( const std::string& name, const std::vector<FileCandidate>& recorded, const std::string& processId,
              FileCandidate& out );
private:
   JourneyStore*                                     m_store;
   bool                                              m_haveIcons = false, m_haveLibrary = false;
   std::vector<FileCandidate>                        m_icons, m_library;
   std::map<std::string, std::vector<FileCandidate>> m_defaults;
};

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

using FileFinder = std::function<bool( const std::string& name, FileCandidate& out )>;

// Resolves every file reference in parameters / tableParameters of processId
// (canonical id) in place; a reference is accepted only in an INPUT file
// parameter. When `recorded` is given (the replay step's recorded INPUT file
// values, with its raw recorded tableParameters): every recorded file
// parameter the model OMITTED is set from the recording (a table: the whole
// recorded table, its file cells resolved), and a plain string the model gave
// for such a parameter must be a bare file name (resolved like a reference) --
// a value with a directory is refused. Outside a replay, plain strings are
// left as they are (the existing path rules apply). "" when fine; else the
// error for the model (file names only), and nothing may run. Root thread.
String SubstituteFileReferences( const std::string& processId, nlohmann::json& parameters, nlohmann::json& tableParameters,
                                 const std::vector<FileParameterValue>* recorded, const nlohmann::json& recordedTables,
                                 const FileFinder& find, std::vector<FileSubstitution>& subs );

// " [marsDatabaseFiles[0].path = /full/x.xmars, found in its recorded
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
