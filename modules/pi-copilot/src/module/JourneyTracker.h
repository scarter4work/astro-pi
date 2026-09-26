// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyTracker_h
#define PICopilot_JourneyTracker_h

#include "HistoryReader.h"
#include "JourneyStore.h"

#include <pcl/Control.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/StringList.h>
#include <pcl/Timer.h>
#include <pcl/View.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pcl
{

double JourneyWallNow();   // Unix epoch seconds (the clock of PI's <time start>)
std::string LocalDateToday();   // "YYYY-MM-DD", local time

enum class RecordingState { Off, NotTracked, Recording, Paused };

struct JourneyStatus
{
   RecordingState state = RecordingState::NotTracked;
   String         reason;      // Paused: why (exact path / SQLite message / read error)
   String         note;        // informational, not a pause: e.g. "2 unrecorded step ranges wait to be written as gaps"
   int64          journeyId = 0;
   int64          imageId = 0;
   std::string    name, target, kind;   // kind = StripKind()
   int            activeSteps = 0;      // base steps excluded
   std::string    why;                  // master evidence (Ruling 1) or the link evidence that joined it
                                        // (memory only: shown in the strip tooltip and the join note, not persisted)
};

using HistoryReadFn = std::function<HistorySnapshot( const IsoString&, int )>;

/*
 * Decides which images belong to a journey and records their steps. Root
 * thread only. Notification handlers only append to m_events (they never touch
 * m_tracked / m_candidates / m_ignored, which Tick() iterates). Tick() first
 * drops closed windows (DropClosed: a View of a closed window must never be
 * copied -- PCL's attach throws), then drains m_events, then samples
 * ActiveWindow(), scans and does all reading and writing. It never waits on a
 * busy view (deferred), never nests an EvaluateScript (it skips the whole tick
 * while a run_pjsr script runs or EvaluateScriptDepth() > 0), and is
 * re-entrancy guarded (processes pump events). Every JourneyStore call it
 * makes is on the root thread (Timer ticks and notifications are root-thread).
 */
class JourneyTracker
{
public:

   explicit JourneyTracker( JourneyStore* store, const String& storeError = String() );

   void SetStore( JourneyStore* store, const String& storeError );
   void SetEnabled( bool on );
   bool Enabled() const { return m_enabled; }

   void OnImageCreated( const View& view, double now );
   void OnImageUpdated( const View& view, double now );
   void OnImageRenamed( const View& view, double now );
   void OnImageDeleted( const View& view, double now );
   void OnImageSaved( const View& view, double now );
   void OnImageFocused( const View& view, double now );   // also sampled from ActiveWindow() at every tick

   // A Copilot tool ran processId on viewFullId (empty for a global run) and
   // created these windows. integration: a global integration run (Ruling 1.5).
   void NoteCopilotStep( const IsoString& viewFullId, const std::string& processId, const std::string& reason,
                         const std::vector<std::string>& createdWindowIds, bool integration, double now );

   // start_journey: tracks a main view as the master root of a NEW journey.
   int64 StartJourneyFor( const View& view, String& error, double now );

   void Tick( double now, bool forceScan = false );

   JourneyStatus StatusFor( const IsoString& viewFullId ) const;
   int64 ImageOfView( const IsoString& viewFullId ) const;
   int64 JourneyOfView( const IsoString& viewFullId ) const;

   // Test hooks.
   void SetHistoryReaderForSelfTest( HistoryReadFn fn );      // empty fn -> ReadViewHistory
   void SetScanUsesModifyCountForSelfTest( bool on ) { m_useModifyCount = on; }
   size_type PendingCount() const;
   double LastStepMs() const { return m_lastStepMs; }
   int Deferrals() const { return m_deferrals; }
   StringList TakeJoinNotes();
   // The last 100 candidate evaluations ("<t> r=<result> ticks=<n> <view> <evidence summary>"),
   // for self-test diagnostics.
   std::vector<std::string> RecentDecisionsForSelfTest() const { return { m_decisions.begin(), m_decisions.end() }; }

private:

   struct Tracked
   {
      View        view;
      std::string id;
      int64       imageId = 0;
      int64       journeyId = 0;
      size_type   modifyCount = 0;
      std::vector<int> lastCounts;   // {initialLength, length, historyIndex} (batch scan mode)
      bool        dirty = true;
      int         readFailures = 0;
      std::string why;
   };
   struct Candidate { View view; double firstSeen = 0; int ticks = 0; };
   struct Ignored   { View view; size_type modifyCount = 0; };
   struct CopilotNote { std::string viewId, processId, reason; double t = 0; };
   struct CreatedNote { std::string id, sourceViewId; bool integration = false, first = false; double t = 0; };
   // A notification, queued by a handler and applied by DrainEvents() at the start of Tick() (pre-flight P10).
   enum class EventKind { Created, Updated, Renamed, Deleted, Saved, Focused };
   struct PendingEvent { EventKind kind; View view; double t = 0; };
   struct RecentStep  { std::string identity; int64 imageId = 0, journeyId = 0, stepId = 0; double start = -1, end = -1; };

   JourneyStore*            m_store = nullptr;
   String                   m_storeError;
   bool                     m_enabled = true;
   bool                     m_inTick = false;
   bool                     m_useModifyCount;
   bool                     m_forceScan = true;
   std::deque<PendingEvent> m_events;          // queued notifications (handlers only append)
   double                   m_lastScan = -1e300;
   std::vector<Tracked>     m_tracked;
   std::vector<Candidate>   m_candidates;
   std::vector<Ignored>     m_ignored;
   std::vector<CopilotNote> m_copilot;
   std::vector<CreatedNote> m_created;
   std::deque<RecentStep>   m_recent;          // last 200 recorded steps (timing evidence)
   std::deque<std::pair<std::string, double>> m_active;   // active main view id changes (timing evidence (b))
   std::vector<GapRow>      m_pendingGaps;     // gaps the DB could not take yet
   std::vector<int64>       m_closed;          // journeys whose last open image may have closed
   String                   m_pausedReason;       // history read / DB failure (spec §8 row 1)
   String                   m_statsPausedReason;  // "statistics not recorded: …"; cleared by the next successful stats (P11)
   StringList               m_joinNotes;
   HistoryReadFn            m_read;
   double                   m_lastStepMs = 0;
   int                      m_deferrals = 0;
   std::string              m_evalNote;        // what the current EvaluateCandidate() saw (diagnostics)
   std::deque<std::string>  m_decisions;       // RecentDecisionsForSelfTest()

   Tracked*       FindTracked( const View& v );
   const Tracked* FindTrackedById( const std::string& id ) const;
   bool           IsCandidate( const View& v ) const;
   void           AddCandidate( const View& v, double now );
   void           QueueEvent( EventKind kind, const View& v, double now );
   void           DrainEvents();
   void           ApplyEvent( const PendingEvent& e, const View& view );
   // Drops every tracked/candidate/ignored entry whose window is no longer
   // open (their journeys go to m_closed); fills the open main views and their
   // ModifyCount. Runs at every tick before anything copies a view.
   void           DropClosed( std::vector<View>& open, std::vector<size_type>& counts );
   void           Scan( double now, const std::vector<View>& open, const std::vector<size_type>& counts );
   void           BatchCounts();
   void           ProcessDirty( double now );
   void           ProcessCandidates( double now );
   int            EvaluateCandidate( Candidate& c, double now );   // 1 joined, 0 wait, -1 reject, 2 deferred (busy read)
   int64          JoinAsMaster( const View& v, const HistorySnapshot& snap, const FITSKeywordArray& kw,
                                const std::string& why );
   int64          JoinLinked( const View& v, const HistorySnapshot& snap, int64 fromImageId, int64 journeyId,
                              int64 viaStepId, const std::string& evidence );
   void           AddBaseAndSteps( int64 imageId, const HistorySnapshot& snap, int baseCount );
   void           StartingStats( const View& v, int64 journeyId, int64 imageId );
   void           Remember( const HistoryStep& h, int64 imageId, int64 journeyId, int64 stepId );
   void           NoteActive( const std::string& id, double now );
   bool           ConsumeCopilotNote( const std::string& viewId, const std::string& processId, double now, std::string& reason );
   std::vector<const Tracked*> ReferencedTracked( const HistoryStep& h ) const;
   void           FlushPendingGaps();
   void           EndClosedJourneys();
};

// Ruling 9: prunes when `today` differs from lastRun (then lastRun = today).
// Returns the number pruned, or -1 when it was not due.
int RunRetentionIfDue( JourneyStore& store, int days, const std::string& today, std::string& lastRun, StringList* removed );

class JourneyTimerHost;

/*
 * Module-level owner (one per PixInsight process): the library, the tracker,
 * its Timer (created with a bare Control receiver, so recording never needs
 * the panel) and user-facing notes the panel drains into the chat log.
 * Root thread only.
 */
class JourneyService
{
public:

   static JourneyService& Instance();
   static String LibraryRoot();

   void Start();
   void Stop();
   bool Started() const { return m_started; }

   JourneyStore* Store() { return m_store.get(); }
   const String& StoreError() const { return m_storeError; }
   JourneyTracker& Tracker() { return *m_tracker; }

   void ApplySettings();
   void OnTick();

   void OnImageCreated( const View& view );
   void OnImageUpdated( const View& view );
   void OnImageRenamed( const View& view );
   void OnImageDeleted( const View& view );
   void OnImageSaved( const View& view );
   void OnImageFocused( const View& view );

   void AddNote( const String& note );
   StringList TakeNotes();

   // Self-test: one last forced tick (so the pre-phase is fully recorded), then
   // no more ticks until re-enabled. Never used outside the harness.
   void FlushAndPauseForSelfTest();
   void SetEnabledForSelfTest( bool on ) { m_selfTestPaused = !on; }

private:

   JourneyService();
   ~JourneyService();

   bool                              m_started = false;
   bool                              m_selfTestPaused = false;
   std::unique_ptr<JourneyStore>     m_store;
   String                            m_storeError;
   double                            m_lastOpenAttempt = 0;
   std::unique_ptr<JourneyTracker>   m_tracker;
   std::unique_ptr<JourneyTimerHost> m_host;
   StringList                        m_notes;
   std::string                       m_retentionLastRun;

   void OpenStore();
};

} // namespace pcl

#endif // PICopilot_JourneyTracker_h
