// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyTools_h
#define PICopilot_JourneyTools_h

#include "AgentTools.h"
#include "JourneyTracker.h"
#include "JourneyWriteup.h"

#include <functional>
#include <pcl/StringList.h>
#include <map>
#include <string>
#include <utility>

namespace pcl
{

// Everything the journey tools need; the panel fills it from JourneyService.
// Holds no View / ImageWindow / ProcessInstance: ids only.
struct JourneyToolHost
{
   JourneyStore*   store = nullptr;
   String          storeError;
   JourneyTracker* tracker = nullptr;
   KeeperExporter* keeper = nullptr;
   String          exportFolder;                                     // ⚙; empty = off
   std::function<String()> apiKey;                                   // the write-up's key
   std::function<bool( const String& summaryHtml )> confirmKeeper;   // Yes/No, default No (Ruling 17)
   // (Fix round 2) The replay names a replay_journey lookup leaves pending are
   // no longer held here: they live in JourneyTools itself (per library and
   // view), so a host rebuilt for every tool round keeps them (subsumes Task 11
   // 0703cd5d's RefreshJourneyHost carry-over, which must be dropped at merge).
};

extern const char* const kJourneyPromptRead;   // every mode (UTF-8)
extern const char* const kJourneyPromptAct;    // Copilot and Guided (UTF-8)

// list_journeys, get_journey, compare_to_journey, mark_journey_best (+ start_journey,
// replay_journey outside Advisor), in that order (Ruling 14).
nlohmann::json JourneyToolDefinitions( AgentMode mode );
bool IsJourneyTool( const std::string& name );

// Root thread. Never throws; every failure is isError with a precise message.
ToolOutcome ExecuteJourneyTool( const ToolCall& call, const ToolContext& ctx );

// The keep flow shared by mark_journey_best and the ★ button: summary ->
// confirm (skipped for an already-kept journey, which only retries outputs) ->
// KeeperExporter::Keep/Retry -> freeze (Ruling 26). endViewId: the end image
// (empty = the journey's newest image). Root thread; never inside an open
// JourneyStore::Transaction (refused with a precise error: the durable keep
// commits on its own). Never throws: a locked library or any other failure of
// the keep is ok=false with the reason in message/modelMessage.
struct KeepFlowResult
{
   bool        ok = false;       // the journey is kept (now, or it already was)
   bool        declined = false;
   int64       journeyId = 0;
   String      message;          // what happened, with full paths: the chat log and the ★ button only
   String      modelMessage;     // the same for the model: file names and "the journey's export folder", no directories (GC privacy)
   KeepOutcome outcome;
};

KeepFlowResult RunKeepFlow( JourneyToolHost& host, int64 journeyId, const IsoString& endViewId );

// The journey of a view (0 = not recorded): the tracker's (an image waiting to
// continue a kept journey: its continuation), else the newest recording
// journey that names it in the library.
int64 JourneyForView( JourneyToolHost& host, const IsoString& viewFullId );

// apply_process ran replay step n of journey keeperId successfully on
// viewFullId (a main view; validated by CheckReplayStep before the run): the
// view's journey is named "<keeper> (replay of #<id>)" now, if it is not yet,
// and the replay's clock restarts. Nothing else ever names a journey as a
// replay (no inference from process ids). "" or why the name could not be set.
// Pending lookups are kept per library + view inside JourneyTools, so they
// survive the panel rebuilding its JourneyToolHost. Root thread.
String NoteReplayStepApplied( JourneyToolHost& host, const IsoString& viewFullId, int64 keeperId, int64 n );
// Before an apply_process that carries replay_step {journey_id, n} (round 5, re-review 3 I1): "" when a
// replay_journey lookup of that journey for this view (within an hour of the last replay activity) returned n
// as a non-manual step; else the error for the model (nothing runs).
String CheckReplayStep( JourneyToolHost& host, const IsoString& viewFullId, int64 keeperId, int64 n );
// Forgets every pending replay lookup of host's library. Call it when the
// conversation is cleared / a new chat starts, and on a library switch -- NOT
// per user message: a replay that starts with a manual step (DBE) or waits for
// "go ahead" applies its first real step only after the user's next message
// (re-review round 2, I1). Between those, a pending name lapses only after an
// hour without replay activity on its view. Root thread.
void ForgetPendingReplays( JourneyToolHost& host );

// GC privacy (P6): text for the model with every absolute path reduced to its
// file name (a path may contain spaces). Pure; any thread.
String ModelTextWithoutDirectories( const String& text );
// The same, first reducing every path under one of knownDirs (the export
// folder, the library, $HOME, the temp directory, open images' folders) to its
// last component literally -- whatever characters its folders hold ("M42
// (Orion)", "Scott's Data", "Data, 2026"). Pure; any thread.
String ModelTextWithoutDirectories( const String& text, const StringList& knownDirs );
// The directories model text is scrubbed against: the export folder, the
// library, $HOME, the temp directory, and every open image's folder. Root thread.
StringList JourneyKnownDirs( const JourneyToolHost& host );
// Self-test only: the clock the pending replay names expire by (empty = JourneyWallNow).
void SetReplayNameClockForSelfTest( std::function<double()> clock );

} // namespace pcl

#endif // PICopilot_JourneyTools_h
