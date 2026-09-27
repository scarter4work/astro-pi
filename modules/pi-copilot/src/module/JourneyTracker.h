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

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <set>
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
 * thread only; every JourneyStore call it makes is on the root thread (Timer
 * ticks, notifications and the tool loop all run there).
 *
 * Notification handlers only append to m_events. A tick:
 *   1. DropClosed(): forgets windows that are no longer open (their journeys
 *      may end); a window closed with unrecorded changes queues a gap (its
 *      only read: the image's last recorded seq);
 *   2. DrainEvents(): applies the queued notifications -- reads nothing;
 *   3. the busy gate (review I4, re-review R4): the rest of the tick is
 *      deferred while PixInsight is busy -- RecorderActivity() (the console
 *      abort enabled, or less than 0.3 s since the last image notification /
 *      process end), a run_pjsr script, another module EvaluateScript, or an
 *      open JourneyStore::Transaction. A LOCKED view defers only itself
 *      (per-view IsBusy), never the whole recorder. After 5 s of continuous
 *      deferral StatusFor() reports "waiting: <reason>". A tracked window
 *      closed with unrecorded changes (dirty) gets a gap row. Known blind
 *      spot: a user script that enables no abort and just pumps events is
 *      invisible, so a tick can still run (and read history) inside it;
 *   4. samples ActiveWindow(), scans, and does all reading and writing, with
 *      a soft per-tick time cap: after 100 ms no further image or candidate
 *      is STARTED (one already started finishes, so a tick can reach about
 *      200-250 ms with a 60 MP join; re-review m8); the rest continues on
 *      the next tick.
 * It never waits on a busy view, never nests an EvaluateScript, and is
 * re-entrancy guarded. It holds NO pcl::View / ImageWindow beyond the
 * notification or the tick that produced it (fix round 4, the proven SIGSEGV
 * cause): windows are ids + opaque handles, events are data, and a View is
 * re-resolved by id for each API call. Every join and every recorded batch of steps is ONE
 * JourneyStore::Transaction: a failure leaves no partial journey, image or
 * step rows, and the next tick retries from the database's state.
 *
 * Failure boundaries (fix round 3): every per-item unit of work has its own.
 * JourneyRowMissing (a missing row, or SQLITE_CONSTRAINT_FOREIGNKEY) drops /
 * cleans that item with one note and schedules Reconcile(); any other error
 * defers that item only and the tick goes on. Only a missing store, an open
 * transaction or an unexpected escape pauses the whole tick. The boundaries:
 * DrainEvents (per event), DropClosed (per closing image's seq read), Scan
 * (per view), FlushPendingGaps (per gap), ProcessDirty (per image, incl. its
 * gap flush and statistics; 3 consecutive write failures park the image until
 * its next change, like read failures), ProcessCandidates (per candidate, incl.
 * joins and their statistics; every error kind, repeated missing rows
 * included, is bounded by kCandidateTicks), EndClosedJourneys (per journey,
 * per owner clear), Reconcile (per row check, incl. remembered steps whose
 * image / step row vanished), Scan's batch counts (on failure every probed
 * image is marked dirty), and the service's retention pass (per journey
 * inside the prune; a 60 s back-off after a failed touch). Retried each tick
 * without a count, by design (lock-shaped, cheap): a gap FlushPendingGaps
 * keeps, and a closed journey / owner clear EndClosedJourneys re-queues.
 */
class JourneyTracker
{
public:

   explicit JourneyTracker( JourneyStore* store, const String& storeError = String() );
   ~JourneyTracker();

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
   // Contract for Task 10 (review M5): today the note is matched only to a step
   // recorded AFTER it is posted; a tick that records the step between the
   // execution and this call attributes it to the user. Task 10 should post the
   // note BEFORE executing and cancel it when the tool fails.
   void NoteCopilotStep( const IsoString& viewFullId, const std::string& processId, const std::string& reason,
                         const std::vector<std::string>& createdWindowIds, bool integration, double now );

   // start_journey: tracks a main view as the master root of a NEW journey
   // (or continues its own saved journey). Refused, with a precise error, when
   // recording is off, the library is unavailable, the view is busy or already
   // recorded, or a tick is running. Never leaves a partial journey behind.
   int64 StartJourneyFor( const View& view, String& error, double now );

   void Tick( double now, bool forceScan = false );

