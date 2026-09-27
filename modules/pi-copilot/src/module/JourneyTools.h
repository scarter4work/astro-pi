// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyTools_h
#define PICopilot_JourneyTools_h

#include "AgentTools.h"
#include "JourneyTracker.h"
#include "JourneyWriteup.h"

#include <functional>
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

// apply_process ran processId successfully on viewFullId (a main view): a
// replay looked up for that view names its journey "<keeper> (replay of #<id>)"
// now -- only when processId is the replay's next step and the lookup is at
// most 15 minutes old (review m6, re-review m4). "" or why the name could not
// be set. Pending lookups are kept per library + view inside JourneyTools, so
// they survive the panel rebuilding its JourneyToolHost. Root thread.
String NoteReplayStepApplied( JourneyToolHost& host, const IsoString& viewFullId, const std::string& processId );
// Forgets every pending replay lookup of host's library (call at each new user
// message, so a declined replay never names a journey later). Root thread.
void ForgetPendingReplays( JourneyToolHost& host );

// GC privacy (P6): text for the model with every absolute path reduced to its
// file name (a path may contain spaces). Pure; any thread.
String ModelTextWithoutDirectories( const String& text );

} // namespace pcl

#endif // PICopilot_JourneyTools_h
