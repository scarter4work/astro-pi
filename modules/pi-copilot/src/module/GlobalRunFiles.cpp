// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "GlobalRunFiles.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FileFormat.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

namespace
{

// Length-aware (Utf8.h): an embedded NUL is kept, never a silent end of the
// text -- NulTextProblem() refuses it with its position.
String S16( const std::string& utf8 )
{
   return FromU8( utf8 );
}

// "<where>: the text contains a NUL character (U+0000) at position N; remove
// it", or "" when `v` is not a string or has no NUL. N counts UTF-16 units.
String NulIn( const String& where, const nlohmann::json& v )
{
   if ( !v.is_string() )
      return String();
   const std::string& s = v.get_ref<const std::string&>();
   if ( s.find( '\0' ) == std::string::npos )
      return String();
   const String t = FromU8( s );
   size_type pos = 0;
   while ( pos < t.Length() && t[pos] != 0 )
      ++pos;
   return where + String().Format( ": the text contains a NUL character (U+0000) at position %u; remove it",
                                   unsigned( pos ) );
}

const nlohmann::json& EmptyObject()
{
   static const nlohmann::json empty = nlohmann::json::object();
   return empty;
}

// What a path resolves to, FOLLOWING symbolic links (as the core does when it
// opens the file): frames in linked folders (e.g. on a NAS) are real frames.
// pcl::FileInfo uses lstat() -- a link to a file is not IsFile() there -- and
// throws on EACCES, so plain POSIX stat() is used instead. Never throws.
struct PathStatus
{
   enum Kind { Ok, Missing, BrokenLink, NoAccess } kind = Missing;
   bool   isFile = false;
   bool   isDirectory = false;
   String reason;   // strerror text for NoAccess
};

PathStatus StatFollowingLinks( const String& path )
{
   PathStatus r;
   const IsoString p = path.ToUTF8();
   struct stat st;
   if ( ::stat( p.c_str(), &st ) == 0 )
   {
      r.kind = PathStatus::Ok;
      r.isFile = S_ISREG( st.st_mode );
      r.isDirectory = S_ISDIR( st.st_mode );
      return r;
   }
   const int e = errno;
   if ( e == ENOENT || e == ENOTDIR )
   {
      struct stat ls;
      r.kind = (::lstat( p.c_str(), &ls ) == 0 && S_ISLNK( ls.st_mode )) ? PathStatus::BrokenLink : PathStatus::Missing;
      return r;
   }
   r.kind = PathStatus::NoAccess;   // EACCES (e.g. an unreadable NAS directory), ELOOP, EIO, ...
   r.reason = String( std::strerror( e ) );
   return r;
}

// The message for a path that does not resolve, or "" when it does.
String ResolveProblem( const String& path, const PathStatus& s )
{
   switch ( s.kind )
   {
   case PathStatus::Missing:    return "'" + path + "' does not exist";
   case PathStatus::BrokenLink: return "'" + path + "' is a broken symbolic link";
   case PathStatus::NoAccess:   return "'" + path + "' cannot be accessed (" + s.reason + ")";
   default:                     return String();
   }
}

String CheckInputFile( const String& path, bool image )
{
   if ( path.StartsWith( '~' ) )
      return "'" + path + "': use an absolute path (PixInsight does not expand ~)";
   if ( !path.StartsWith( '/' ) )
      return "'" + path + "' is not an absolute path";
   const PathStatus st = StatFollowingLinks( path );
   const String problem = ResolveProblem( path, st );
   if ( !problem.IsEmpty() )
      return problem;
   if ( !st.isFile )
      return "'" + path + "' is not a file";
   if ( ::access( path.ToUTF8().c_str(), R_OK ) != 0 )
      return "'" + path + "' is not readable";
   // The format lookup by extension is case-insensitive in PI 1.9.5 (".FITS",
   // ".FIT" resolve; checked by self-test B7).
   if ( image )
   {
      const String ext = File::ExtractExtension( path );
      bool readable = false;
      if ( !ext.IsEmpty() )
         try
         {
            const FileFormat f( ext, true/*toRead*/, false/*toWrite*/ );
            readable = f.CanRead();
         }
         catch ( ... )
         {
         }
      if ( !readable )
         return "'" + path + "' is not an image file PixInsight can read ("
                + (ext.IsEmpty() ? String( "no extension" ) : ext) + ")";
   }
   return String();
}

// '/'-rooted strings must resolve (file or directory, links followed); '~' is
// refused; anything else is not a path.
String CheckLoosePath( const String& where, const nlohmann::json& v )
{
   if ( !v.is_string() )
      return String();
   const String s = S16( v.get<std::string>() );
   if ( s.StartsWith( '~' ) )
      return where + ": '" + s + "': use an absolute path (PixInsight does not expand ~)";
   if ( !s.StartsWith( '/' ) )
      return String();
   const String problem = ResolveProblem( s, StatFollowingLinks( s ) );
   return problem.IsEmpty() ? String() : where + ": " + problem;
}

// The fileTables entry for a canonical process id, or an empty object.
const nlohmann::json& TablesFor( const nlohmann::json& fileTables, const std::string& id )
{
   if ( fileTables.is_object() )
   {
      auto it = fileTables.find( id );
      if ( it != fileTables.end() && it->is_object() )
         return *it;
   }
   return EmptyObject();
}

// The canonical id (ProcessParameter::Id()) a model key resolves to, through
// the core's own lookup (so an ALIAS id resolves to its parameter); "" when
// the key names no parameter of P.
std::string CanonicalParameterId( const Process& P, const std::string& key )
{
   try
   {
      return std::string( ProcessParameter( P, IsoString( key.c_str() ) ).Id().c_str() );
   }
   catch ( ... )
   {
      return std::string();
   }
}

} // namespace