   JourneyStatus StatusFor( const IsoString& viewFullId ) const;
   int64 ImageOfView( const IsoString& viewFullId ) const;
   int64 JourneyOfView( const IsoString& viewFullId ) const;
   // The distinct journeys of the images this tracker records right now (valid
   // after a Tick, which dropped closed windows). Retention never prunes them.
   std::vector<int64> OpenJourneyIds() const;

   // Test hooks.
   void SetHistoryReaderForSelfTest( HistoryReadFn fn );      // empty fn -> ReadViewHistory
   void SetScanUsesModifyCountForSelfTest( bool on ) { m_useModifyCount = on; }
   // Called inside every join's transaction at its midpoint ("master" after
   // CreateJourney, "linked" after AddImage); a throw there must leave no rows.
   void SetJoinFaultForSelfTest( std::function<void( const char* where )> fn ) { m_joinFault = std::move( fn ); }
   size_type PendingCount() const;
   double LastStepMs() const { return m_lastStepMs; }
   int Deferrals() const { return m_deferrals; }
   StringList TakeJoinNotes();
   // The last 100 candidate evaluations and scan anomalies
   // ("<t> r=<result> ticks=<n> <view> <evidence summary>"), for self-test diagnostics.
   std::vector<std::string> RecentDecisionsForSelfTest() const { return { m_decisions.begin(), m_decisions.end() }; }
   // A candidate's / ignored window's first sighting (-1 when not a candidate or ignored).
   double FirstSeenForSelfTest( const IsoString& viewId ) const;
   // The tick-wide failure (m_pausedReason) the last tick ended with; "" = none. Only a failure no
   // per-item boundary could contain sets it (fix round 3).
   String TickFailureForSelfTest() const { return m_pausedReason; }
   size_type PendingGapsForSelfTest() const { return m_pendingGaps.size(); }

private:

   // Window identity as DATA (SIGSEGV root cause, fix round 4): the tracker never keeps a pcl::View or
   // ImageWindow past the callback or tick that produced it -- PCL does not null a held handle when the
   // core frees the view, the core reuses the address, and destroying the stale copy detaches whatever
   // object now lives there (proven: a running ProcessInstance freed -> SIGSEGV storm). A window is its
   // main view's id + its handle as an OPAQUE integer, compared only against handles read from live
   // windows in the same tick and never dereferenced; Views are re-resolved by id (View::ViewById) in
   // narrow scopes when an API call is needed.
   struct Tracked
   {
      const void* handle = nullptr;
      std::string id;
      int64       imageId = 0;
      int64       journeyId = 0;
      size_type   modifyCount = 0;
      std::vector<int> lastCounts;   // {initialLength, length, historyIndex} (batch scan mode)
      bool        dirty = true;
      int         readFailures = 0;
      std::string why;
      String      pausedReason;      // this image's history read failure (review M3)
      String      statsReason;       // "statistics not recorded: ..."; cleared by this image's next success (P11)
      bool        hasGaps = false;   // a read-failure gap was queued / written (re-review m2)
      int         writeFailures = 0; // consecutive failed DB writes (not a missing row); 3 -> wait for the next change (m-h)
   };
   struct Candidate
   {
      const void*     handle = nullptr;
      std::string     id;
      double          firstSeen = 0;
      int             ticks = 0;
      bool            fresh = false;     // a window that APPEARED (Created / first seen), not a re-queued one (review I3)
      bool            hasSnap = false;   // the last read, reused while ModifyCount is unchanged (review M8)
      size_type       modifyCount = 0;
      HistorySnapshot snap;
      int             failures = 0;      // evaluations that threw: ignored after kCandidateTicks
      int             rowMissing = 0;    // row-missing evaluations: the 1st only defers (Reconcile runs), repeats count as failures (I-A)
   };
   struct Ignored   { const void* handle = nullptr; std::string id; size_type modifyCount = 0; double firstSeen = 0; };   // N3
   struct CopilotNote { std::string viewId, processId, reason; double t = 0; };
   struct CreatedNote { std::string id, sourceViewId; bool integration = false, first = false; double t = 0; };
   // A notification, queued by a handler and applied by DrainEvents() at the start of Tick() (pre-flight P10).
   enum class EventKind { Created, Updated, Renamed, Deleted, Saved, Focused };
   // Everything an event needs, read inside the callback while the view is alive (ids and an opaque
   // handle only; no View is queued).
   struct PendingEvent { EventKind kind; const void* handle = nullptr; std::string id, mainId; bool preview = false; double t = 0;
                         uint64 seq = 0; };
   // A window's death (ImageDeleted of a main view), kept OUT of the droppable event queue (re-review
   // I-1r): recorded even while recording is off, never dropped on overflow, consumed exactly once by the
   // next DropClosed (Tick or StartJourneyFor). seq orders it against the queued events: only events queued
   // BEFORE the death belong to the dead window; later ones belong to a new window at the reused address.
   struct Death { uint64 seq = 0; const void* handle = nullptr; };
   // One open window, read at the start of a tick (data only).
   struct OpenWindow { const void* handle = nullptr; std::string id; size_type modifyCount = 0; };
   struct RecentStep  { std::string identity; int64 imageId = 0, journeyId = 0, stepId = 0; double start = -1, end = -1; };
   // A link a join writes in its transaction; viaSeq > 0: the via step is the
   // joining image's own step at that seq (reference evidence).
   struct PlannedLink { int64 fromImageId = 0, viaStepId = 0; int viaSeq = 0; };

