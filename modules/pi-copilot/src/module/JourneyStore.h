// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyStore_h
#define PICopilot_JourneyStore_h

#include "HistoryReader.h"
#include "JourneyTypes.h"

#include <pcl/String.h>
#include <pcl/StringList.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace pcl
{

constexpr int PICopilotJourneyDbBusyMs = 250;   // never wait longer on another program's lock

// Reason prefix of a gap written because an image's history could not be read.
// Only these gaps are resolved when a later read succeeds (ResolveGaps).
constexpr const char* PICopilotJourneyReadGapPrefix = "history read failed: ";

// Thrown when a row a call needs does not exist (Task 7 re-reviews R2, m-f):
// the UPDATE-by-id mutators on a missing row, and ANY statement that fails
// with SQLITE_CONSTRAINT_FOREIGNKEY (an insert naming a vanished journey /
// image / step) -- classified by SQLite's extended error code, never by its
// English message. Callers can tell "gone" from "locked" / "failed".
class JourneyRowMissing : public Error
{
public:
   explicit JourneyRowMissing( const String& message ) : Error( message ) {}
};

// The recording owner of an image row (re-review m6): "<pid>:<process start
// ticks>" of a PixInsight process (this one when pid <= 0); "" when that
// process does not exist. JourneyOwnerAlive: the owner names a running process
// (pid and start ticks match, so a reused pid is not "alive").
std::string JourneyOwnerOf( long pid = 0 );
bool        JourneyOwnerAlive( const std::string& owner );
// Limits (documented, re-review round 2): owners are read from /proc, so they
// are set only on Linux (macOS / Windows: "", no protection); instances in
// different PID namespaces (Flatpak, containers) sharing one library see each
// other as dead.

std::string NowIso();                  // UTC, "YYYY-MM-DDThh:mm:ss.mmmZ"
std::string IsoDaysAgo( int days );     // same format, now - days
// Files and subdirectories (openat/unlinkat, O_NOFOLLOW): a symbolic link --
// including `dir` itself -- is removed, never followed. Absent = no-op. Any
// failure throws pcl::Error naming the path that could not be removed.
void RemoveDirectoryTree( const String& dir );

// Ruling 20. Replaces location data with "[redacted]"; true when anything changed.
bool RedactLocationData( nlohmann::json& parameters, nlohmann::json& tableParameters );

// A step row from a parsed history step, redaction applied (params_json =
// {parameters, tableParameters, xpsm, identity, mask, replayable, parseNote};
// Task 7 adds "base": true (bool) to steps already in the image's history when
// it joined a journey — absent means false; StepCount skips base steps).
StepRow MakeStepRow( const HistoryStep& h, int64 imageId, const std::string& state, const std::string& actor,
                     const std::string& reason, int historyIndex );

/*
 * The image-journey library: <root>/journeys.sqlite3 (schema v1, spec §5 +
 * stats.image_id, Ruling 2) plus a folder per journey (<root>/<id>/).
 * One connection, ROOT THREAD ONLY. Every write/read method throws pcl::Error
 * naming the database path and SQLite's message ("… database is locked" when
 * another program holds a lock for longer than PICopilotJourneyDbBusyMs).
 */
class JourneyStore
{
public:

   // Schema v1 was never released (re-review m-1r, verified: the shipped 0.1.2.0 has no journey code and
   // no library exists on the development machine), so its late changes -- image.owner and AUTOINCREMENT
   // ids for journey / image / step -- stay in v1 without a bump. Delete any development journey library
   // made before them (it keeps rowid reuse).
   static constexpr int SchemaVersion = 1;

   // Opens (creating a NEW file only when none exists) and checks it:
   // quick_check "ok", user_version 0 with no tables (initialized now) or 1.
   // A damaged file, a newer schema or a foreign database -> nullptr + error
   // with the exact path; the file is NEVER modified or recreated.
   // File discipline: `root` goes through EnsurePrivateDirectory (made 0700,
   // never through a link, ours, not writable by others); a new DB file is
   // created O_EXCL|O_NOFOLLOW with mode 0600 (its -wal/-shm follow); a DB
   // path that is a symbolic link or not a regular file is refused.
   static std::unique_ptr<JourneyStore> Open( const String& root, String& error );
   ~JourneyStore();

   JourneyStore( const JourneyStore& ) = delete;
   JourneyStore& operator =( const JourneyStore& ) = delete;

   const String& Root() const { return m_root; }
   const String& DbPath() const { return m_dbPath; }
   String JourneyDir( int64 journeyId ) const;

   // Mutators that UPDATE one row by id -- RenameJourney, TouchJourney,
   // SetJourneyStatus, MarkKept, SetImageView, SetImageOwner, SetStepState,
   // SetStepReason -- throw JourneyRowMissing (a pcl::Error:
   // "<Method>: <table> #<id>: no such row; nothing was changed") when that
   // row does not exist; they never succeed silently. Inserts that name a
   // parent row (AddImage, AddStep, AddStats, AddLink, AddGap, SetAcquisition)
   // fail through the foreign keys when it does not exist.
   int64 CreateJourney( const std::string& name, const std::string& target, const std::string& nowIso );
   void  RenameJourney( int64 journeyId, const std::string& name );
   void  TouchJourney( int64 journeyId, const std::string& nowIso );
   void  SetJourneyStatus( int64 journeyId, const std::string& status );
   void  MarkKept( int64 journeyId, int64 endImageId, const std::string& nowIso );
   int64 AddImage( int64 journeyId, const std::string& viewId, const std::string& filePath,
                   const std::string& fingerprint, bool isMaster, const std::string& nowIso );
   void  SetImageView( int64 imageId, const std::string& viewId, const std::string& filePath );
   void  SetImageOwner( int64 imageId, const std::string& owner );   // "" clears it
   void  SetAcquisition( int64 imageId, const AcquisitionFacts& a );
   int64 AddStep( const StepRow& s );
   void  SetStepState( int64 stepId, const std::string& state );
   void  SetStepReason( int64 stepId, const std::string& reason, bool inferred );
   void  AddStats( int64 imageId, int64 stepId /*0 = the image's starting stats*/, const std::vector<ChannelStats>& channels );
   void  AddLink( const LinkRow& l );
   void  AddGap( const GapRow& g );
   // Deletes the image's READ-FAILURE gaps (reason starting with
   // PICopilotJourneyReadGapPrefix) at after_step_seq < recordedUpToSeq: the
   // steps they stood for were read and recorded after all (Task 7 review
   // M4 / re-review m3). Other gaps are kept. Returns how many were deleted.
   int   ResolveGaps( int64 imageId, int recordedUpToSeq );

   std::vector<JourneyRow> ListJourneys( bool keptOnly, const std::string& target, int limit );
   bool  GetJourney( int64 id, JourneyRow& out );
   std::vector<ImageRow> Images( int64 journeyId );
   bool  GetImage( int64 imageId, ImageRow& out );
   bool  FindOpenImageByView( const std::string& viewId, ImageRow& out );        // newest, journey status 'recording'
   // Newest image with this fingerprint whose journey is NOT kept, skipping
   // excludeImageIds (the images open and recorded right now: a second window
   // of the same history-less master must never continue the first one's row,
   // Task 7 review I2).
   bool  FindResumableByFingerprint( const std::string& fp, ImageRow& out,
                                     const std::vector<int64>& excludeImageIds = std::vector<int64>() );
   // Every such image, newest first (the caller skips rows owned by another
   // live PixInsight instance, re-review m6).
   std::vector<ImageRow> ResumableByFingerprint( const std::string& fp,
                                                 const std::vector<int64>& excludeImageIds = std::vector<int64>() );
   std::vector<StepRow> Steps( int64 imageId, bool includeSuperseded );
   bool  GetStep( int64 stepId, StepRow& out );
   std::vector<ChannelStats> Stats( int64 imageId, int64 stepId /*0 = starting*/ );
   bool  Acquisition( int64 imageId, AcquisitionFacts& out );
   std::vector<LinkRow> Links( int64 journeyId );
   std::vector<GapRow> Gaps( int64 journeyId );
   int   StepCount( int64 journeyId, bool activeOnly );   // base and noEffect steps (params_json) excluded
   bool  HasReadGaps( int64 imageId );   // any gap with PICopilotJourneyReadGapPrefix (re-review m-e)

   // Deletes non-kept journeys with updated < cutoffIso (rows cascade) and their
   // folders; returns how many. Keepers are never touched (Ruling 9). A journey
   // with any image recorded by a LIVE PixInsight process (image.owner,
   // JourneyOwnerAlive) is skipped too (re-review m-a). Residual: the folder is
   // removed before COMMIT; if COMMIT then fails (I/O error, disk full) the row
   // outlives its folder -- readers (Tasks 8-10) must treat a missing folder or
   // thumbnail as absent, never as an error (a throw part-way through the
   // removal leaves the same shape). The next due pass removes such a row.
   // Task 7 re-review R1: each journey is ONE transaction that re-checks
   // "kept=0 AND updated < cutoff" under the write lock -- a journey kept or
   // touched meanwhile (by another PixInsight instance too) is skipped
   // silently -- then removes the folder; a folder that cannot be removed
   // rolls THAT journey's row back (the next pass retries both), the other
   // journeys are still pruned, and the first such failure is thrown at the end
   // (re-review m-j). Live owners are read before the DELETE (I-B).
   // excludeJourneyIds (the journeys this instance has open) are never pruned.
   int   PruneUnkept( const std::string& cutoffIso, StringList* removedDirs,
                      const std::vector<int64>& excludeJourneyIds = std::vector<int64>() );
   // Test hook: called with each selected id before its transaction (a second
   // connection can keep / touch the journey there, like another instance).
   void  SetPruneHookForSelfTest( std::function<void( int64 )> fn ) { m_pruneHook = std::move( fn ); }

   void  Checkpoint();   // PRAGMA wal_checkpoint(TRUNCATE)

   // MarkKept as its own transaction committed at synchronous=FULL, so the
   // commit is on disk (WAL fsync) before this returns: a keep is user intent
   // History cannot re-derive (Task 7 re-review m7). synchronous goes back to
   // NORMAL afterwards on every path. Refused loudly inside an open
   // Transaction (it could not make the outer commit durable). Root thread.
   void  MarkKeptDurably( int64 journeyId, int64 endImageId, const std::string& nowIso );
   // Test hooks: the synchronous level in force just before the keep's COMMIT,
   // and the connection's current level (1 = NORMAL, 2 = FULL).
   void  SetKeepCommitHookForSelfTest( std::function<void( int )> fn ) { m_keepCommitHook = std::move( fn ); }
   int   SynchronousLevelForSelfTest() { return ScalarInt( "PRAGMA synchronous" ); }
   // Fault seams for the reason-transaction tests (never set outside the harness):
   // - called first thing in SetStepReason with the step id (may throw, or arm below);
   void  SetStepReasonHookForSelfTest( std::function<void( int64 )> fn ) { m_stepReasonHook = std::move( fn ); }
   // - the NEXT statement step on this connection is interrupted by SQLite's own
   //   progress handler (a real SQLITE_INTERRUPT: inside an explicit transaction
   //   SQLite rolls the whole transaction back). One shot.
   void  ArmInterruptForSelfTest();
   // - raw SQL on this connection (e.g. a deferred foreign-key violation that makes COMMIT fail);
   void  ExecForSelfTest( const char* sql ) { Exec( sql ); }
   // - sqlite3_get_autocommit() (1 = no transaction open on the connection).
   int   AutocommitForSelfTest() const;

   /*
    * One atomic group of writes (Task 7 review I5; Tasks 9-10 use it for
    * FreezeJourney / keep). RAII, root thread only:
    *   JourneyStore::Transaction tx( store );   // BEGIN IMMEDIATE
    *   store.CreateJourney( ... ); store.AddImage( ... ); ...
    *   tx.Commit();                             // COMMIT
    * - The constructor takes the write lock at once (BEGIN IMMEDIATE), so a
    *   lock held by another program fails HERE, before any write, after at
    *   most PICopilotJourneyDbBusyMs ("... database is locked").
    * - Nesting is refused loudly: a second Transaction on the same store
    *   throws pcl::Error ("a transaction is already open"); it is never
    *   silently flattened into the outer one.
    * - Without Commit() -- an exception between the two -- the destructor
    *   rolls everything back (noexcept; skipped off the root thread, where the
    *   connection is never touched). A failed Commit() throws and the
    *   destructor then rolls back.
    * Every read and write method works inside a transaction as outside it.
    * - The constructor also refuses when the CONNECTION is already inside a
    *   transaction (sqlite3_get_autocommit() == 0, e.g. a failed rollback),
    *   with a precise error, instead of SQLite's generic one (re-review m1).
    * - NEVER pump events (ProcessEvents, a process, a script) inside a
    *   Transaction: a tick would find the store busy and defer.
    * - Contract for Tasks 9/10 (re-review m7): the library runs at
    *   synchronous=NORMAL, so a power loss can drop the last commits. A keep
    *   (MarkKept) is user intent that History cannot re-derive: run
    *   Checkpoint() (or PRAGMA synchronous=FULL around it) after its commit.
    */
   class Transaction
   {
   public:
      explicit Transaction( JourneyStore& store );
      ~Transaction();
      void Commit();
      Transaction( const Transaction& ) = delete;
      Transaction& operator =( const Transaction& ) = delete;
   private:
      JourneyStore& m_store;
      bool          m_open = false;
   };
   bool  InTransaction() const { return m_inTransaction; }
   // True when a Transaction is open here but SQLite has already rolled the
   // connection back on its own (an I/O, full-disk or corruption error aborts
   // the whole transaction): later writes would autocommit one by one, so the
   // caller must stop the group and report every write in it as not made.
   // Root thread (throws off it, like every other store access).
   bool  TransactionAborted() const;

private:

   JourneyStore( sqlite3* db, const String& root, const String& dbPath );

   sqlite3* m_db = nullptr;
   bool     m_inTransaction = false;
   std::function<void( int64 )> m_pruneHook;
   std::function<void( int )>   m_keepCommitHook;
   std::function<void( int64 )> m_stepReasonHook;
   bool                         m_interruptArmed = false;
   String   m_root;
   String   m_dbPath;

   void Exec( const char* sql );
   int  ScalarInt( const char* sql );
   std::string ScalarText( const char* sql );
   [[noreturn]] void Fail( const char* what ) const;
   // Throws unless the last statement changed at least one row (see the mutators' contract).
   void RequireChanged( const char* what, int64 id ) const;
   void CreateSchemaV1();
   // Throws pcl::Error (DB path + `what`) unless on the root thread. Called by
   // the Stmt constructor and Exec, the two paths every DB read and write
   // takes (Transaction goes through Exec). The only other accesses are the
   // destructors -- ~JourneyStore's close and ~Transaction's rollback -- which
   // cannot throw: off the root thread they leave the connection untouched
   // (and owners destroy the store only on the root thread).
   void RequireRootThread( const char* what ) const;

   friend class Stmt;
};

} // namespace pcl

#endif // PICopilot_JourneyStore_h
