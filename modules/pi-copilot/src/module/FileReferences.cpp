// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "FileReferences.h"
#include "HistoryReader.h"   // ParseXpsmStep
#include "JourneyStore.h"
#include "JourneyTools.h"    // ModelTextWithoutDirectories
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>

#include <map>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

std::string UsableFilePath( const std::string& path )
{
   if ( path.empty() || path[0] != '/' || path.find( '\0' ) != std::string::npos )
      return std::string();
   struct stat st;
   if ( ::stat( path.c_str(), &st ) != 0 || !S_ISREG( st.st_mode ) || ::access( path.c_str(), R_OK ) != 0 )
      return std::string();
   return path;
}

bool FindKnownFile( const std::string& name, const std::vector<FileCandidate>& candidates, FileCandidate& out )
{
   if ( name.empty() )
      return false;
   for ( const FileCandidate& c : candidates )
      if ( PathFileName( c.path ) == name && !UsableFilePath( c.path ).empty() )
      {
         out = c;
         return true;
      }
   return false;
}

void AddFileCandidates( std::vector<FileCandidate>& to, const std::vector<FileParameterValue>& values, const std::string& foundIn )
{
   for ( const FileParameterValue& v : values )
      if ( !v.value.empty() && v.value[0] == '/' )
         to.push_back( FileCandidate{ v.value, foundIn } );
}

void AddDefaultInstanceFileCandidates( std::vector<FileCandidate>& to, const std::string& processId )
{
   try
   {
      std::string xpsm;
      {
         const Process P( IsoString( processId.c_str() ) );
         const ProcessInstance defaults( P );   // a temporary: never kept
         if ( defaults.IsNull() )
            return;
         xpsm = U8( defaults.ToSource( "XPSM 1.0" ) );
      }
      HistoryStep h;
      String e;
      if ( !ParseXpsmStep( xpsm, h, e ) )
         return;
      AddFileCandidates( to, FileParameterValues( processId, h.parameters, h.tableParameters ), processId + "'s default settings" );
   }
   catch ( ... )
   {
      // no default instance or unreadable metadata: nothing to add (the resolution names every place it looked)
   }
}

void AddLibraryFileCandidates( std::vector<FileCandidate>& to, JourneyStore& store )
{
   try
   {
      for ( const JourneyRow& j : store.ListJourneys( false, std::string(), 500 ) )
         for ( const ImageRow& i : store.Images( j.id ) )
            for ( const StepRow& s : store.Steps( i.id, true ) )
            {
               const nlohmann::json p = s.params.value( "parameters", nlohmann::json::object() );
               const nlohmann::json t = s.params.value( "tableParameters", nlohmann::json::object() );
               if ( p.dump().find( '/' ) == std::string::npos && t.dump().find( '/' ) == std::string::npos )
                  continue;
               try
               {
                  AddFileCandidates( to, FileParameterValues( s.processId, p, t ), "a step of journey #" + std::to_string( j.id ) );
               }
               catch ( ... )
               {
                  // this step's table columns cannot be read: it offers no candidate
               }
            }
   }
   catch ( ... )
   {
      // the library cannot be read: no candidates from it
   }
}

nlohmann::json FileReferenceJson( const char* key, const std::string& name, bool available, const std::string& foundIn )
{
   nlohmann::json r = { { key, name }, { "available", available } };
   if ( available && !foundIn.empty() )
      r["foundIn"] = foundIn;
   return r;
}

bool IsFileReference( const nlohmann::json& v, std::string& name )
{
   if ( !v.is_object() )
      return false;
   for ( const char* key : { "file", "recorded_file" } )
      if ( v.contains( key ) && v[key].is_string() )
      {
         name = v[key].get<std::string>();
         return true;
      }
   return false;
}

std::string CanonicalProcessIdOf( const std::string& processId )
{
   try
   {
      return std::string( Process( IsoString( processId.c_str() ) ).Id().c_str() );
   }
   catch ( ... )
   {
      return std::string();
   }
}