   JourneyStore*            m_store = nullptr;
   String                   m_storeError;
   bool                     m_enabled = true;
   bool                     m_inTick = false;
   bool                     m_useModifyCount;
   bool                     m_forceScan = true;
   bool                     m_renameSinceScan = false;
   std::set<const void*>    m_renamedHandles;        // Renamed notifications since the last scan (opaque handles)
   std::vector<Death>       m_deaths;                // see Death
   uint64                   m_seq = 0;               // one counter for events and deaths
   bool                     m_identityLost = false;  // m_deaths overflowed: every entry is treated as closed once
   bool                     m_scannedOnce = false;   // views found by the first scan (after start / re-enable) existed before: not fresh
   double                   m_gateSince = 0;         // start of the current continuous deferral (0 = none, R4)
   String                   m_gateReason;
   bool                     m_gateNoted = false;     // the "waiting" note was given for the current stall
   std::string              m_owner;                 // JourneyOwnerOf() of this process (m6)
   std::vector<int64>       m_closedImages;          // images whose window closed: owner cleared in EndClosedJourneys
   bool                     m_reconcile = false;     // a row went missing this tick: Reconcile() at its end
   std::deque<PendingEvent> m_events;          // queued notifications (handlers only append)
   double                   m_lastScan = -1e300;
   std::vector<std::unique_ptr<Tracked>>   m_tracked;
   std::vector<std::unique_ptr<Candidate>> m_candidates;
   std::vector<std::unique_ptr<Ignored>>   m_ignored;
   std::vector<CopilotNote> m_copilot;
   std::vector<CreatedNote> m_created;
   std::deque<RecentStep>   m_recent;          // last 200 recorded steps (timing evidence)
   std::deque<std::pair<std::string, double>> m_active;   // active main view id changes (timing evidence (b))
   std::vector<GapRow>      m_pendingGaps;     // gaps the DB could not take yet
   std::vector<int64>       m_closed;          // journeys whose last open image may have closed
   String                   m_pausedReason;    // tick-wide failure (DB / exception); cleared by the next complete tick
   StringList               m_joinNotes;
   HistoryReadFn            m_read;
   std::function<void( const char* )> m_joinFault;
   double                   m_lastStepMs = 0;
   int                      m_deferrals = 0;
   std::chrono::steady_clock::time_point m_tickStart;
   std::string              m_evalNote;        // what the current EvaluateCandidate() saw (diagnostics)
   std::deque<std::string>  m_decisions;       // RecentDecisionsForSelfTest()

