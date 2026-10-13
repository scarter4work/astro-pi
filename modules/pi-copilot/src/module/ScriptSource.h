// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ScriptSource_h
#define PICopilot_ScriptSource_h

#include <pcl/StringList.h>   // StringKeyValueList
#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

/*
 * describe_script: what an installed PJSR script is, which parameters it
 * reads and how it behaves when PI Copilot starts it -- found by READING its
 * source as text. Nothing here executes, evaluates or parses the script with
 * a JavaScript engine: a small C++ scanner strips comments, follows the
 * script's own #include files inside the script tree, resolves simple
 * #define string/number constants and looks for the Parameters API calls.
 * What cannot be resolved that way is reported as "unresolved", never guessed.
 * Pure C++ (file reads only); any thread.
 */

constexpr size_t PICopilotScriptMaxFiles      = 256;                // main file + includes
constexpr size_t PICopilotScriptMaxFileBytes  = 4u << 20;           // per file
constexpr size_t PICopilotScriptMaxTotalBytes = 32u << 20;          // all files

struct ScriptSourceRef
{
   std::string file;   // relative to the script tree root
   int         line = 0;
};

struct ScriptParameter
{
   std::string                  name;
   std::vector<std::string>     getters;   // e.g. "has", "getReal" (distinct, in order of appearance)
   std::string                  type;      // "boolean" | "integer" | "unsigned" | "int64" | "uint64" | "real"
                                           // | "string" | "presence" (only has()) | "mixed" (typed getters disagree)
   std::string                  viaDefine; // the #define the key came from, if any
   bool                         hasDefault = false;
   nlohmann::json               defaultValue;
   ScriptSourceRef              defaultAt;
   std::string                  defaultNote;   // why no default, when there are candidate assignments
   std::vector<ScriptSourceRef> at;
};

struct ScriptUnresolvedRead
{
   std::string     getter;
   std::string     expression;   // the key expression as written (cut)
   ScriptSourceRef at;
};

struct ScriptDescription
{
   bool        ok = false;
   std::string error;

   // Identity (directives and #define constants of the main file).
   std::string featureId;     // the id part of #feature-id (empty when the directive has only a menu path)
   std::string menu;          // the menu path part ("Utilities > CorrectMagentaStars")
   std::string info;          // #feature-info, HTML tags removed, whitespace collapsed
   std::string title;         // #define TITLE
   std::string version;       // #define VERSION
   std::string scriptIdDirective;   // #script-id

   std::vector<std::string> files;   // read, relative to the tree root (main first)
   nlohmann::json           includesNotFollowed = nlohmann::json::array();   // [{include, from, reason}]
   bool                     limitHit = false;

   // Execution context evidence (Parameters.isViewTarget / targetView / isGlobalTarget).
   std::vector<ScriptSourceRef> viewTargetAt;
   std::vector<ScriptSourceRef> globalTargetAt;

   // Interaction evidence.
   std::vector<ScriptSourceRef> executeCallsAt;    // ".execute()" (Dialog.execute() is modal)
   std::vector<ScriptSourceRef> messageBoxesAt;    // "new MessageBox"
   std::vector<ScriptSourceRef> settingsReadAt;    // "Settings.read" (last-used values)

   std::vector<ScriptParameter>      parameters;
   std::vector<ScriptUnresolvedRead> unresolved;

   bool SupportsView() const   { return !viewTargetAt.empty(); }
   bool SupportsGlobal() const { return !globalTargetAt.empty(); }
};

// mainFile and treeRoot are real (symlink-free) paths; mainFile is inside
// treeRoot. Quoted #include files are followed only inside treeRoot; <...>
// system headers only inside systemIncludeDir (PixInsight's include
// directory; empty = not followed). Never throws: a failure is ok=false with
// error.
ScriptDescription DescribeScriptSource( const String& mainFile, const String& treeRoot,
                                        const String& systemIncludeDir = String() );

// The description for the model (and the self-test).
nlohmann::json ScriptDescriptionJson( const ScriptDescription& d );

// "honours-parameters-when-targeted" | "headless-when-targeted" | "always-interactive" | "no-dialog"
std::string ScriptInteraction( const ScriptDescription& d );

// Checks run_script arguments against the description: an object whose keys
// are parameters the script reads (exact names; anything else is refused with
// the list of valid ones) and whose values have the getter's type (JSON
// boolean / integer / number / string). Converts them to the strings the
// script's Parameters getters parse (true/false, decimal integers, numbers in
// the shortest round-trip form). "" when fine; else the model-correctable
// refusal. `shown` gets {key: value-as-passed} for the dialog and the log.
String ValidateScriptArguments( const ScriptDescription& d, const nlohmann::json& args,
                                StringKeyValueList& out, nlohmann::json& shown );

} // namespace pcl

#endif // PICopilot_ScriptSource_h
