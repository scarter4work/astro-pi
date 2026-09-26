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
// "" when Copilot can replay the step. Otherwise why not, naming files by
// their FILE NAME only: a Script step, a Ruling 16 process, a parameter that
// names a file (the recipe keeps only its name, so the step cannot be replayed
// as recorded), a masked step, or a step that cannot be replayed (parseNote).
std::string ManualWhy( const StepRow& step );

// Any string that is an absolute path ("/..." but not a "/*" or "//" comment,
// "~" or "~/...", "X:\..." or "X:/...") and holds no line break becomes its file
// name (after the last '/' or '\'). Everything else -- e.g. the PixelMath
// expressions "~$T/2" and "/* note */ $T" -- is returned unchanged.
nlohmann::json PrivacyStripPaths( const nlohmann::json& v );

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