   Tracked*       FindTracked( const void* handle );
   const Tracked* FindTrackedById( const std::string& id ) const;
   bool           IsCandidate( const void* handle ) const;
   void           AddCandidate( const std::string& id, const void* handle, double now, bool fresh );
   void           Forget( const void* handle );    // drops the window's candidate / ignored entries
   bool           OverTickBudget() const;
   void           Decision( const std::string& line );
   void           QueueEvent( EventKind kind, const View& v, double now );
   void           DrainEvents();
   void           ApplyEvent( const PendingEvent& e );
   void           DropClosed( std::vector<OpenWindow>& open );
   void           Scan( double now, const std::vector<OpenWindow>& open );
   void           BatchCounts();
   void           ProcessDirty( double now );
   void           ProcessOne( Tracked& t, double now );   // one dirty image; throws its DB failures
   void           ProcessCandidates( double now );
   int            EvaluateCandidate( Candidate& c, double now );   // 1 joined, 0 wait, -1 reject, 2 deferred (busy)
   // firstSeen: when the tracker first saw the window (its Created notification,
   // or the scan). Base = initialProcessing + the processing steps that started
   // before it (re-review R3): steps made after the window appeared stay the
   // user's steps even when the busy gate deferred this join past them.
   int64          JoinAsMaster( const std::string& id, const void* handle, const HistorySnapshot& snap, const FITSKeywordArray& kw,
                                const std::string& why, double firstSeen );
   // Drops a tracked image whose journey row vanished (R2): one note, and the
   // window is re-seen as a candidate (not fresh). Index into m_tracked.
   void           DropMissing( size_t index, const String& why );
   // Fix round 3: after any JourneyRowMissing, re-checks every tracked image / journey row that still
   // exists; drops the vanished ones (DropMissing) and purges their remembered steps, Copilot notes,
   // queued gaps, closed-journey and owner work. Its own failure only defers it to the next tick.
   void           Reconcile();
   int64          JoinLinked( const std::string& id, const void* handle, const HistorySnapshot& snap, int64 journeyId,
                              const std::vector<PlannedLink>& links, const std::string& evidence, const std::string& why );
   std::vector<int64> AddBaseAndSteps( int64 imageId, const HistorySnapshot& snap, int baseCount );
   void           StartingStats( Tracked& t );
   void           Remember( const HistoryStep& h, int64 imageId, int64 journeyId, int64 stepId );
   void           NoteActive( const std::string& id, double now );
   int            FindCopilotNote( const std::string& viewId, const std::string& processId, double now,
                                   const std::vector<int>& taken ) const;
   std::vector<const Tracked*> ReferencedTracked( const HistoryStep& h ) const;
   void           FlushPendingGaps();
   void           EndClosedJourneys();
};

// Ruling 9: prunes when `today` differs from lastRun. lastRun = today is set
// BEFORE pruning (Task 7 review I1), so a failing prune (it throws, naming
// the path) is retried on the next date, never on every tick. Returns the
// number pruned, or -1 when it was not due.
int RunRetentionIfDue( JourneyStore& store, int days, const std::string& today, std::string& lastRun, StringList* removed,
                       const std::vector<int64>& excludeJourneyIds = std::vector<int64>() );

// Re-review R1: one retention pass of an instance. First (once per local date
// per instance, openTouchedDate) touches every journey this tracker has open,
// in one transaction, so no instance's cutoff (>= 1 day) ever reaches an open
// journey; then RunRetentionIfDue excluding them. Throws what they throw.
int RetentionPass( JourneyStore& store, const JourneyTracker& tracker, int days, const std::string& today,
                   std::string& openTouchedDate, std::string& lastRun, StringList* removed );

class JourneyTimerHost;

/*
 * Module-level owner (one per PixInsight process): the library, the tracker,
 * its Timer (created with a bare Control receiver, so recording never needs
 * the panel) and user-facing notes the panel drains into the chat log.
 * Root thread only. Stop() (OnUnload) destroys everything on the root thread;
 * if it never ran, the static destructor leaks instead of touching PCL.
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
   // Self-test: every notification is also given to this tracker (J6's own; nullptr = none).
   void SetNotificationForwardForSelfTest( JourneyTracker* t ) { m_forward = t; }
   // Self-test (re-review n-2): how many notifications of each kind reached the forward tracker, i.e. were
   // delivered by PixInsight through the interface's handlers (index: 0 created, 1 updated, 2 renamed,
   // 3 deleted, 4 saved, 5 focused).
   int ForwardedForSelfTest( int kind ) const { return (kind >= 0 && kind < 6) ? m_forwarded[kind] : -1; }

private:

   JourneyService();
   ~JourneyService();

   bool                              m_started = false;
   bool                              m_selfTestPaused = false;
   bool                              m_inOnTick = false;
   JourneyTracker*                   m_forward = nullptr;
   int                               m_forwarded[6] = {};
   std::unique_ptr<JourneyStore>     m_store;
   String                            m_storeError;
   double                            m_lastOpenAttempt = 0;
   std::unique_ptr<JourneyTracker>   m_tracker;
   std::unique_ptr<JourneyTimerHost> m_host;
   StringList                        m_notes;
   std::string                       m_retentionLastRun;
   std::string                       m_openTouchedDate;       // R1: in memory, per instance
   double                            m_retentionRetryAfter = 0;   // m-b: back-off after a failed pass
   String                            m_lastRetentionError;   // one note per distinct failure (review I1)

   void OpenStore();
   void RunRetention();
};

} // namespace pcl

#endif // PICopilot_JourneyTracker_h
