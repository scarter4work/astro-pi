// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneyStore.h"
#include "SafeFileWrite.h"   // EnsurePrivateDirectory
#include "Utf8.h"
#include "ViewContext.h"   // IsRedactedFitsKeyword

#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/Thread.h>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

namespace
{

const char* const kSchemaV1 =
   "CREATE TABLE journey("
   " id INTEGER PRIMARY KEY, created TEXT NOT NULL, updated TEXT NOT NULL, name TEXT NOT NULL, target TEXT,"
   " kept INTEGER NOT NULL DEFAULT 0, kept_at TEXT, end_image_id INTEGER,"
   " status TEXT NOT NULL DEFAULT 'recording' CHECK(status IN ('recording','ended')));"
   "CREATE TABLE image("
   " id INTEGER PRIMARY KEY, journey_id INTEGER NOT NULL REFERENCES journey(id) ON DELETE CASCADE,"
   " view_id TEXT NOT NULL, file_path TEXT, fingerprint TEXT NOT NULL, is_master INTEGER NOT NULL, created TEXT NOT NULL);"
   "CREATE TABLE acquisition("
   " image_id INTEGER PRIMARY KEY REFERENCES image(id) ON DELETE CASCADE, target TEXT, filter TEXT, camera TEXT,"
   " gain REAL, offset REAL, sensor_temp REAL, sub_exposure REAL, sub_count INTEGER, total_integration_s REAL,"
   " session_date TEXT);"
   "CREATE TABLE step("
   " id INTEGER PRIMARY KEY, image_id INTEGER NOT NULL REFERENCES image(id) ON DELETE CASCADE, seq INTEGER NOT NULL,"
   " process_id TEXT NOT NULL, params_json TEXT NOT NULL, started TEXT, duration_s REAL,"
   " actor TEXT NOT NULL CHECK(actor IN ('user','copilot')), reason TEXT, reason_inferred INTEGER NOT NULL DEFAULT 0,"
   " state TEXT NOT NULL CHECK(state IN ('active','undone','superseded')), history_index INTEGER NOT NULL);"
   "CREATE TABLE stats("
   " step_id INTEGER REFERENCES step(id) ON DELETE CASCADE,"
   " image_id INTEGER NOT NULL REFERENCES image(id) ON DELETE CASCADE,"
   " channel INTEGER NOT NULL, median REAL, mad REAL, mean REAL, min REAL, max REAL, noise REAL);"
   "CREATE TABLE link("
   " from_image_id INTEGER NOT NULL REFERENCES image(id) ON DELETE CASCADE,"
   " to_image_id INTEGER NOT NULL REFERENCES image(id) ON DELETE CASCADE,"
   " via_step_id INTEGER REFERENCES step(id) ON DELETE SET NULL,"
   " evidence TEXT NOT NULL CHECK(evidence IN ('copilot','timing','reference')), UNIQUE(from_image_id, to_image_id));"
   "CREATE TABLE gap("
   " journey_id INTEGER NOT NULL REFERENCES journey(id) ON DELETE CASCADE,"
   " image_id INTEGER REFERENCES image(id) ON DELETE CASCADE, after_step_seq INTEGER, reason TEXT NOT NULL);"
   "CREATE INDEX image_journey ON image(journey_id);"
   "CREATE INDEX image_view ON image(view_id);"
   "CREATE INDEX image_fingerprint ON image(fingerprint);"
   "CREATE INDEX step_image ON step(image_id, seq);"
   "CREATE INDEX stats_image ON stats(image_id, step_id);"
   "PRAGMA user_version = 1;";

bool IsLocationId( const std::string& id )
{
   if ( IsRedactedFitsKeyword( IsoString( id.c_str() ) ) )
      return true;
   const std::string l = AsciiLower( id );   // JourneyTypes.h
   for ( const char* w : { "latitude", "longitude", "elevation", "observer" } )
      if ( l.find( w ) != std::string::npos )
         return true;
   return false;
}

bool IsRedactedName( const nlohmann::json& cell )
{
   return cell.is_string() && IsRedactedFitsKeyword( IsoString( cell.get<std::string>().c_str() ) );
}

} // namespace

// Small RAII statement wrapper; every failure throws with the DB path.
class Stmt
{
public:

   Stmt( const JourneyStore& s, const char* sql ) : m_s( s )
   {
      s.RequireRootThread( sql );   // every read and write passes here (or through Exec)
      if ( sqlite3_prepare_v2( s.m_db, sql, -1, &m_st, nullptr ) != SQLITE_OK )
         s.Fail( sql );
   }
   ~Stmt() { sqlite3_finalize( m_st ); }
   Stmt( const Stmt& ) = delete;
   Stmt& operator =( const Stmt& ) = delete;