String SubstituteFileReferences( const std::string& pid, nlohmann::json& parameters, nlohmann::json& tableParameters,
                                 const std::vector<FileParameterValue>* recorded, const nlohmann::json& recordedTables,
                                 const std::vector<FileCandidate>& known, std::vector<FileSubstitution>& subs )
{
   subs.clear();
   auto hasDir = []( const std::string& s ) { return s.find_first_of( "/\\" ) != std::string::npos; };
   // Resolves `name` for `where`; "" + sub pushed, or the error for the model.
   auto resolve = [&]( const std::string& name, const std::string& where, std::string& path ) -> String
   {
      if ( name.empty() || hasDir( name ) )
         return FromU8( pid + "." + where + ": a file reference names a file by its name only, without a folder (e.g. {\"file\": \""
                        + PathFileName( name ) + "\"})" );
      FileCandidate c;
      if ( !FindKnownFile( name, known, c ) )
         return FromU8( "the file " + name + " (" + pid + "." + where + ") was not found on this machine: not at the recorded "
                        "location (replay steps), in a workspace process icon, in " + pid + "'s default settings, or in another "
                        "recorded journey step. Nothing ran. Tell the user which file is needed; they can put a process icon "
                        "that uses it on the workspace, or set this step up themselves." );
      path = c.path;
      subs.push_back( FileSubstitution{ where, name, c.path, c.foundIn } );
      return String();
   };
   auto refuseDir = [&]( const std::string& where, const std::string& value ) -> String
   {
      const std::string name = PathFileName( value );
      return FromU8( pid + "." + where + " of a replay step is set by PI Copilot from the recorded file: omit it, or pass "
                     "{\"recorded_file\": \"" + name + "\"}. A folder path is never taken from you here." );
   };
   std::set<std::string> recParams, recTables;
   if ( recorded != nullptr )
      for ( const FileParameterValue& v : *recorded )
         (v.inTable ? recTables : recParams).insert( v.parameter );

   // Scalar parameters the model gave.
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
      {
         std::string name;
         if ( IsFileReference( it.value(), name ) )
         {
            if ( !IsFileParameter( pid, it.key() ) )
               return FromU8( pid + "." + it.key() + " is not a file parameter; a {\"file\": ...} reference only goes in one" );
            std::string path;
            const String e = resolve( name, it.key(), path );
            if ( !e.IsEmpty() )
               return e;
            it.value() = path;
         }
         else if ( recParams.count( it.key() ) && it.value().is_string() && !it.value().get_ref<const std::string&>().empty() )
         {
            const std::string v = it.value().get<std::string>();
            if ( hasDir( v ) )
               return refuseDir( it.key(), v );
            std::string path;
            const String e = resolve( v, it.key(), path );
            if ( !e.IsEmpty() )
               return e;
            it.value() = path;
         }
      }
   // Recorded scalar file parameters the model omitted.
   if ( recorded != nullptr )
      for ( const FileParameterValue& v : *recorded )
         if ( !v.inTable && !(parameters.is_object() && parameters.contains( v.parameter )) )
         {
            if ( !parameters.is_object() )
               parameters = nlohmann::json::object();
            std::string path;
            const String e = resolve( PathFileName( v.value ), v.Where(), path );
            if ( !e.IsEmpty() )
               return e;
            parameters[v.parameter] = path;
         }
   // Tables the model gave.
   if ( tableParameters.is_object() )
      for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
      {
         if ( !it.value().is_array() )
            continue;
         const std::string& tid = it.key();
         std::vector<std::string> cols;
         try
         {
            cols = ProcessTableColumnIds( pid, tid );
         }
         catch ( const pcl::Exception& x )
         {
            return FromU8( pid + "." + tid + ": its columns could not be read from the process catalog (" ) + x.Message() + ")";
         }
         const bool tableIsFile = IsFileParameter( pid, tid );
         for ( size_t r = 0; r < it.value().size(); ++r )
         {
            nlohmann::json& row = it.value()[r];
            if ( !row.is_array() )
               continue;
            for ( size_t k = 0; k < row.size(); ++k )
            {
               const bool known = k < cols.size();
               const bool fileCell = tableIsFile || (known && IsFileParameter( pid, tid + "." + cols[k] ));
               const std::string where = tid + "[" + std::to_string( r ) + "]" + (known ? "." + cols[k] : "[" + std::to_string( k ) + "]");
               std::string name;
               if ( IsFileReference( row[k], name ) )
               {
                  if ( !fileCell )
                     return FromU8( pid + "." + where + " is not a file column; a {\"file\": ...} reference only goes in one" );
                  std::string path;
                  const String e = resolve( name, where, path );
                  if ( !e.IsEmpty() )
                     return e;
                  row[k] = path;
               }
               else if ( recTables.count( tid ) && fileCell && row[k].is_string() && !row[k].get_ref<const std::string&>().empty() )
               {
                  const std::string v = row[k].get<std::string>();
                  if ( hasDir( v ) )
                     return refuseDir( where, v );
                  std::string path;
                  const String e = resolve( v, where, path );
                  if ( !e.IsEmpty() )
                     return e;
                  row[k] = path;
               }
            }
         }
      }
   // Recorded file tables the model omitted: the whole recorded table, its file cells resolved.
   if ( recorded != nullptr )
      for ( const std::string& tid : recTables )
      {
         if ( tableParameters.is_object() && tableParameters.contains( tid ) )
            continue;
         if ( !recordedTables.is_object() || !recordedTables.contains( tid ) || !recordedTables[tid].is_array() )
            continue;
         nlohmann::json rows = recordedTables[tid];
         for ( const FileParameterValue& v : *recorded )
         {
            if ( !v.inTable || v.parameter != tid || v.row >= rows.size() || !rows[v.row].is_array() || v.column >= rows[v.row].size() )
               continue;
            std::string path;
            const String e = resolve( PathFileName( v.value ), v.Where(), path );
            if ( !e.IsEmpty() )
               return e;
            rows[v.row][v.column] = path;
         }
         if ( !tableParameters.is_object() )
            tableParameters = nlohmann::json::object();
         tableParameters[tid] = rows;
      }
   return String();
}

