// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyExport_h
#define PICopilot_JourneyExport_h

#include "JourneyStore.h"

#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

constexpr const char* PICopilotRecipeSchemaId = "picopilot-recipe";
constexpr int         PICopilotRecipeSchemaVersion = 1;

// What the user confirms before a journey becomes a keeper (spec §6, Ruling 17).
struct KeeperSummary
{
   int64                    journeyId = 0;
   std::string              name, target;
   bool                     alreadyKept = false;
   int                      masters = 0, images = 0, steps = 0, copilotSteps = 0;
   std::vector<std::string> masterLines;   // "ExpM42 (Ha, 20 x 300 s)"
   std::vector<std::string> linkLines;     // "pcExpM -> pcExpM_starless (linked by timing)"
   std::vector<std::string> gapLines;      // "after step 2 of pcExpM_starless: <reason>"
};

// All JourneyStore-reading functions below are ROOT THREAD ONLY (the store's rule).
KeeperSummary BuildKeeperSummary( JourneyStore& store, int64 journeyId );
String KeeperSummaryHtml( const KeeperSummary& s );   // MessageBox rich text, every value HTML-escaped

bool IsManualProcess( const std::string& processId );   // Ruling 16's process list

// True when a parameter (or table column) id names a file or folder location.
// PCL's parameter metadata has no file/path type, so it goes by the id --
// case-insensitive "file", "path", "directory" or "folder", or an id that ends
// in "dir" -- the convention PI's own processes follow (Script.filePath,
// ImageCalibration.masterBiasPath/outputDirectory, ImageIntegration's images
// table column "path"). PixelMath's parameters (expressions, symbols) and any
// "*expression*" id are never file parameters. Table columns are resolved to
// their ids through the INSTALLED process's metadata (root thread).
bool IsFileParameter( const std::string& processId, const std::string& parameterId );

// The file-holding parameters the id heuristic misses, as "Process/param" or
// "Process/table.column" (verified against PI's own scripts; see the .cpp).
const std::vector<std::string>& KnownFileParameterIds();

// Self-test only: while on, resolving a table's column ids fails as if the
// process catalog had thrown.
void SetJourneyExportCatalogFailForSelfTest( bool on );
// "" when Copilot can replay the step. Otherwise why not, naming files by
// their FILE NAME only: a Script step, a Ruling 16 process, a parameter value
// with a directory component (see PrivacyStripStepParameters: the recipe keeps
// only its name, so the step cannot be replayed as recorded), a masked step,
// or a step that cannot be replayed (parseNote). A bare file name in a file
// parameter stays replayable. ROOT THREAD ONLY (throws pcl::Error elsewhere);
// so are BuildRecipe and BuildJourneyXpsm, which use it.
std::string ManualWhy( const StepRow& step );

// Any string that is an absolute path ("/..." but not a "/*" or "//" comment,
// "~" or "~/...", "X:\..." or "X:/...") and holds no line break becomes its file
// name (after the last '/' or '\'). Everything else -- e.g. the PixelMath
// expressions "~$T/2" and "/* note */ $T" -- is returned unchanged.
nlohmann::json PrivacyStripPaths( const nlohmann::json& v );

// A step's parameters as exported: {"parameters": …, "tableParameters": …}
// with PrivacyStripPaths applied to every value, and additionally every value
// of a FILE parameter (IsFileParameter; table columns by the installed
// process's column ids) that has any directory component -- relative
// ("models/x.onnx") or a network share ("//server/x") -- reduced to its file
// name. A bare file name is kept. Such a step is manual (ManualWhy). Use this,
// not PrivacyStripPaths, for anything that came from a step.
// Fails CLOSED: a table whose column ids cannot be read from the catalog has
// every value treated as a file value (and ManualWhy says so). A process that
// is not installed is not a failure (its array rows get the generic check).
// ROOT THREAD ONLY: elsewhere it throws pcl::Error ("... root thread only").
nlohmann::json PrivacyStripStepParameters( const std::string& processId, const nlohmann::json& parameters,
                                           const nlohmann::json& tableParameters );

// recipe.json v1 (data/recipe-v1.schema.json): active, non-base steps only.
nlohmann::json BuildRecipe( JourneyStore& store, int64 journeyId, const std::string& generator );
// Every rule and type the schema declares, plus: image keys unique, and every
// image reference (links, steps, gaps, journey.endImage) names an image of the
// recipe. why = "steps[0].actor: …" on failure, "" on success.
bool ValidateRecipe( const nlohmann::json& recipe, std::string& why );
const char* RecipeSchemaText();

// The process icon set (Ruling 13): one ProcessContainer + icon per image.
// A Script step, a step that cannot be replayed and a step with a parameter
// that names a file are never emitted as instances; each is an XML comment
// naming the step (files by file name only). The text never holds a directory
// (pre-flight P5).
std::string BuildJourneyXpsm( JourneyStore& store, int64 journeyId );

String ExportDirOf( JourneyStore& store, int64 journeyId );   // <journey dir>/export
String ExportBaseName( const JourneyRow& j );                 // SafeFolderName( j.name )

struct KeeperFilesResult
{
   bool   xpsmOk = false;
   bool   recipeOk = false;
   bool   thumbsOk = false;
   String xpsmError;
   String recipeError;
   String thumbsError;
   String dir;
};

// Writes <name>.xpsm, recipe.json and recipe.schema.json (SafeFileMode::Shared)
// into ExportDirOf() (made by EnsurePrivateDirectory), and copies
// <journey dir>/thumbs/ to ExportDirOf()/thumbs/ so the "thumbs/<n>.jpg"
// references of recipe.json and journey.md resolve inside export/. Each output
// is independent (a failure is named, the others are still written). A recipe
// that does not validate is never written. Root thread. Never throws.
KeeperFilesResult WriteKeeperFiles( JourneyStore& store, int64 journeyId, const std::string& generator );

// Copies ExportDirOf() (which holds thumbs/) to <exportFolder>/<target>/<YYYY-MM-DD>-<name>/.
// exportFolder must be an absolute, EXISTING directory (never created; Ruling 18);
// the two folders below it are made as needed (never through a symbolic link),
// files are written SafeFileMode::Shared, and links in the source are skipped.
// "" on success (copiedTo set), else a message naming the path (copiedTo empty).
// Root thread. Never throws.
String CopyKeeperToExportFolder( JourneyStore& store, int64 journeyId, const String& exportFolder, String& copiedTo );

} // namespace pcl

#endif // PICopilot_JourneyExport_h