   Stmt& Text( int i, const std::string& v ) { sqlite3_bind_text( m_st, i, v.c_str(), int( v.size() ), SQLITE_TRANSIENT ); return *this; }
   Stmt& TextOrNull( int i, const std::string& v ) { if ( v.empty() ) sqlite3_bind_null( m_st, i ); else Text( i, v ); return *this; }
   Stmt& Int( int i, int64 v ) { sqlite3_bind_int64( m_st, i, v ); return *this; }
   Stmt& IntOrNull( int i, int64 v ) { if ( v == 0 ) sqlite3_bind_null( m_st, i ); else Int( i, v ); return *this; }
   Stmt& Real( int i, double v ) { sqlite3_bind_double( m_st, i, v ); return *this; }
   Stmt& Opt( int i, const std::optional<double>& v ) { if ( v ) Real( i, *v ); else sqlite3_bind_null( m_st, i ); return *this; }
   Stmt& Opt( int i, const std::optional<int>& v ) { if ( v ) Int( i, *v ); else sqlite3_bind_null( m_st, i ); return *this; }

   bool Row()
   {
      const int rc = sqlite3_step( m_st );
      if ( rc == SQLITE_ROW )
         return true;
      if ( rc != SQLITE_DONE )
         m_s.Fail( sqlite3_sql( m_st ) );
      return false;
   }
   void Run() { while ( Row() ) {} }

   std::string ColText( int c ) const
   {
      const unsigned char* t = sqlite3_column_text( m_st, c );
      return t != nullptr ? std::string( reinterpret_cast<const char*>( t ), size_t( sqlite3_column_bytes( m_st, c ) ) ) : std::string();
   }
   int64 ColInt( int c ) const { return sqlite3_column_int64( m_st, c ); }
   double ColReal( int c ) const { return sqlite3_column_double( m_st, c ); }
   bool IsNull( int c ) const { return sqlite3_column_type( m_st, c ) == SQLITE_NULL; }

private:

   const JourneyStore& m_s;
   sqlite3_stmt*       m_st = nullptr;
};

std::string NowIso()
{
   const auto now = std::chrono::system_clock::now();
   const std::time_t t = std::chrono::system_clock::to_time_t( now );
   const int ms = int( std::chrono::duration_cast<std::chrono::milliseconds>( now.time_since_epoch() ).count() % 1000 );
   std::tm tm;
   gmtime_r( &t, &tm );
   char buf[ 64 ];
   std::snprintf( buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms );
   return buf;
}

std::string IsoDaysAgo( int days )
{
   const std::time_t t = std::time( nullptr ) - std::time_t( days )*86400;
   std::tm tm;
   gmtime_r( &t, &tm );
   char buf[ 64 ];
   std::snprintf( buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec );
   return buf;
}