String FileSubstitutionLogText( const std::vector<FileSubstitution>& subs )
{
   String s;
   for ( const FileSubstitution& f : subs )
      s += FromU8( " [" + f.where + " = " + f.path + ", found in " + f.foundIn + ", set by PI Copilot]" );
   return s;
}

String WithoutSubstitutedPaths( const String& text, const std::vector<FileSubstitution>& subs )
{
   String t = text;
   for ( const FileSubstitution& f : subs )
      t.ReplaceString( FromU8( f.path ), FromU8( f.name ) );
   return ModelTextWithoutDirectories( t );
}

nlohmann::json RedactSubstitutedPaths( const nlohmann::json& v, const std::vector<FileSubstitution>& subs )
{
   if ( v.is_string() )
   {
      for ( const FileSubstitution& f : subs )
         if ( v.get_ref<const std::string&>() == f.path )
            return f.name;
      return v;
   }
   if ( v.is_array() )
   {
      nlohmann::json a = nlohmann::json::array();
      for ( const nlohmann::json& e : v )
         a.push_back( RedactSubstitutedPaths( e, subs ) );
      return a;
   }
   if ( v.is_object() )
   {
      nlohmann::json o = nlohmann::json::object();
      for ( auto it = v.begin(); it != v.end(); ++it )
         o[it.key()] = RedactSubstitutedPaths( it.value(), subs );
      return o;
   }
   return v;
}

} // namespace pcl