String DuplicateKeyProblem( const Process& P, const nlohmann::json& object, const char* what )
{
   if ( !object.is_object() )
      return String();
   std::map<std::string, std::string> seen;   // canonical -> the first key given for it
   for ( auto it = object.begin(); it != object.end(); ++it )
   {
      const std::string canonical = CanonicalParameterId( P, it.key() );
      if ( canonical.empty() )
         continue;   // unknown key: SetParameters() reports it precisely
      auto hit = seen.find( canonical );
      if ( hit != seen.end() )
      {
         const String pname( P.Id() );
         return pname + "." + S16( hit->second ) + " and " + pname + "." + S16( it.key() ) + " name the same "
                + what + " (" + S16( canonical ) + "); pass it once, as " + S16( canonical );
      }
      seen[canonical] = it.key();
   }
   return String();
}

String NulTextProblem( const IsoString& processId, const nlohmann::json& parameters,
                       const nlohmann::json& tableParameters )
{
   std::unique_ptr<Process> P;
   try
   {
      P.reset( new Process( processId ) );
   }
   catch ( ... )
   {
   }
   const String pname = P ? String( P->Id() ) : String( processId );
   try
   {
      if ( parameters.is_object() )
         for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         {
            const String e = NulIn( pname + "." + S16( it.key() ), it.value() );
            if ( !e.IsEmpty() )
               return e;
         }
      if ( tableParameters.is_object() )
         for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
         {
            if ( !it.value().is_array() )
               continue;
            // Cells are named <table>[row].<column> when the table resolves.
            ProcessParameter::parameter_list cols;
            if ( P )
               try
               {
                  const ProcessParameter tp( *P, IsoString( it.key().c_str() ) );
                  if ( tp.IsTable() )
                     cols = tp.TableColumns();
               }
               catch ( ... )
               {
               }
            const nlohmann::json& rows = it.value();
            for ( size_type r = 0; r < rows.size(); ++r )
               if ( rows[r].is_array() )
                  for ( size_type k = 0; k < rows[r].size(); ++k )
                  {
                     const String where = pname + "." + S16( it.key() )
                                        + (k < cols.Length() ? String().Format( "[%u].", unsigned( r ) ) + String( cols[k].Id() )
                                                             : String().Format( "[%u][%u]", unsigned( r ), unsigned( k ) ));
                     const String e = NulIn( where, rows[r][k] );
                     if ( !e.IsEmpty() )
                        return e;
                  }
         }
   }
   catch ( const pcl::Exception& x )
   {
      return "internal: text validation failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return "internal: text validation failed: " + String( x.what() );
   }
   return String();
}

bool DeclaresFileTables( const IsoString& processId, const nlohmann::json& fileTables )
{
   try
   {
      const Process P( processId );
      return !TablesFor( fileTables, std::string( P.Id().c_str() ) ).empty();
   }
   catch ( ... )
   {
      return false;
   }
}