namespace
{

[[noreturn]] void TreeFail( const char* what, const std::string& path, int e )
{
   throw Error( String( "cannot " ) + what + ' ' + FromU8( path ) + ": " + String( std::strerror( e ) ) );
}

// Empties and removes `name`, a real directory (not a link) inside the open
// directory parentFd. Everything is relative to directory descriptors opened
// with O_NOFOLLOW and inspected with AT_SYMLINK_NOFOLLOW, so a link -- even
// one swapped in while this runs -- is only ever unlinked, never entered.
void RemoveTreeAt( int parentFd, const std::string& name, const std::string& shown )
{
   const int fd = ::openat( parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
   if ( fd < 0 )
      TreeFail( "open directory", shown, errno );
   DIR* d = ::fdopendir( fd );
   if ( d == nullptr )
   {
      const int e = errno;
      ::close( fd );
      TreeFail( "read directory", shown, e );
   }
   std::vector<std::string> names;
   errno = 0;
   while ( const dirent* e = ::readdir( d ) )
      if ( std::strcmp( e->d_name, "." ) != 0 && std::strcmp( e->d_name, ".." ) != 0 )
         names.push_back( e->d_name );
   if ( errno != 0 )
   {
      const int e = errno;
      ::closedir( d );
      TreeFail( "read directory", shown, e );
   }
   try
   {
      const int dfd = ::dirfd( d );
      for ( const std::string& n : names )
      {
         const std::string child = shown + '/' + n;
         struct stat st;
         if ( ::fstatat( dfd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW ) != 0 )
         {
            if ( errno == ENOENT )
               continue;
            TreeFail( "inspect", child, errno );
         }
         if ( S_ISDIR( st.st_mode ) )
            RemoveTreeAt( dfd, n, child );
         else if ( ::unlinkat( dfd, n.c_str(), 0 ) != 0 && errno != ENOENT )
            TreeFail( "remove", child, errno );
      }
   }
   catch ( ... )
   {
      ::closedir( d );
      throw;
   }
   ::closedir( d );
   if ( ::unlinkat( parentFd, name.c_str(), AT_REMOVEDIR ) != 0 && errno != ENOENT )
      TreeFail( "remove directory", shown, errno );
}

} // namespace

void RemoveDirectoryTree( const String& dir )
{
   std::string p = U8( dir );
   while ( p.size() > 1 && p.back() == '/' )
      p.pop_back();
   if ( p.empty() )
      return;
   if ( p == "/" )
      throw Error( "RemoveDirectoryTree: refusing to remove /" );
   struct stat st;
   if ( ::lstat( p.c_str(), &st ) != 0 )
   {
      if ( errno == ENOENT )
         return;
      TreeFail( "inspect", p, errno );
   }
   if ( S_ISLNK( st.st_mode ) )   // the link itself goes; its target is never touched
   {
      if ( ::unlink( p.c_str() ) != 0 && errno != ENOENT )
         TreeFail( "remove", p, errno );
      return;
   }
   if ( !S_ISDIR( st.st_mode ) )
      throw Error( dir + " is not a directory; not removed" );
   const size_t slash = p.find_last_of( '/' );
   const std::string parent = slash == std::string::npos ? std::string( "." ) : (slash == 0 ? std::string( "/" ) : p.substr( 0, slash ));
   const std::string base = slash == std::string::npos ? p : p.substr( slash + 1 );
   const int pfd = ::open( parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC );
   if ( pfd < 0 )
      TreeFail( "open directory", parent, errno );
   try
   {
      RemoveTreeAt( pfd, base, p );
   }
   catch ( ... )
   {
      ::close( pfd );
      throw;
   }
   ::close( pfd );
}

bool RedactLocationData( nlohmann::json& parameters, nlohmann::json& tables )
{
   bool changed = false;
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         if ( IsLocationId( it.key() ) && it.value() != "[redacted]" )
         {
            it.value() = "[redacted]";
            changed = true;
         }
   if ( tables.is_object() )
      for ( auto t = tables.begin(); t != tables.end(); ++t )
      {
         if ( !t.value().is_array() )
            continue;
         for ( nlohmann::json& row : t.value() )
         {
            bool hit = false;
            if ( row.is_array() )
            {
               for ( const nlohmann::json& cell : row )
                  hit = hit || IsRedactedName( cell ) || (cell.is_string() && IsLocationId( cell.get<std::string>() ));
               if ( hit )
                  for ( nlohmann::json& cell : row )
                     if ( !IsRedactedName( cell ) && !(cell.is_string() && IsLocationId( cell.get<std::string>() )) )
                        cell = "[redacted]";
            }
            else if ( row.is_object() )   // raw rows of an uninstalled process
               for ( auto c = row.begin(); c != row.end(); ++c )
                  if ( IsLocationId( c.key() ) || IsRedactedName( c.value() ) )
                  {
                     c.value() = "[redacted]";
                     hit = true;
                  }
            changed = changed || hit;
         }
      }
   return changed;
}

StepRow MakeStepRow( const HistoryStep& h, int64 imageId, const std::string& state, const std::string& actor,
                     const std::string& reason, int historyIndex )
{
   StepRow r;
   r.imageId = imageId;
   r.seq = h.combinedIndex + 1;
   r.processId = h.processId;
   r.started = h.started;
   r.durationS = h.durationS;
   r.actor = actor;
   r.reason = reason;
   r.state = state;
   r.historyIndex = historyIndex;
   nlohmann::json p = h.parameters, t = h.tableParameters;
   const bool redacted = RedactLocationData( p, t );
   r.params = {
      { "parameters", p }, { "tableParameters", t },
      { "xpsm", redacted ? std::string() : h.xpsm },
      { "identity", h.identity },   // a hash: reveals nothing, and must keep matching future reads
      { "mask", h.maskId.empty() ? nlohmann::json() : nlohmann::json( { { "id", h.maskId }, { "inverted", h.maskInverted } } ) },
      { "replayable", h.replayable && !redacted },
      { "parseNote", redacted ? std::string( "contained observing-site data; not stored" ) : h.parseNote } };
   return r;
}

JourneyStore::JourneyStore( sqlite3* db, const String& root, const String& dbPath )
   : m_db( db ), m_root( root ), m_dbPath( dbPath )
{
}

JourneyStore::~JourneyStore()
{
   if ( m_db != nullptr )
      sqlite3_close_v2( m_db );
}

void JourneyStore::Fail( const char* what ) const
{
   throw Error( "journey database " + m_dbPath + ": " + FromU8( sqlite3_errmsg( m_db ) )
                + " (" + FromU8( std::string( what != nullptr ? what : "" ).substr( 0, 60 ) ) + ")" );
}

void JourneyStore::RequireRootThread( const char* what ) const
{
   if ( !Thread::IsRootThread() )
      throw Error( "journey database " + m_dbPath + ": called off the root thread ("
                   + FromU8( std::string( what != nullptr ? what : "" ).substr( 0, 60 ) )
                   + "); JourneyStore is root thread only -- nothing was read or written" );
}

void JourneyStore::Exec( const char* sql )
{
   RequireRootThread( sql );
   char* err = nullptr;
   if ( sqlite3_exec( m_db, sql, nullptr, nullptr, &err ) != SQLITE_OK )
   {
      sqlite3_free( err );
      Fail( sql );
   }
}

int JourneyStore::ScalarInt( const char* sql )
{
   Stmt s( *this, sql );
   return s.Row() ? int( s.ColInt( 0 ) ) : 0;
}

std::string JourneyStore::ScalarText( const char* sql )
{
   Stmt s( *this, sql );
   return s.Row() ? s.ColText( 0 ) : std::string();
}

void JourneyStore::CreateSchemaV1()
{
   Exec( "BEGIN IMMEDIATE" );
   try
   {
      Exec( kSchemaV1 );
      Exec( "COMMIT" );
   }
   catch ( ... )
   {
      sqlite3_exec( m_db, "ROLLBACK", nullptr, nullptr, nullptr );
      throw;
   }
}

std::unique_ptr<JourneyStore> JourneyStore::Open( const String& root, String& error )
{
   const String path = root + "/journeys.sqlite3";
   const String keep = ". PI Copilot never replaces it: move the file aside to start a new library, or restore a backup. "
                       "Recording is paused.";
   if ( !Thread::IsRootThread() )   // before ANY file-system or SQLite work
   {
      error = "journey database " + path + ": JourneyStore::Open called off the root thread; JourneyStore is root "
              "thread only -- nothing was created, opened or changed. Recording is paused.";
      return nullptr;
   }
   try
   {
      // The LIBRARY root is ours (unlike the export folder): made 0700 without
      // following links, and it must be a real directory nobody else can write
      // into -- then nothing inside it (the DB, its -wal/-shm, the journey
      // folders) can be planted or swapped by another user.
      const String rootWhy = EnsurePrivateDirectory( root );
      if ( !rootWhy.IsEmpty() )
      {
         error = "journey library " + rootWhy + ". Recording is paused.";
         return nullptr;
      }
      // The DB file: a NEW one is created here with O_EXCL|O_NOFOLLOW and mode
      // 0600 (SQLite gives its -wal/-shm the same mode); SQLite itself never
      // creates it (no SQLITE_OPEN_CREATE) and never follows a link to it
      // (SQLITE_OPEN_NOFOLLOW). An existing file is opened as it is.
      const std::string native = U8( path );
      struct stat st;
      if ( ::lstat( native.c_str(), &st ) != 0 )
      {
         if ( errno != ENOENT )
         {
            error = "journey database " + path + " cannot be inspected: " + String( std::strerror( errno ) ) + ". Recording is paused.";
            return nullptr;
         }
         const int fd = ::open( native.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
         if ( fd >= 0 )
            ::close( fd );
         else if ( errno != EEXIST )
         {
            error = "journey database " + path + " cannot be created: " + String( std::strerror( errno ) ) + ". Recording is paused.";
            return nullptr;
         }
         if ( ::lstat( native.c_str(), &st ) != 0 )
         {
            error = "journey database " + path + " cannot be inspected: " + String( std::strerror( errno ) ) + ". Recording is paused.";
            return nullptr;
         }
      }
      if ( S_ISLNK( st.st_mode ) )
      {
         error = "journey database " + path + " is a symbolic link; PI Copilot does not follow it" + keep;
         return nullptr;
      }
      if ( !S_ISREG( st.st_mode ) )
      {
         error = "journey database " + path + " is not a regular file" + keep;
         return nullptr;
      }
      sqlite3* db = nullptr;
      const int rc = sqlite3_open_v2( native.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, nullptr );
      if ( rc != SQLITE_OK )
      {
         error = "journey database " + path + " cannot be opened: " + FromU8( db != nullptr ? sqlite3_errmsg( db ) : "out of memory" ) + keep;
         if ( db != nullptr )
            sqlite3_close_v2( db );
         return nullptr;
      }
      std::unique_ptr<JourneyStore> s( new JourneyStore( db, root, path ) );
      sqlite3_busy_timeout( db, PICopilotJourneyDbBusyMs );
      std::string qc;
      try
      {
         qc = s->ScalarText( "PRAGMA quick_check" );
      }
      catch ( const pcl::Exception& )
      {
         // Another program holding a lock is NOT damage: say so, keep the file,
         // and let the caller retry. Anything else is damage, in SQLite's own
         // words ("file is not a database", "database disk image is malformed").
         const int code = sqlite3_errcode( db ) & 0xff;
         if ( code == SQLITE_BUSY || code == SQLITE_LOCKED )
            error = "journey database " + path + " is locked by another program (" + FromU8( sqlite3_errmsg( db ) )
                  + "); it is left untouched and recording is paused until it is released.";
         else
            error = "journey database " + path + " is damaged (" + FromU8( sqlite3_errmsg( db ) ) + ")" + keep;
         return nullptr;
      }
      if ( qc != "ok" )
      {
         error = "journey database " + path + " is damaged (" + FromU8( qc ) + ")" + keep;
         return nullptr;
      }
      try
      {
         const int version = s->ScalarInt( "PRAGMA user_version" );
         const int tables = s->ScalarInt( "SELECT count(*) FROM sqlite_master WHERE type='table'" );
         if ( version > SchemaVersion )
         {
            error = "journey database " + path + String().Format( " was written by a newer PI Copilot (schema %d); update "
                    "PI Copilot. It is left untouched; recording is paused.", version );
            return nullptr;
         }
         if ( version == 0 && tables > 0 )
         {
            error = "journey database " + path + " is not a PI Copilot journey database (it has tables but no schema "
                    "version)" + keep;
            return nullptr;
         }
         const std::string mode = AsciiLower( s->ScalarText( "PRAGMA journal_mode=WAL" ) );
         if ( mode != "wal" )
         {
            error = "journey database " + path + " could not be switched to WAL journaling (journal_mode is '"
                  + FromU8( mode ) + "'). Recording is paused.";
            return nullptr;
         }
         s->Exec( "PRAGMA foreign_keys=ON" );
         if ( version == 0 )
            s->CreateSchemaV1();
         return s;
      }
      catch ( const pcl::Exception& x )
      {
         // x names the path and SQLite's message. A lock is transient: no
         // "move the file aside" advice for it.
         const int code = sqlite3_errcode( db ) & 0xff;
         error = x.Message() + ((code == SQLITE_BUSY || code == SQLITE_LOCKED)
                                ? String( "; the file is left untouched and recording is paused until it is released." )
                                : keep);
         return nullptr;
      }
   }
   catch ( const pcl::Exception& x )
   {
      error = x.Message() + keep;
   }
   catch ( const std::exception& x )
   {
      error = "journey database " + path + ": " + String( x.what() ) + keep;
   }
   return nullptr;
}

String JourneyStore::JourneyDir( int64 journeyId ) const
{
   return m_root + String().Format( "/%lld", static_cast<long long>( journeyId ) );
}

int64 JourneyStore::CreateJourney( const std::string& name, const std::string& target, const std::string& nowIso )
{
   Stmt( *this, "INSERT INTO journey(created, updated, name, target) VALUES(?,?,?,?)" )
      .Text( 1, nowIso ).Text( 2, nowIso ).Text( 3, name ).TextOrNull( 4, target ).Run();
   return sqlite3_last_insert_rowid( m_db );
}

void JourneyStore::RenameJourney( int64 id, const std::string& name )
{
   Stmt( *this, "UPDATE journey SET name=? WHERE id=?" ).Text( 1, name ).Int( 2, id ).Run();
}

void JourneyStore::TouchJourney( int64 id, const std::string& nowIso )
{
   Stmt( *this, "UPDATE journey SET updated=? WHERE id=?" ).Text( 1, nowIso ).Int( 2, id ).Run();
}

void JourneyStore::SetJourneyStatus( int64 id, const std::string& status )
{
   Stmt( *this, "UPDATE journey SET status=? WHERE id=?" ).Text( 1, status ).Int( 2, id ).Run();
}

void JourneyStore::MarkKept( int64 id, int64 endImageId, const std::string& nowIso )
{
   Stmt( *this, "UPDATE journey SET kept=1, kept_at=?, end_image_id=?, updated=? WHERE id=?" )
      .Text( 1, nowIso ).IntOrNull( 2, endImageId ).Text( 3, nowIso ).Int( 4, id ).Run();
}

int64 JourneyStore::AddImage( int64 journeyId, const std::string& viewId, const std::string& filePath,
                              const std::string& fingerprint, bool isMaster, const std::string& nowIso )
{
   Stmt( *this, "INSERT INTO image(journey_id, view_id, file_path, fingerprint, is_master, created) VALUES(?,?,?,?,?,?)" )
      .Int( 1, journeyId ).Text( 2, viewId ).TextOrNull( 3, filePath ).Text( 4, fingerprint ).Int( 5, isMaster ? 1 : 0 )
      .Text( 6, nowIso ).Run();
   return sqlite3_last_insert_rowid( m_db );
}

void JourneyStore::SetImageView( int64 imageId, const std::string& viewId, const std::string& filePath )
{
   Stmt( *this, "UPDATE image SET view_id=?, file_path=COALESCE(?, file_path) WHERE id=?" )
      .Text( 1, viewId ).TextOrNull( 2, filePath ).Int( 3, imageId ).Run();
}

void JourneyStore::SetAcquisition( int64 imageId, const AcquisitionFacts& a )
{
   Stmt( *this, "INSERT OR REPLACE INTO acquisition(image_id, target, filter, camera, gain, offset, sensor_temp, "
                "sub_exposure, sub_count, total_integration_s, session_date) VALUES(?,?,?,?,?,?,?,?,?,?,?)" )
      .Int( 1, imageId ).TextOrNull( 2, a.target ).TextOrNull( 3, a.filter ).TextOrNull( 4, a.camera )
      .Opt( 5, a.gain ).Opt( 6, a.offset ).Opt( 7, a.sensorTempC ).Opt( 8, a.subExposureS ).Opt( 9, a.subCount )
      .Opt( 10, a.totalIntegrationS ).TextOrNull( 11, a.sessionDate ).Run();
}

int64 JourneyStore::AddStep( const StepRow& s )
{
   // Redaction is re-applied here too: whatever built the row, location data never lands.
   nlohmann::json params = s.params;
   if ( params.contains( "parameters" ) && params.contains( "tableParameters" )
     && RedactLocationData( params["parameters"], params["tableParameters"] ) )
   {
      params["xpsm"] = "";
      params["replayable"] = false;
      params["parseNote"] = "contained observing-site data; not stored";
   }
   Stmt st( *this, "INSERT INTO step(image_id, seq, process_id, params_json, started, duration_s, actor, reason, "
                   "reason_inferred, state, history_index) VALUES(?,?,?,?,?,?,?,?,?,?,?)" );
   st.Int( 1, s.imageId ).Int( 2, s.seq ).Text( 3, s.processId ).Text( 4, params.dump() ).TextOrNull( 5, s.started );
   if ( s.durationS < 0 )
      st.Opt( 6, std::optional<double>() );
   else
      st.Real( 6, s.durationS );
   st.Text( 7, s.actor ).TextOrNull( 8, s.reason ).Int( 9, s.reasonInferred ? 1 : 0 ).Text( 10, s.state ).Int( 11, s.historyIndex ).Run();
   return sqlite3_last_insert_rowid( m_db );
}

void JourneyStore::SetStepState( int64 stepId, const std::string& state )
{
   Stmt( *this, "UPDATE step SET state=? WHERE id=?" ).Text( 1, state ).Int( 2, stepId ).Run();
}

void JourneyStore::SetStepReason( int64 stepId, const std::string& reason, bool inferred )
{
   Stmt( *this, "UPDATE step SET reason=?, reason_inferred=? WHERE id=?" )
      .TextOrNull( 1, reason ).Int( 2, inferred ? 1 : 0 ).Int( 3, stepId ).Run();
}

void JourneyStore::AddStats( int64 imageId, int64 stepId, const std::vector<ChannelStats>& channels )
{
   if ( stepId == 0 )
      Stmt( *this, "DELETE FROM stats WHERE image_id=? AND step_id IS NULL" ).Int( 1, imageId ).Run();
   else
      Stmt( *this, "DELETE FROM stats WHERE step_id=?" ).Int( 1, stepId ).Run();
   for ( const ChannelStats& c : channels )
      Stmt( *this, "INSERT INTO stats(step_id, image_id, channel, median, mad, mean, min, max, noise) VALUES(?,?,?,?,?,?,?,?,?)" )
         .IntOrNull( 1, stepId ).Int( 2, imageId ).Int( 3, c.channel ).Real( 4, c.median ).Real( 5, c.mad ).Real( 6, c.mean )
         .Real( 7, c.min ).Real( 8, c.max ).Real( 9, c.noise ).Run();
}

void JourneyStore::AddLink( const LinkRow& l )
{
   Stmt( *this, "INSERT OR IGNORE INTO link(from_image_id, to_image_id, via_step_id, evidence) VALUES(?,?,?,?)" )
      .Int( 1, l.fromImageId ).Int( 2, l.toImageId ).IntOrNull( 3, l.viaStepId ).Text( 4, l.evidence ).Run();
}

void JourneyStore::AddGap( const GapRow& g )
{
   Stmt( *this, "INSERT INTO gap(journey_id, image_id, after_step_seq, reason) VALUES(?,?,?,?)" )
      .Int( 1, g.journeyId ).IntOrNull( 2, g.imageId ).Int( 3, g.afterSeq ).Text( 4, g.reason ).Run();
}

namespace
{
JourneyRow ReadJourney( Stmt& s )
{
   JourneyRow j;
   j.id = s.ColInt( 0 ); j.created = s.ColText( 1 ); j.updated = s.ColText( 2 ); j.name = s.ColText( 3 );
   j.target = s.ColText( 4 ); j.kept = s.ColInt( 5 ) != 0; j.keptAt = s.ColText( 6 ); j.endImageId = s.ColInt( 7 );
   j.status = s.ColText( 8 );
   return j;
}
ImageRow ReadImage( Stmt& s )
{
   ImageRow i;
   i.id = s.ColInt( 0 ); i.journeyId = s.ColInt( 1 ); i.viewId = s.ColText( 2 ); i.filePath = s.ColText( 3 );
   i.fingerprint = s.ColText( 4 ); i.isMaster = s.ColInt( 5 ) != 0; i.created = s.ColText( 6 );
   return i;
}
StepRow ReadStep( Stmt& s )
{
   StepRow r;
   r.id = s.ColInt( 0 ); r.imageId = s.ColInt( 1 ); r.seq = int( s.ColInt( 2 ) ); r.processId = s.ColText( 3 );
   r.params = nlohmann::json::parse( s.ColText( 4 ) ); r.started = s.ColText( 5 );
   r.durationS = s.IsNull( 6 ) ? -1 : s.ColReal( 6 ); r.actor = s.ColText( 7 ); r.reason = s.ColText( 8 );
   r.reasonInferred = s.ColInt( 9 ) != 0; r.state = s.ColText( 10 ); r.historyIndex = int( s.ColInt( 11 ) );
   return r;
}
const char* const kJourneyCols = "id, created, updated, name, target, kept, kept_at, end_image_id, status";
const char* const kImageCols = "id, journey_id, view_id, file_path, fingerprint, is_master, created";
const char* const kStepCols = "id, image_id, seq, process_id, params_json, started, duration_s, actor, reason, "
                              "reason_inferred, state, history_index";
} // namespace

std::vector<JourneyRow> JourneyStore::ListJourneys( bool keptOnly, const std::string& target, int limit )
{
   const std::string sql = std::string( "SELECT " ) + kJourneyCols + " FROM journey WHERE (?1 = 0 OR kept = 1)"
                         " AND (?2 = '' OR lower(target) = lower(?2)) ORDER BY updated DESC LIMIT ?3";
   Stmt s( *this, sql.c_str() );
   s.Int( 1, keptOnly ? 1 : 0 ).Text( 2, target ).Int( 3, std::max( 1, limit ) );
   std::vector<JourneyRow> r;
   while ( s.Row() )
      r.push_back( ReadJourney( s ) );
   return r;
}

bool JourneyStore::GetJourney( int64 id, JourneyRow& out )
{
   Stmt s( *this, ( std::string( "SELECT " ) + kJourneyCols + " FROM journey WHERE id=?" ).c_str() );
   s.Int( 1, id );
   if ( !s.Row() )
      return false;
   out = ReadJourney( s );
   return true;
}

std::vector<ImageRow> JourneyStore::Images( int64 journeyId )
{
   Stmt s( *this, ( std::string( "SELECT " ) + kImageCols + " FROM image WHERE journey_id=? ORDER BY id" ).c_str() );
   s.Int( 1, journeyId );
   std::vector<ImageRow> r;
   while ( s.Row() )
      r.push_back( ReadImage( s ) );
   return r;
}

bool JourneyStore::GetImage( int64 imageId, ImageRow& out )
{
   Stmt s( *this, ( std::string( "SELECT " ) + kImageCols + " FROM image WHERE id=?" ).c_str() );
   s.Int( 1, imageId );
   if ( !s.Row() )
      return false;
   out = ReadImage( s );
   return true;
}

bool JourneyStore::FindOpenImageByView( const std::string& viewId, ImageRow& out )
{
   Stmt s( *this, "SELECT i.id, i.journey_id, i.view_id, i.file_path, i.fingerprint, i.is_master, i.created FROM image i"
                  " JOIN journey j ON j.id = i.journey_id WHERE i.view_id=? AND j.status='recording' ORDER BY i.id DESC LIMIT 1" );
   s.Text( 1, viewId );
   if ( !s.Row() )
      return false;
   out = ReadImage( s );
   return true;
}

bool JourneyStore::FindResumableByFingerprint( const std::string& fp, ImageRow& out )
{
   Stmt s( *this, "SELECT i.id, i.journey_id, i.view_id, i.file_path, i.fingerprint, i.is_master, i.created FROM image i"
                  " JOIN journey j ON j.id = i.journey_id WHERE i.fingerprint=? AND j.kept=0 ORDER BY i.id DESC LIMIT 1" );
   s.Text( 1, fp );
   if ( !s.Row() )
      return false;
   out = ReadImage( s );
   return true;
}

std::vector<StepRow> JourneyStore::Steps( int64 imageId, bool includeSuperseded )
{
   Stmt s( *this, ( std::string( "SELECT " ) + kStepCols + " FROM step WHERE image_id=?"
                    + (includeSuperseded ? "" : " AND state <> 'superseded'") + " ORDER BY seq, id" ).c_str() );
   s.Int( 1, imageId );
   std::vector<StepRow> r;
   while ( s.Row() )
      r.push_back( ReadStep( s ) );
   return r;
}

bool JourneyStore::GetStep( int64 stepId, StepRow& out )
{
   Stmt s( *this, ( std::string( "SELECT " ) + kStepCols + " FROM step WHERE id=?" ).c_str() );
   s.Int( 1, stepId );
   if ( !s.Row() )
      return false;
   out = ReadStep( s );
   return true;
}

std::vector<ChannelStats> JourneyStore::Stats( int64 imageId, int64 stepId )
{
   Stmt s( *this, stepId == 0
      ? "SELECT channel, median, mad, mean, min, max, noise FROM stats WHERE image_id=?1 AND step_id IS NULL ORDER BY channel"
      : "SELECT channel, median, mad, mean, min, max, noise FROM stats WHERE image_id=?1 AND step_id=?2 ORDER BY channel" );
   s.Int( 1, imageId );
   if ( stepId != 0 )
      s.Int( 2, stepId );
   std::vector<ChannelStats> r;
   while ( s.Row() )
      r.push_back( { int( s.ColInt( 0 ) ), s.ColReal( 1 ), s.ColReal( 2 ), s.ColReal( 3 ), s.ColReal( 4 ), s.ColReal( 5 ), s.ColReal( 6 ) } );
   return r;
}

bool JourneyStore::Acquisition( int64 imageId, AcquisitionFacts& a )
{
   Stmt s( *this, "SELECT target, filter, camera, gain, offset, sensor_temp, sub_exposure, sub_count, total_integration_s, "
                  "session_date FROM acquisition WHERE image_id=?" );
   s.Int( 1, imageId );
   if ( !s.Row() )
      return false;
   a = AcquisitionFacts();
   a.target = s.ColText( 0 ); a.filter = s.ColText( 1 ); a.camera = s.ColText( 2 );
   if ( !s.IsNull( 3 ) ) a.gain = s.ColReal( 3 );
   if ( !s.IsNull( 4 ) ) a.offset = s.ColReal( 4 );
   if ( !s.IsNull( 5 ) ) a.sensorTempC = s.ColReal( 5 );
   if ( !s.IsNull( 6 ) ) a.subExposureS = s.ColReal( 6 );
   if ( !s.IsNull( 7 ) ) a.subCount = int( s.ColInt( 7 ) );
   if ( !s.IsNull( 8 ) ) a.totalIntegrationS = s.ColReal( 8 );
   a.sessionDate = s.ColText( 9 );
   return true;
}

std::vector<LinkRow> JourneyStore::Links( int64 journeyId )
{
   Stmt s( *this, "SELECT l.from_image_id, l.to_image_id, l.via_step_id, l.evidence FROM link l"
                  " JOIN image i ON i.id = l.to_image_id WHERE i.journey_id=? ORDER BY l.rowid" );
   s.Int( 1, journeyId );
   std::vector<LinkRow> r;
   while ( s.Row() )
      r.push_back( { s.ColInt( 0 ), s.ColInt( 1 ), s.ColInt( 2 ), s.ColText( 3 ) } );
   return r;
}

std::vector<GapRow> JourneyStore::Gaps( int64 journeyId )
{
   Stmt s( *this, "SELECT journey_id, image_id, after_step_seq, reason FROM gap WHERE journey_id=? ORDER BY rowid" );
   s.Int( 1, journeyId );
   std::vector<GapRow> r;
   while ( s.Row() )
      r.push_back( { s.ColInt( 0 ), s.ColInt( 1 ), int( s.ColInt( 2 ) ), s.ColText( 3 ) } );
   return r;
}

int JourneyStore::StepCount( int64 journeyId, bool activeOnly )
{
   Stmt s( *this, activeOnly
      ? "SELECT count(*) FROM step s JOIN image i ON i.id = s.image_id WHERE i.journey_id=? AND s.state='active'"
        " AND coalesce(json_extract(s.params_json,'$.base'),0)=0"
      : "SELECT count(*) FROM step s JOIN image i ON i.id = s.image_id WHERE i.journey_id=? AND s.state<>'superseded'"
        " AND coalesce(json_extract(s.params_json,'$.base'),0)=0" );
   s.Int( 1, journeyId );
   return s.Row() ? int( s.ColInt( 0 ) ) : 0;
}

int JourneyStore::PruneUnkept( const std::string& cutoffIso, StringList* removedDirs )
{
   std::vector<int64> ids;
   {
      Stmt s( *this, "SELECT id FROM journey WHERE kept=0 AND updated < ?" );
      s.Text( 1, cutoffIso );
      while ( s.Row() )
         ids.push_back( s.ColInt( 0 ) );
   }
   // Folder first, then the row: if the folder cannot be removed (it throws,
   // naming the path) the row stays, so the next pass retries both instead of
   // leaving an orphaned folder of thumbnails that no row points to.
   int n = 0;
   for ( int64 id : ids )
   {
      const String dir = JourneyDir( id );
      RemoveDirectoryTree( dir );
      Stmt( *this, "DELETE FROM journey WHERE id=? AND kept=0" ).Int( 1, id ).Run();
      ++n;
      if ( removedDirs != nullptr )
         *removedDirs << dir;
   }
   return n;
}

void JourneyStore::Checkpoint()
{
   Exec( "PRAGMA wal_checkpoint(TRUNCATE)" );
}

} // namespace pcl
