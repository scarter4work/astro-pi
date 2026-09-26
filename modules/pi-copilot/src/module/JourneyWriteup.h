// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyWriteup_h
#define PICopilot_JourneyWriteup_h

#include "AnthropicClient.h"
#include "ChatThread.h"
#include "JourneyExport.h"

#include <pcl/StringList.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pcl
{

// Haiku's only role in PI Copilot (spec D9). Fixed and internal: never in
// the chat model catalog, never user-selectable (Global Constraints).
constexpr const char* PICopilotJourneyWriteupModel      = "claude-haiku-4-5";
constexpr int         PICopilotJourneyWriteupMaxTokens  = 8000;
constexpr size_t      PICopilotJourneyWriteupInputChars = 120000;
constexpr size_t      PICopilotJourneyWriteupParamChars = 300;

// The recipe cut down for the write-up (Ruling 11): no XPSM, no pixels, no
// paths (the recipe already holds file names only), parameters as JSON text
// cut to PICopilotJourneyWriteupParamChars (+ "..."), steps dropped from the
// END until the text fits PICopilotJourneyWriteupInputChars (counted in
// "omittedSteps"). Pure function, any thread.
std::string CondensedRecipeForWriteup( const nlohmann::json& recipe );

String JourneyWriteupSystemPrompt();
Array<AnthropicMessage> JourneyWriteupHistory( const nlohmann::json& recipe );   // one user text message, no image
RequestShape JourneyWriteupShape();                                              // non-streamed, 8000 tokens, no caching/thinking

struct WriteupReply
{
   bool                                       ok = false;
   std::string                                markdown;   // the reply without the trailing json fence
   std::vector<std::pair<int64, std::string>> inferred;   // step id -> inferred reason (well-formed entries only)
   std::vector<std::string>                   rejected;   // "entry N (why)": malformed entries, never stored
   std::string                                note;       // why no reasons were recorded ("" = the block parsed)
};

// The LAST ```json fence holds {"inferredReasons":[{step, reason}]}. A missing
// or unparseable block keeps the markdown and sets `note`. A block that opens
// but never closes (the reply was cut off inside it) is cut from the markdown
// -- the document never ends in an open fence or raw JSON -- and `note` says
// the reasons were cut off. An entry that is not {"step": <whole number>,
// "reason": <non-empty string>} goes to `rejected`, named by its 1-based position.
WriteupReply ParseWriteupReply( const std::string& text );

struct KeepOutcome
{
   bool              marked = false;        // this call marked the journey kept
   bool              alreadyKept = false;   // it was kept before this call
   KeeperFilesResult files;
   bool              writeupStarted = false;
   String            writeupError;          // why no write-up was started (e.g. no API key)
   // copyDone: the export copy was made now (a started write-up copies when it ends).
   // copyError: why not -- OR, with copyDone true, that the copy was made but its
   // location could not be remembered (the .copied-to marker write failed, so a
   // later Retry copies again). The two are NOT mutually exclusive.
   bool              copyDone = false;
   String            copyError;
   String            copiedTo;
   StringList        redone;                // Retry: the outputs re-run ("<name>.xpsm", "recipe.json", "thumbs", "journey.md", "export copy")
};

class JourneyWriteupJob;

/*
 * Keeper outputs (spec §6): mark kept, <name>.xpsm + recipe.json (Task 8),
 * journey.md by Haiku (the POST off the root thread, in a ChatThread), then
 * the export copy -- after the write-up when one runs, so the copy includes
 * it. Each output is independent and every failure is named.
 * ROOT THREAD ONLY: every store call, the ChatThread's construction and
 * destruction, and the file writes happen here; the worker only POSTs.
 * The API key never appears in a note, an error or an outcome.
 */
class KeeperExporter
{
public:

   explicit KeeperExporter( JourneyStore* store );
   ~KeeperExporter();   // cancels and waits for any write-up still in flight

   KeeperExporter( const KeeperExporter& ) = delete;
   KeeperExporter& operator =( const KeeperExporter& ) = delete;

   // A pending write-up finishes against the store it was started with only
   // while that store is still set: SetStore() with another store (or nullptr)
   // cancels the pending write-ups first.
   void SetStore( JourneyStore* store );

   // After the user confirmed the summary.
   KeepOutcome Keep( int64 journeyId, int64 endImageId, const String& apiKey, const String& exportFolder,
                     const String& url = PICOPILOT_MESSAGES_URL );

   // mark_journey_best on an already-kept journey: re-runs only the outputs
   // that are missing (and the copy, when an export folder is set and the last
   // copy is gone or anything was redone).
   KeepOutcome Retry( int64 journeyId, const String& apiKey, const String& exportFolder,
                      const String& url = PICOPILOT_MESSAGES_URL );

   // Finishes write-ups whose request completed; appends user-facing notes.
   // The reply is untrusted: an inferred reason is stored only on a step that
   // exists, belongs to the written journey, was made by the user and has no
   // stated reason. Every other entry stores nothing and is named in the note.
   void Poll( StringList& notes );
   bool Busy() const;

private:

   JourneyStore*                                   m_store = nullptr;
   std::vector<std::unique_ptr<JourneyWriteupJob>> m_jobs;

   bool StartWriteup( int64 journeyId, const String& apiKey, const String& exportFolder, const String& url, KeepOutcome& o );
   void CopyNow( int64 journeyId, const String& exportFolder, KeepOutcome& o );
   void Finish( JourneyWriteupJob& job, StringList& notes );
};

} // namespace pcl

#endif // PICopilot_JourneyWriteup_h
