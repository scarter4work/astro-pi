// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ScriptCatalog_h
#define PICopilot_ScriptCatalog_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

/*
 * Which installed scripts run_script may start (CPD rules 5.1/5.4: PI Copilot
 * never runs model-written code; it only asks PixInsight to run a script that
 * PixInsight itself installed and whose signature PixInsight verifies).
 *
 * A file is ELIGIBLE only if all of these hold:
 *  1. Location: its real path (symbolic links resolved) is inside
 *     <PixInsight>/src/scripts, the deployment directory of the PixInsight
 *     distribution and of update packages (Application/SrcDirectory). The path
 *     must be absolute, without "..", NUL, control or invisible characters; a
 *     link that leads outside is refused; directory links are not walked.
 *  2. A featured main script: a regular ".js" file with a #feature-id
 *     directive, not under Script > Development (code signing, signing keys,
 *     CPD submission, documentation compiler: never offered).
 *  3. Signed: a regular <name>.xsgn next to it, and PixInsight's own
 *     Security.getScriptSignature() reports it VALID -- the signature covers
 *     the preprocessed source, so editing the script or any file it #includes
 *     invalidates it (measured) -- by a developer in PixInsight's database of
 *     certified developers (Security.certifiedDevelopers(); a local test
 *     signing identity is not enough). The module checks this itself: a user
 *     may allow unsigned scripts in PixInsight's preferences, and that
 *     setting never widens what PI Copilot runs.
 *
 * PixInsight keeps no per-file record of what its distribution or the update
 * system deployed (etc/update/installed.xri lists packages, not files;
 * etc/security/protected lists bin/ files only), so "deployed by PixInsight or
 * an update" is enforced through (1) + (3): a file copied in by hand is
 * refused unless it carries a valid signature of a certified developer.
 *
 * The check runs on the root thread (the signature is verified by the core
 * through the module's own fixed script, ModuleScript.h; the path enters it
 * only as a data literal). Nothing here executes the script.
 */

struct ScriptEntry
{
   std::string id;              // the catalog id the model uses (scriptId, "@<dir>" added when two share one)
   std::string path;            // real path (UTF-8: no pcl::String in a std::vector element)

   String PathString() const;   // the same, as a pcl::String
   std::string rel;             // relative to the scripts root
   std::string scriptId;        // from the verified signature
   std::string featureId;       // #feature-id id part
   std::string menu;            // #feature-id menu path
   std::string developerId;
   std::string developerName;
   std::string signedAt;        // signature timestamp
};

struct ScriptRefusal
{
   std::string rel;
   std::string reason;
};

struct ScriptCatalog
{
   bool                       ok = false;
   String                     error;
   String                     root;   // real path of <PixInsight>/src/scripts
   std::vector<ScriptEntry>   eligible;
   std::vector<ScriptRefusal> refused;
   double                     ms = 0;
};

// realpath( Application/SrcDirectory + "/scripts" ); "" + error when it is
// not a directory. Root thread.
String ProductionScriptsRoot( String& error );

// PixInsight's include directory (Application/IncludeDirectory), for
// DescribeScriptSource's <...> headers. Root thread.
String PixInsightIncludeDirectory();

// Every featured .js under root, classified. useCache: signature results of
// unchanged files (same size and mtime of the script and its .xsgn) are
// reused within this session -- for listing only; run_script and
// describe_script re-verify the chosen file with CheckScriptEligibility().
// Root thread; never throws.
ScriptCatalog BuildScriptCatalog( const String& root, bool useCache );

// One file against the rules above for `root` (fresh signature check). true +
// entry (id = scriptId) when eligible; else false + the reason. Root thread;
// never throws.
bool CheckScriptEligibility( const String& path, const String& root, ScriptEntry& entry, String& reason );

// The entry with this catalog id; nullptr when none.
const ScriptEntry* FindScript( const ScriptCatalog& c, const std::string& id );

} // namespace pcl

#endif // PICopilot_ScriptCatalog_h