String ValidateGlobalRunFilePaths( const IsoString& processId, const nlohmann::json& fileTables,
                                   const nlohmann::json& parameters, const nlohmann::json& tableParameters )
{
   std::unique_ptr<Process> resolved;
   try
   {
      resolved.reset( new Process( processId ) );
   }
   catch ( ... )
   {
      return String();   // unknown process: the executor reports it precisely
   }
   const Process& P = *resolved;
   try
   {
      // NUL first, in EVERY string (declared or not, enabled row or not): the
      // core's C strings would silently cut the text there.
      {
         const String e = NulTextProblem( processId, parameters, tableParameters );
         if ( !e.IsEmpty() )
            return e;
      }
      // Table keys resolve through the core (an ALIAS key is its table);
      // canonical + alias together are refused, never "last one wins".
      {
         const String e = DuplicateKeyProblem( P, tableParameters, "table" );
         if ( !e.IsEmpty() )
            return e;
      }
      std::map<std::string, std::string> givenKey;   // canonical table id -> the key the model used
      if ( tableParameters.is_object() )
         for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
         {
            const std::string canonical = CanonicalParameterId( P, it.key() );
            if ( !canonical.empty() )
               givenKey[canonical] = it.key();
         }

      const std::string id( P.Id().c_str() );
      const String pname( P.Id() );
      const nlohmann::json& tables = TablesFor( fileTables, id );
      for ( auto t = tables.begin(); t != tables.end(); ++t )
      {
         const std::string table = t.key();
         const nlohmann::json& rule = t.value();
         const String tname = pname + "." + S16( table );
         std::unique_ptr<ProcessParameter> tpp;
         try
         {
            tpp.reset( new ProcessParameter( P, IsoString( table.c_str() ) ) );
         }
         catch ( ... )
         {
         }
         if ( !tpp || !tpp->IsTable() )
            return "internal: the file-table policy names " + tname + ", which is not a table parameter of " + pname;
         if ( std::string( tpp->Id().c_str() ) != table )
            return "internal: the file-table policy names " + tname + " by an alias; use its canonical id "
                   + pname + "." + String( tpp->Id() );
         const ProcessParameter& tp = *tpp;
         const ProcessParameter::parameter_list cols = tp.TableColumns();
         std::map<std::string, size_type> index;
         String columnList;
         for ( size_type k = 0; k < cols.Length(); ++k )
         {
            index[std::string( cols[k].Id().c_str() )] = k;
            columnList += (k > 0 ? String( ", " ) : String()) + String( cols[k].Id() );
         }
         auto given = givenKey.find( table );
         if ( given == givenKey.end() )
            return tname + " is required: pass table_parameters." + S16( table ) + " as rows [" + columnList + "]";
         const nlohmann::json& rows = tableParameters[given->second];
         if ( !rows.is_array() )
            return String();   // SetParameters() reports the shape
         const std::string enabledCol = rule.value( "enabledColumn", std::string() );
         if ( !enabledCol.empty() && !index.count( enabledCol ) )
            return "internal: the file-table policy names " + S16( enabledCol ) + " as the enabled column of " + tname
                   + ", which has no such column";
         const bool hasEnabled = !enabledCol.empty();
         const nlohmann::json& columns = rule.contains( "columns" ) ? rule["columns"] : EmptyObject();
         size_type enabled = 0;
         for ( size_type r = 0; r < rows.size(); ++r )
         {
            const nlohmann::json& row = rows[r];
            if ( !row.is_array() || row.size() != cols.Length() )
               return String();   // SetParameters() reports the shape
            if ( hasEnabled )
            {
               const nlohmann::json& e = row[index[enabledCol]];
               if ( !(e.is_boolean() && e.get<bool>()) )
                  continue;
            }
            ++enabled;
            for ( auto c = columns.begin(); c != columns.end(); ++c )
            {
               if ( !index.count( c.key() ) )
                  return "internal: policy column " + S16( c.key() ) + " is not a column of " + tname;
               const nlohmann::json& cell = row[index[c.key()]];
               const bool optional = c.value() == "optionalFile";
               const String where = tname + String().Format( "[%u].", unsigned( r ) ) + S16( c.key() );
               if ( cell.is_null() && optional )
                  continue;
               if ( !cell.is_string() )
                  return where + ": expected a file path string";
               const String path = S16( cell.get<std::string>() );
               if ( path.IsEmpty() )
               {
                  if ( optional )
                     continue;
                  return where + ": a file path is required";
               }
               const String e = CheckInputFile( path, c.value() == "image" );
               if ( !e.IsEmpty() )
                  return where + ": " + e;
            }
         }
         const size_type minRows = rule.value( "minEnabledRows", size_type( 0 ) );
         if ( enabled < minRows )
            return tname + String().Format( ": %u enabled rows; at least %u are required",
                                            unsigned( enabled ), unsigned( minRows ) );
      }
      if ( parameters.is_object() )
         for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         {
            const String e = CheckLoosePath( pname + "." + S16( it.key() ), it.value() );
            if ( !e.IsEmpty() )
               return e;
         }
      if ( tableParameters.is_object() )
         for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
         {
            const std::string canonical = CanonicalParameterId( P, it.key() );
            if ( !canonical.empty() && tables.contains( canonical ) )
               continue;   // a declared table: checked above
            if ( it.value().is_array() )
               for ( size_type r = 0; r < it.value().size(); ++r )
                  if ( it.value()[r].is_array() )
                     for ( size_type k = 0; k < it.value()[r].size(); ++k )
                     {
                        const String e = CheckLoosePath( pname + "." + S16( it.key() )
                                                         + String().Format( "[%u][%u]", unsigned( r ), unsigned( k ) ),
                                                         it.value()[r][k] );
                        if ( !e.IsEmpty() )
                           return e;
                     }
         }
   }
   catch ( const pcl::Exception& x )
   {
      return "internal: file path validation failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return "internal: file path validation failed: " + String( x.what() );
   }
   return String();
}

} // namespace pcl
