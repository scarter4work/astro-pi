// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "GlobalRunFiles.h"

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

String S16( const std::string& utf8 )
{
   return String::UTF8ToUTF16( utf8.c_str() );
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

} // namespace

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
      const std::string id( P.Id().c_str() );
      const String pname( P.Id() );
      std::set<std::string> declared;
      const nlohmann::json& tables = TablesFor( fileTables, id );
      for ( auto t = tables.begin(); t != tables.end(); ++t )
      {
         const std::string table = t.key();
         const nlohmann::json& rule = t.value();
         declared.insert( table );
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
         const ProcessParameter& tp = *tpp;
         const ProcessParameter::parameter_list cols = tp.TableColumns();
         std::map<std::string, size_type> index;
         String columnList;
         for ( size_type k = 0; k < cols.Length(); ++k )
         {
            index[std::string( cols[k].Id().c_str() )] = k;
            columnList += (k > 0 ? String( ", " ) : String()) + String( cols[k].Id() );
         }
         if ( !tableParameters.is_object() || !tableParameters.contains( table ) )
            return tname + " is required: pass table_parameters." + S16( table ) + " as rows [" + columnList + "]";
         const nlohmann::json& rows = tableParameters[table];
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
            if ( !declared.count( it.key() ) && it.value().is_array() )
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

const nlohmann::json& PhaseAFileTables()
{
   static const nlohmann::json tables = nlohmann::json::parse( R"JSON(
    {
      "ImageIntegration": {
        "images": {
          "enabledColumn": "enabled",
          "minEnabledRows": 3,
          "columns": { "path": "image", "drizzlePath": "optionalFile", "localNormalizationDataPath": "optionalFile" }
        }
      }
    }
   )JSON" );
   return tables;
}

} // namespace pcl
