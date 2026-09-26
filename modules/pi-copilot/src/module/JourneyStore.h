// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyStore_h
#define PICopilot_JourneyStore_h

#include "HistoryReader.h"
#include "JourneyTypes.h"

#include <pcl/String.h>
#include <pcl/StringList.h>

#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace pcl
{

constexpr int PICopilotJourneyDbBusyMs = 250;   // never wait longer on another program's lock

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

   int64 CreateJourney( const std::string& name, const std::string& target, const std::string& nowIso );
   void  RenameJourney( int64 journeyId, const std::string& name );
   void  TouchJourney( int64 journeyId, const std::string& nowIso );
   void  SetJourneyStatus( int64 journeyId, const std::string& status );
   void  MarkKept( int64 journeyId, int64 endImageId, const std::string& nowIso );
   int64 AddImage( int64 journeyId, const std::string& viewId, const std::string& filePath,
                   const std::string& fingerprint, bool isMaster, const std::string& nowIso );
   void  SetImageView( int64 imageId, const std::string& viewId, const std::string& filePath );
   void  SetAcquisition( int64 imageId, const AcquisitionFacts& a );
   int64 AddStep( const StepRow& s );
   void  SetStepState( int64 stepId, const std::string& state );
   void  SetStepReason( int64 stepId, const std::string& reason, bool inferred );
   void  AddStats( int64 imageId, int64 stepId /*0 = the image's starting stats*/, const std::vector<ChannelStats>& channels );
   void  AddLink( const LinkRow& l );
   void  AddGap( const GapRow& g );

   std::vector<JourneyRow> ListJourneys( bool keptOnly, const std::string& target, int limit );
   bool  GetJourney( int64 id, JourneyRow& out );
   std::vector<ImageRow> Images( int64 journeyId );
   bool  GetImage( int64 imageId, ImageRow& out );
   bool  FindOpenImageByView( const std::string& viewId, ImageRow& out );        // newest, journey status 'recording'
   bool  FindResumableByFingerprint( const std::string& fp, ImageRow& out );     // newest, journey NOT kept
   std::vector<StepRow> Steps( int64 imageId, bool includeSuperseded );
   bool  GetStep( int64 stepId, StepRow& out );
   std::vector<ChannelStats> Stats( int64 imageId, int64 stepId /*0 = starting*/ );
   bool  Acquisition( int64 imageId, AcquisitionFacts& out );
   std::vector<LinkRow> Links( int64 journeyId );
   std::vector<GapRow> Gaps( int64 journeyId );
   int   StepCount( int64 journeyId, bool activeOnly );   // base steps (params_json.base) excluded

   // Deletes non-kept journeys with updated < cutoffIso (rows cascade) and their
   // folders; returns how many. Keepers are never touched (Ruling 9). Each
   // folder goes before its row, so a folder that cannot be removed (throws)
   // keeps its row and is retried by the next pass.
   int   PruneUnkept( const std::string& cutoffIso, StringList* removedDirs );

   void  Checkpoint();   // PRAGMA wal_checkpoint(TRUNCATE)

private:

   JourneyStore( sqlite3* db, const String& root, const String& dbPath );

   sqlite3* m_db = nullptr;
   String   m_root;
   String   m_dbPath;

   void Exec( const char* sql );
   int  ScalarInt( const char* sql );
   std::string ScalarText( const char* sql );
   [[noreturn]] void Fail( const char* what ) const;
   void CreateSchemaV1();
   // Throws pcl::Error (DB path + `what`) unless on the root thread. Called by
   // the Stmt constructor and Exec, the two paths every DB read and write
   // takes. The only other access is the destructor's close, which cannot
   // throw: off the root thread it leaves the connection open instead of
   // closing it (and owners destroy the store only on the root thread).
   void RequireRootThread( const char* what ) const;

   friend class Stmt;
};

} // namespace pcl

#endif // PICopilot_JourneyStore_h
