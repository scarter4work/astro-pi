// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "GlobalRunFiles.h"

#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FileFormat.h>
#include <pcl/FileInfo.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>

#include <map>
#include <memory>
#include <set>
#include <string>

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

String CheckInputFile( const String& path, bool image )
{
   if ( path.StartsWith( '~' ) )
      return "'" + path + "': use an absolute path (PixInsight does not expand ~)";
   if ( !path.StartsWith( '/' ) )
      return "'" + path + "' is not an absolute path";
   const FileInfo fi( path );
   if ( !fi.Exists() )
      return "'" + path + "' does not exist";
   if ( !fi.IsFile() )
      return "'" + path + "' is not a file";
   if ( !fi.IsReadable() )
      return "'" + path + "' is not readable";
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

// '/'-rooted strings must exist; '~' is refused; anything else is not a path.
String CheckLoosePath( const String& where, const nlohmann::json& v )
{
   if ( !v.is_string() )
      return String();
   const String s = S16( v.get<std::string>() );
   if ( s.StartsWith( '~' ) )
      return where + ": '" + s + "': use an absolute path (PixInsight does not expand ~)";
   if ( s.StartsWith( '/' ) && !File::Exists( s ) && !File::DirectoryExists( s ) )
      return where + ": '" + s + "' does not exist";
   return String();
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
         const bool hasEnabled = index.count( enabledCol ) > 0;
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
