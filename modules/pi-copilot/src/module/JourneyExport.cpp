// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneyExport.h"
#include "MasterFacts.h"
#include "RecipeSchemaData.h"
#include "SafeFileWrite.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/XML.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace pcl
{

namespace
{

const char* const kStatsBasis =
   "Per-channel statistics of a read-only block-averaged copy of the image (long edge <= 2048 px). "
   "noise = 1.4826 * MAD of the 4-neighbour Laplacian residual / sqrt(1.25), rescaled to full resolution.";

std::string Key( int64 imageId )
{
   return "img" + std::to_string( imageId );
}

// The file name of a path: after the last '/' or '\' (a Windows path recorded
// on another machine is stripped the same way).
std::string FileNameOf( const std::string& path )
{
   const size_t p = path.find_last_of( "/\\" );
   return p == std::string::npos ? path : path.substr( p + 1 );
}

bool LooksLikeAbsolutePath( const std::string& s )
{
   if ( s.empty() || s.find_first_of( "\r\n" ) != std::string::npos )
      return false;
   if ( s[0] == '/' )
      return s.size() == 1 || (s[1] != '*' && s[1] != '/');   // "/*" and "//" open PixelMath comments
   if ( s[0] == '~' )
      return s.size() == 1 || s[1] == '/';                    // "~$T" is PixelMath's inversion
   return s.size() > 2 && std::isalpha( static_cast<unsigned char>( s[0] ) ) && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
}

// The first string in v that is an absolute path: its location ("filePath",
// "H[3][1]", "images[0].path") and its file name. False when there is none.
bool FindPath( const nlohmann::json& v, const std::string& at, std::string& where, std::string& name )
{
   if ( v.is_string() )
   {
      if ( !LooksLikeAbsolutePath( v.get<std::string>() ) )
         return false;
      where = at;
      name = FileNameOf( v.get<std::string>() );
      return true;
   }
   if ( v.is_array() )
   {
      for ( size_t i = 0; i < v.size(); ++i )
         if ( FindPath( v[i], at + "[" + std::to_string( i ) + "]", where, name ) )
            return true;
      return false;
   }
   if ( v.is_object() )
      for ( auto it = v.begin(); it != v.end(); ++it )
         if ( FindPath( it.value(), at.empty() ? it.key() : at + "." + it.key(), where, name ) )
            return true;
   return false;
}

bool PathParameter( const StepRow& s, std::string& where, std::string& name )
{
   return FindPath( s.params.value( "parameters", nlohmann::json::object() ), "", where, name )
       || FindPath( s.params.value( "tableParameters", nlohmann::json::object() ), "", where, name );
}

nlohmann::json StatsJson( const std::vector<ChannelStats>& s )
{
   if ( s.empty() )
      return nullptr;
   nlohmann::json a = nlohmann::json::array();
   for ( const ChannelStats& c : s )
      a.push_back( { { "channel", c.channel }, { "median", c.median }, { "mad", c.mad }, { "mean", c.mean },
                     { "min", c.min }, { "max", c.max }, { "noise", c.noise } } );
   return a;
}

nlohmann::json Opt( const std::optional<double>& v ) { return v ? nlohmann::json( *v ) : nlohmann::json(); }
nlohmann::json Opt( const std::optional<int>& v )    { return v ? nlohmann::json( *v ) : nlohmann::json(); }

std::string EscapeHtml( const std::string& s )
{
   std::string r;
   for ( char c : s )
      r += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : std::string( 1, c );
   return r;
}

// Text for an XML comment: escaped, no control characters XML 1.0 forbids, and
// no "--" (not allowed inside a comment).
std::string CommentText( const std::string& s )
{
   std::string r;
   for ( char c : EscapeHtml( s ) )
      r += (static_cast<unsigned char>( c ) < 0x20 && c != '\t' && c != '\n' && c != '\r') ? ' ' : c;
   for ( size_t p = r.find( "--" ); p != std::string::npos; p = r.find( "--", p ) )
      r.replace( p, 2, "- -" );
   return r;
}

// 300 -> "300", 0.5 -> "0.5".
std::string Seconds( double v )
{
   char b[ 64 ];
   std::snprintf( b, sizeof( b ), "%g", v );
   return b;
}

bool IsBase( const StepRow& s )
{
   return s.params.value( "base", false );
}

std::string ThumbRel( JourneyStore& store, int64 journeyId, const String& name )
{
   return File::Exists( store.JourneyDir( journeyId ) + "/thumbs/" + name ) ? "thumbs/" + U8( name ) : std::string();
}

// Masters first (by id), then the other images in the order they joined.
std::vector<ImageRow> OrderedImages( JourneyStore& store, int64 journeyId )
{
   std::vector<ImageRow> v = store.Images( journeyId );
   std::stable_sort( v.begin(), v.end(), []( const ImageRow& a, const ImageRow& b ) { return a.isMaster > b.isMaster; } );
   return v;
}

bool IsType( const nlohmann::json& v, const char* t )
{
   const std::string s( t );
   return (s == "string" && v.is_string()) || (s == "integer" && v.is_number_integer()) || (s == "number" && v.is_number())
       || (s == "boolean" && v.is_boolean()) || (s == "object" && v.is_object()) || (s == "array" && v.is_array())
       || (s == "null" && v.is_null());
}

bool Need( const nlohmann::json& o, const std::string& path, std::initializer_list<const char*> keys, std::string& why )
{
   if ( !o.is_object() )
   {
      why = (path.empty() ? std::string( "recipe" ) : path) + ": not an object";
      return false;
   }
   for ( const char* k : keys )
      if ( !o.contains( k ) )
      {
         why = path + (path.empty() ? "" : ".") + k + ": missing";
         return false;
      }
   return true;
}

bool Typed( const nlohmann::json& v, const std::string& path, std::initializer_list<const char*> types, std::string& why )
{
   for ( const char* t : types )
      if ( IsType( v, t ) )
         return true;
   why = path + ": wrong type";
   return false;
}

bool StatsOk( const nlohmann::json& v, const std::string& path, std::string& why )
{
   if ( v.is_null() )
      return true;
   if ( !v.is_array() || v.empty() )
   {
      why = path + ": must be null or a non-empty array";
      return false;
   }
   for ( size_t i = 0; i < v.size(); ++i )
   {
      const std::string p = path + "[" + std::to_string( i ) + "]";
      if ( !Need( v[i], p, { "channel", "median", "mad", "mean", "min", "max", "noise" }, why ) )
         return false;
      const nlohmann::json& ch = v[i]["channel"];
      if ( !ch.is_number_integer() || (ch.is_number_unsigned() ? false : ch.get<int64>() < 0) )
      {
         why = p + ".channel: not an integer >= 0";
         return false;
      }
      for ( const char* k : { "median", "mad", "mean", "min", "max", "noise" } )
         if ( !v[i][k].is_number() )
         {
            why = p + "." + k + ": not a number";
            return false;
         }
   }
   return true;
}

// $defs/acquisition: every field present and typed.
bool AcquisitionOk( const nlohmann::json& a, const std::string& p, std::string& why )
{
   if ( !Need( a, p, { "target", "filter", "camera", "gain", "offset", "sensorTempC", "subExposureS", "subCount",
                       "totalIntegrationS", "sessionDate" }, why ) )
      return false;
   for ( const char* k : { "target", "filter", "camera", "sessionDate" } )
      if ( !Typed( a[k], p + "." + k, { "string" }, why ) )
         return false;
   for ( const char* k : { "gain", "offset", "sensorTempC", "subExposureS", "totalIntegrationS" } )
      if ( !Typed( a[k], p + "." + k, { "number", "null" }, why ) )
         return false;
   return Typed( a["subCount"], p + ".subCount", { "integer", "null" }, why );
}

// steps[].achieved (non-null): { "median": { "from": [numbers], "to": [numbers] } }.
bool AchievedOk( const nlohmann::json& a, const std::string& p, std::string& why )
{
   if ( !Need( a, p, { "median" }, why ) || !Need( a["median"], p + ".median", { "from", "to" }, why ) )
      return false;
   for ( const char* k : { "from", "to" } )
   {
      const nlohmann::json& v = a["median"][k];
      const std::string q = p + ".median." + k;
      if ( !v.is_array() )
      {
         why = q + ": not an array";
         return false;
      }
      for ( const nlohmann::json& e : v )
         if ( !e.is_number() )
         {
            why = q + ": not an array of numbers";
            return false;
         }
   }
   return true;
}

bool KeyOk( const nlohmann::json& v, const std::set<std::string>& keys, const std::string& path, std::string& why, bool nullable )
{
   if ( nullable && v.is_null() )
      return true;
   if ( !v.is_string() || keys.count( v.get<std::string>() ) == 0 )
   {
      why = path + ": not the key of an image in this recipe";
      return false;
   }
   return true;
}

std::string Errno( int e )
{
   return std::strerror( e );
}

// A folder below the user's export folder: made 0755 (umask applied) when
// missing; an existing name must be a real directory, never a symbolic link.
String MakeCopyDirectory( const String& dir )
{
   const std::string p = U8( dir );
   if ( ::mkdir( p.c_str(), 0755 ) != 0 && errno != EEXIST )
      return "cannot create folder " + dir + ": " + FromU8( Errno( errno ) );
   struct stat st;
   if ( ::lstat( p.c_str(), &st ) != 0 )
      return "cannot inspect folder " + dir + ": " + FromU8( Errno( errno ) );
   if ( S_ISLNK( st.st_mode ) )
      return dir + " is a symbolic link; PI Copilot does not follow it";
   if ( !S_ISDIR( st.st_mode ) )
      return dir + " exists and is not a folder";
   return String();
}

// Copies the regular files and folders of `from` (ours) into `to`. Links in
// the source are skipped, never followed; each file goes through
// SafeWriteFile (Shared: never through a link at the target, never a partial
// file). makeDir makes each destination folder ("" = ready). Throws pcl::Error
// naming the path that failed.
void CopyTree( const String& from, const String& to, const std::function<String( const String& )>& makeDir )
{
   const String made = makeDir( to );
   if ( !made.IsEmpty() )
      throw Error( made );
   DIR* d = ::opendir( U8( from ).c_str() );
   if ( d == nullptr )
      throw Error( "cannot read folder " + from + ": " + FromU8( Errno( errno ) ) );
   std::vector<std::string> files, dirs;
   for ( const dirent* e; (e = ::readdir( d )) != nullptr; )
   {
      const std::string n = e->d_name;
      if ( n == "." || n == ".." )
         continue;
      struct stat st;
      if ( ::lstat( (U8( from ) + "/" + n).c_str(), &st ) != 0 )
         continue;
      if ( S_ISREG( st.st_mode ) )
         files.push_back( n );
      else if ( S_ISDIR( st.st_mode ) )
         dirs.push_back( n );
   }
   ::closedir( d );
   std::sort( files.begin(), files.end() );
   std::sort( dirs.begin(), dirs.end() );
   for ( const std::string& n : files )
   {
      const String src = from + "/" + FromU8( n );
      const String dst = to + "/" + FromU8( n );
      const String e = SafeWriteFile( dst, File::ReadFile( src ), SafeFileMode::Shared );
      if ( !e.IsEmpty() )
         throw Error( "could not write " + dst + ": " + e );
   }
   for ( const std::string& n : dirs )
      CopyTree( from + "/" + FromU8( n ), to + "/" + FromU8( n ), makeDir );
}

String WhatOf( const std::exception& x )
{
   return String( x.what() );
}

} // namespace

bool IsManualProcess( const std::string& id )
{
   static const std::set<std::string> manual = { "DynamicBackgroundExtraction", "DynamicCrop", "DynamicAlignment",
                                                 "CloneStamp", "GradientsMergeMosaic" };
   return manual.count( id ) > 0;
}

std::string ManualWhy( const StepRow& s )
{
   if ( s.processId == "Script" )
   {
      const nlohmann::json p = s.params.value( "parameters", nlohmann::json::object() );
      const std::string file = p.contains( "filePath" ) && p["filePath"].is_string() && !p["filePath"].get<std::string>().empty()
                             ? FileNameOf( p["filePath"].get<std::string>() )
                             : std::string( "a script" );
      return "a script step: run " + file + " yourself (PI Copilot never re-runs scripts)";
   }
   if ( IsManualProcess( s.processId ) )
      return s.processId + " needs your hand (sample points or interactive geometry): set it up yourself, then continue";
   std::string where, name;
   if ( PathParameter( s, where, name ) )
      return "parameter " + where + " names a file (" + name + "); only its name is kept, so point it at your own copy "
             "and run this step yourself";
   if ( s.params.contains( "mask" ) && s.params["mask"].is_object() )
      return "applied through mask " + s.params["mask"].value( "id", std::string() )
           + (s.params["mask"].value( "inverted", false ) ? " (inverted)" : "")
           + ": make that mask for the new image and apply the step through it";
   if ( !s.params.value( "replayable", true ) )
   {
      const std::string note = s.params.value( "parseNote", std::string() );
      return note.empty() ? std::string( "cannot be replayed" ) : note;
   }
   return std::string();
}

nlohmann::json PrivacyStripPaths( const nlohmann::json& v )
{
   if ( v.is_string() )
      return LooksLikeAbsolutePath( v.get<std::string>() ) ? nlohmann::json( FileNameOf( v.get<std::string>() ) ) : v;
   if ( v.is_array() )
   {
      nlohmann::json a = nlohmann::json::array();
      for ( const nlohmann::json& e : v )
         a.push_back( PrivacyStripPaths( e ) );
      return a;
   }
   if ( v.is_object() )
   {
      nlohmann::json o = nlohmann::json::object();
      for ( auto it = v.begin(); it != v.end(); ++it )
         o[it.key()] = PrivacyStripPaths( it.value() );
      return o;
   }
   return v;
}

KeeperSummary BuildKeeperSummary( JourneyStore& store, int64 journeyId )
{
   KeeperSummary k;
   k.journeyId = journeyId;
   JourneyRow j;
   if ( !store.GetJourney( journeyId, j ) )
      throw Error( String().Format( "no journey #%lld", static_cast<long long>( journeyId ) ) );
   k.name = j.name;
   k.target = j.target;
   k.alreadyKept = j.kept;
   std::map<int64, std::string> viewOf;
   for ( const ImageRow& i : OrderedImages( store, journeyId ) )
   {
      viewOf[i.id] = i.viewId;
      ++k.images;
      if ( i.isMaster )
      {
         ++k.masters;
         AcquisitionFacts a;
         std::string line = i.viewId;
         if ( store.Acquisition( i.id, a ) )
         {
            line = (a.target.empty() ? i.viewId : a.target) + " (" + (a.filter.empty() ? std::string( "no filter" ) : a.filter);
            if ( a.subCount && a.subExposureS )
               line += ", " + std::to_string( *a.subCount ) + " x " + Seconds( *a.subExposureS ) + " s";
            line += ")";
         }
         k.masterLines.push_back( line );
      }
      for ( const StepRow& s : store.Steps( i.id, false ) )
         if ( s.state == "active" && !IsBase( s ) )
         {
            ++k.steps;
            if ( s.actor == "copilot" )
               ++k.copilotSteps;
         }
   }
   auto view = [&viewOf]( int64 id ) { auto it = viewOf.find( id ); return it == viewOf.end() ? std::string( "?" ) : it->second; };
   for ( const LinkRow& l : store.Links( journeyId ) )
      k.linkLines.push_back( view( l.fromImageId ) + " -> " + view( l.toImageId ) + " (linked by " + l.evidence + ")" );
   for ( const GapRow& g : store.Gaps( journeyId ) )
      k.gapLines.push_back( "after step " + std::to_string( g.afterSeq ) + " of "
                          + (g.imageId == 0 ? std::string( "the journey" ) : view( g.imageId )) + ": " + g.reason );
   return k;
}

String KeeperSummaryHtml( const KeeperSummary& s )
{
   std::string h = "<p><b>Keep the journey \xE2\x80\x9C" + EscapeHtml( s.name ) + "\xE2\x80\x9D?</b></p>";
   h += "<p>Masters: " + std::to_string( s.masters );
   for ( const std::string& m : s.masterLines )
      h += "<br/>&nbsp;&nbsp;" + EscapeHtml( m );
   h += "</p><p>Steps: " + std::to_string( s.steps ) + " (" + std::to_string( s.steps - s.copilotSteps ) + " by you, "
      + std::to_string( s.copilotSteps ) + " by PI Copilot) across " + std::to_string( s.images ) + " images</p>";
   h += "<p>Links: " + (s.linkLines.empty() ? std::string( "none" ) : std::string());
   for ( const std::string& l : s.linkLines )
      h += "<br/>&nbsp;&nbsp;" + EscapeHtml( l );
   h += "</p><p>Gaps (steps that could not be recorded): " + (s.gapLines.empty() ? std::string( "none" ) : std::string());
   for ( const std::string& g : s.gapLines )
      h += "<br/>&nbsp;&nbsp;" + EscapeHtml( g );
   h += "</p><p>Keeping writes a process icon set (.xpsm), recipe.json and a write-up (journey.md), and copies them to "
        "your export folder if one is set in the settings. Kept journeys are never deleted automatically.</p>";
   return FromU8( h );
}

nlohmann::json BuildRecipe( JourneyStore& store, int64 journeyId, const std::string& generator )
{
   JourneyRow j;
   if ( !store.GetJourney( journeyId, j ) )
      throw Error( String().Format( "no journey #%lld", static_cast<long long>( journeyId ) ) );
   nlohmann::json images = nlohmann::json::array(), steps = nlohmann::json::array(), links = nlohmann::json::array(),
                  gaps = nlohmann::json::array();
   for ( const ImageRow& i : OrderedImages( store, journeyId ) )
   {
      AcquisitionFacts a;
      nlohmann::json acq = nullptr;
      if ( i.isMaster && store.Acquisition( i.id, a ) )
         acq = { { "target", a.target }, { "filter", a.filter }, { "camera", a.camera }, { "gain", Opt( a.gain ) },
                 { "offset", Opt( a.offset ) }, { "sensorTempC", Opt( a.sensorTempC ) }, { "subExposureS", Opt( a.subExposureS ) },
                 { "subCount", Opt( a.subCount ) }, { "totalIntegrationS", Opt( a.totalIntegrationS ) }, { "sessionDate", a.sessionDate } };
      const std::vector<ChannelStats> start = store.Stats( i.id, 0 );
      const std::string startThumb = ThumbRel( store, journeyId, String().Format( "start-%lld.jpg", static_cast<long long>( i.id ) ) );
      images.push_back( { { "key", Key( i.id ) }, { "viewId", i.viewId }, { "fileName", FileNameOf( i.filePath ) },
                          { "isMaster", i.isMaster }, { "acquisition", acq }, { "startStats", StatsJson( start ) },
                          { "thumbnail", startThumb.empty() ? nlohmann::json() : nlohmann::json( startThumb ) } } );
      std::vector<ChannelStats> before = start;
      for ( const StepRow& s : store.Steps( i.id, false ) )
      {
         if ( s.state != "active" || IsBase( s ) )
            continue;
         const std::vector<ChannelStats> after = store.Stats( i.id, s.id );
         nlohmann::json achieved = nullptr;
         if ( !before.empty() && !after.empty() )
         {
            nlohmann::json from = nlohmann::json::array(), to = nlohmann::json::array();
            for ( const ChannelStats& c : before ) from.push_back( c.median );
            for ( const ChannelStats& c : after )  to.push_back( c.median );
            achieved = { { "median", { { "from", from }, { "to", to } } } };
         }
         const std::string why = ManualWhy( s );
         const std::string thumb = ThumbRel( store, journeyId, String().Format( "%lld.jpg", static_cast<long long>( s.id ) ) );
         const nlohmann::json mask = s.params.contains( "mask" ) && s.params["mask"].is_object() ? s.params["mask"] : nlohmann::json();
         steps.push_back( {
            { "id", s.id }, { "image", Key( i.id ) }, { "seq", s.seq }, { "processId", s.processId },
            { "parameters", PrivacyStripPaths( s.params.value( "parameters", nlohmann::json::object() ) ) },
            { "tableParameters", PrivacyStripPaths( s.params.value( "tableParameters", nlohmann::json::object() ) ) },
            { "mask", mask },
            { "started", s.started.empty() ? nlohmann::json() : nlohmann::json( s.started ) },
            { "durationS", s.durationS < 0 || !std::isfinite( s.durationS ) ? nlohmann::json() : nlohmann::json( s.durationS ) },
            { "actor", s.actor }, { "reason", s.reason.empty() ? nlohmann::json() : nlohmann::json( s.reason ) },
            { "reasonInferred", s.reasonInferred }, { "manual", !why.empty() },
            { "manualWhy", why.empty() ? nlohmann::json() : nlohmann::json( why ) },
            { "statsBefore", StatsJson( before ) }, { "statsAfter", StatsJson( after ) }, { "achieved", achieved },
            { "thumbnail", thumb.empty() ? nlohmann::json() : nlohmann::json( thumb ) } } );
         if ( !after.empty() )
            before = after;
      }
   }
   for ( const LinkRow& l : store.Links( journeyId ) )
      links.push_back( { { "from", Key( l.fromImageId ) }, { "to", Key( l.toImageId ) },
                         { "viaStep", l.viaStepId == 0 ? nlohmann::json() : nlohmann::json( l.viaStepId ) }, { "evidence", l.evidence } } );
   for ( const GapRow& g : store.Gaps( journeyId ) )
      gaps.push_back( { { "image", g.imageId == 0 ? nlohmann::json() : nlohmann::json( Key( g.imageId ) ) },
                        { "afterSeq", g.afterSeq }, { "reason", g.reason } } );
   return {
      { "schema", PICopilotRecipeSchemaId }, { "schemaVersion", PICopilotRecipeSchemaVersion }, { "generator", generator },
      { "journey", { { "id", j.id }, { "name", j.name }, { "target", j.target }, { "created", j.created },
                     { "keptAt", j.keptAt.empty() ? nlohmann::json() : nlohmann::json( j.keptAt ) },
                     { "endImage", j.endImageId == 0 ? nlohmann::json() : nlohmann::json( Key( j.endImageId ) ) } } },
      { "statsBasis", kStatsBasis }, { "images", images }, { "links", links }, { "steps", steps }, { "gaps", gaps } };
}

bool ValidateRecipe( const nlohmann::json& r, std::string& why )
{
   if ( !Need( r, "", { "schema", "schemaVersion", "generator", "journey", "statsBasis", "images", "links", "steps", "gaps" }, why ) )
      return false;
   if ( r["schema"] != PICopilotRecipeSchemaId ) { why = "schema: must be \"picopilot-recipe\""; return false; }
   if ( !r["schemaVersion"].is_number_integer() || r["schemaVersion"] != PICopilotRecipeSchemaVersion ) { why = "schemaVersion: must be 1"; return false; }
   if ( !r["generator"].is_string() || r["generator"].get<std::string>().empty() ) { why = "generator: must be a non-empty string"; return false; }
   const nlohmann::json& jn = r["journey"];
   if ( !Need( jn, "journey", { "id", "name", "target", "created", "keptAt", "endImage" }, why ) ) return false;
   if ( !jn["id"].is_number_integer() || (!jn["id"].is_number_unsigned() && jn["id"].get<int64>() < 1) || jn["id"] == 0 )
   {
      why = "journey.id: not a positive integer";
      return false;
   }
   for ( const char* k : { "name", "target", "created" } )
      if ( !Typed( jn[k], std::string( "journey." ) + k, { "string" }, why ) ) return false;
   if ( !Typed( jn["keptAt"], "journey.keptAt", { "string", "null" }, why ) ) return false;
   if ( !Typed( jn["endImage"], "journey.endImage", { "string", "null" }, why ) ) return false;
   if ( !Typed( r["statsBasis"], "statsBasis", { "string" }, why ) ) return false;
   if ( !r["images"].is_array() || r["images"].empty() ) { why = "images: must be a non-empty array"; return false; }
   std::set<std::string> keys;
   for ( size_t i = 0; i < r["images"].size(); ++i )
   {
      const nlohmann::json& im = r["images"][i];
      const std::string p = "images[" + std::to_string( i ) + "]";
      if ( !Need( im, p, { "key", "viewId", "fileName", "isMaster", "acquisition", "startStats", "thumbnail" }, why ) ) return false;
      const std::string key = im["key"].is_string() ? im["key"].get<std::string>() : std::string();
      if ( key.size() < 4 || key.compare( 0, 3, "img" ) != 0
        || !std::all_of( key.begin() + 3, key.end(), []( char c ) { return c >= '0' && c <= '9'; } ) )
      {
         why = p + ".key: must match ^img[0-9]+$";
         return false;
      }
      if ( !keys.insert( key ).second )
      {
         why = p + ".key: " + key + " is used by an earlier image";
         return false;
      }
      if ( !Typed( im["viewId"], p + ".viewId", { "string" }, why ) ) return false;
      if ( !Typed( im["fileName"], p + ".fileName", { "string" }, why ) ) return false;
      if ( !Typed( im["isMaster"], p + ".isMaster", { "boolean" }, why ) ) return false;
      if ( !im["acquisition"].is_null() && !AcquisitionOk( im["acquisition"], p + ".acquisition", why ) ) return false;
      if ( !StatsOk( im["startStats"], p + ".startStats", why ) ) return false;
      if ( !Typed( im["thumbnail"], p + ".thumbnail", { "string", "null" }, why ) ) return false;
   }
   if ( !KeyOk( jn["endImage"], keys, "journey.endImage", why, true ) ) return false;
   if ( !r["links"].is_array() ) { why = "links: not an array"; return false; }
   for ( size_t i = 0; i < r["links"].size(); ++i )
   {
      const nlohmann::json& l = r["links"][i];
      const std::string p = "links[" + std::to_string( i ) + "]";
      if ( !Need( l, p, { "from", "to", "viaStep", "evidence" }, why ) ) return false;
      if ( !KeyOk( l["from"], keys, p + ".from", why, false ) || !KeyOk( l["to"], keys, p + ".to", why, false ) ) return false;
      if ( !Typed( l["viaStep"], p + ".viaStep", { "integer", "null" }, why ) ) return false;
      const std::string e = l["evidence"].is_string() ? l["evidence"].get<std::string>() : std::string();
      if ( e != "copilot" && e != "timing" && e != "reference" ) { why = p + ".evidence: not copilot/timing/reference"; return false; }
   }
   if ( !r["steps"].is_array() ) { why = "steps: not an array"; return false; }
   for ( size_t i = 0; i < r["steps"].size(); ++i )
   {
      const nlohmann::json& s = r["steps"][i];
      const std::string p = "steps[" + std::to_string( i ) + "]";
      if ( !Need( s, p, { "id", "image", "seq", "processId", "parameters", "tableParameters", "mask", "started", "durationS",
                          "actor", "reason", "reasonInferred", "manual", "manualWhy", "statsBefore", "statsAfter", "achieved",
                          "thumbnail" }, why ) )
         return false;
      auto positive = []( const nlohmann::json& v ) { return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64>() >= 1) && v != 0; };
      if ( !positive( s["id"] ) ) { why = p + ".id: not a positive integer"; return false; }
      if ( !KeyOk( s["image"], keys, p + ".image", why, false ) ) return false;
      if ( !positive( s["seq"] ) ) { why = p + ".seq: not a positive integer"; return false; }
      if ( !s["processId"].is_string() || s["processId"].get<std::string>().empty() ) { why = p + ".processId: must be a non-empty string"; return false; }
      if ( !Typed( s["parameters"], p + ".parameters", { "object" }, why ) ) return false;
      if ( !Typed( s["tableParameters"], p + ".tableParameters", { "object" }, why ) ) return false;
      if ( !s["mask"].is_null() )
      {
         if ( !Need( s["mask"], p + ".mask", { "id", "inverted" }, why ) ) return false;
         if ( !Typed( s["mask"]["id"], p + ".mask.id", { "string" }, why ) ) return false;
         if ( !Typed( s["mask"]["inverted"], p + ".mask.inverted", { "boolean" }, why ) ) return false;
      }
      if ( !Typed( s["started"], p + ".started", { "string", "null" }, why ) ) return false;
      if ( !Typed( s["durationS"], p + ".durationS", { "number", "null" }, why ) ) return false;
      const std::string actor = s["actor"].is_string() ? s["actor"].get<std::string>() : std::string();
      if ( actor != "user" && actor != "copilot" ) { why = p + ".actor: not user/copilot"; return false; }
      if ( !Typed( s["reason"], p + ".reason", { "string", "null" }, why ) ) return false;
      if ( !Typed( s["reasonInferred"], p + ".reasonInferred", { "boolean" }, why ) ) return false;
      if ( !Typed( s["manual"], p + ".manual", { "boolean" }, why ) ) return false;
      if ( !Typed( s["manualWhy"], p + ".manualWhy", { "string", "null" }, why ) ) return false;
      if ( !StatsOk( s["statsBefore"], p + ".statsBefore", why ) || !StatsOk( s["statsAfter"], p + ".statsAfter", why ) ) return false;
      if ( !s["achieved"].is_null() && !AchievedOk( s["achieved"], p + ".achieved", why ) ) return false;
      if ( !Typed( s["thumbnail"], p + ".thumbnail", { "string", "null" }, why ) ) return false;
   }
   if ( !r["gaps"].is_array() ) { why = "gaps: not an array"; return false; }
   for ( size_t i = 0; i < r["gaps"].size(); ++i )
   {
      const std::string p = "gaps[" + std::to_string( i ) + "]";
      if ( !Need( r["gaps"][i], p, { "image", "afterSeq", "reason" }, why ) ) return false;
      if ( !KeyOk( r["gaps"][i]["image"], keys, p + ".image", why, true ) ) return false;
      if ( !Typed( r["gaps"][i]["afterSeq"], p + ".afterSeq", { "integer" }, why ) ) return false;
      if ( !Typed( r["gaps"][i]["reason"], p + ".reason", { "string" }, why ) ) return false;
   }
   why.clear();
   return true;
}

const char* RecipeSchemaText()
{
   return kRecipeSchemaV1Json;
}

std::string BuildJourneyXpsm( JourneyStore& store, int64 journeyId )
{
   JourneyRow j;
   if ( !store.GetJourney( journeyId, j ) )
      throw Error( String().Format( "no journey #%lld", static_cast<long long>( journeyId ) ) );
   auto iconId = []( const std::string& s )
   {
      std::string r;
      for ( unsigned char c : s )
         r += ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? char( c ) : '_';
      return r;
   };
   std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<!--\nPixInsight XML Process Serialization Module - XPSM 1.0\nPI Copilot image journey #"
                 + std::to_string( journeyId ) + ": " + CommentText( j.name ) + "\n-->\n"
                   "<xpsm version=\"1.0\" xmlns=\"http://www.pixinsight.com/xpsm\" "
                   "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                   "xsi:schemaLocation=\"http://www.pixinsight.com/xpsm http://pixinsight.com/xpsm/xpsm-1.0.xsd\">\n";
   std::string icons;
   int n = 0;
   for ( const ImageRow& i : OrderedImages( store, journeyId ) )
   {
      const std::vector<StepRow> steps = store.Steps( i.id, false );
      const std::string cid = "PICopilot_J" + std::to_string( journeyId ) + "_I" + std::to_string( i.id ) + "_instance";
      std::string body;
      for ( const StepRow& s : steps )
      {
         if ( s.state != "active" || IsBase( s ) )
            continue;
         // A Script step (its XPSM carries the script's full path), a step whose parameter names a file (its
         // XPSM carries that path) and any step that cannot be replayed never enter the icon set: a comment
         // names them, files by FILE NAME only (ManualWhy; pre-flight P5).
         std::string where, name;
         if ( s.processId == "Script" || PathParameter( s, where, name ) || !s.params.value( "replayable", true ) )
         {
            const std::string why = s.processId == "Script" || PathParameter( s, where, name )
                                  ? ManualWhy( s )
                                  : s.params.value( "parseNote", std::string( "cannot be replayed" ) );
            body += "<!-- step " + std::to_string( s.seq ) + " (" + CommentText( s.processId ) + ") is not in this icon set: "
                  + CommentText( why ) + " -->\n";
            continue;
         }
         std::string inst = s.params.value( "xpsm", std::string() );
         if ( inst.empty() )
         {
            body += "<!-- step " + std::to_string( s.seq ) + " (" + CommentText( s.processId ) + ") omitted: "
                  + CommentText( s.params.value( "parseNote", std::string( "not stored" ) ) ) + " -->\n";
            continue;
         }
         if ( s.params.contains( "mask" ) && s.params["mask"].is_object() )
            body += "<!-- step " + std::to_string( s.seq ) + " was applied through mask "
                  + CommentText( s.params["mask"].value( "id", std::string() ) )
                  + (s.params["mask"].value( "inverted", false ) ? " (inverted)" : "") + " -->\n";
         // Inside a container PI writes enabled="true" where a lone instance has id="…_instance".
         const size_t tagEnd = inst.find( '>' );
         const size_t idPos = inst.find( " id=\"" );
         if ( idPos != std::string::npos && idPos < tagEnd )
         {
            const size_t close = inst.find( '"', idPos + 5 );
            if ( close != std::string::npos && close < tagEnd )
               inst.replace( idPos, close + 1 - idPos, " enabled=\"true\"" );
         }
         body += inst + "\n";
      }
      x += "<!-- " + CommentText( i.viewId ) + (i.isMaster ? " (master)" : "") + " -->\n"
         + "<instance class=\"ProcessContainer\" id=\"" + cid + "\">\n" + body + "</instance>\n";
      icons += "<icon id=\"J" + std::to_string( journeyId ) + "_" + iconId( i.viewId ) + "\" instance=\"" + cid
             + "\" xpos=\"8\" ypos=\"" + std::to_string( 8 + 48*n ) + "\" workspace=\"Workspace01\"/>\n";
      ++n;
   }
   return x + icons + "</xpsm>\n";
}

String ExportDirOf( JourneyStore& store, int64 journeyId )
{
   return store.JourneyDir( journeyId ) + "/export";
}

String ExportBaseName( const JourneyRow& j )
{
   return String( SafeFolderName( j.name ).c_str() );
}

KeeperFilesResult WriteKeeperFiles( JourneyStore& store, int64 journeyId, const std::string& generator )
{
   KeeperFilesResult r;
   JourneyRow j;
   try
   {
      r.dir = ExportDirOf( store, journeyId );
      if ( !store.GetJourney( journeyId, j ) )
         throw Error( String().Format( "no journey #%lld", static_cast<long long>( journeyId ) ) );
      const String e = EnsurePrivateDirectory( r.dir );
      if ( !e.IsEmpty() )
         throw Error( "cannot prepare " + r.dir + ": " + e );
   }
   catch ( const pcl::Exception& x )
   {
      r.xpsmError = r.recipeError = r.thumbsError = x.Message();
      return r;
   }
   catch ( const std::exception& x )
   {
      r.xpsmError = r.recipeError = r.thumbsError = WhatOf( x );
      return r;
   }

   const String xpsmPath = r.dir + "/" + ExportBaseName( j ) + ".xpsm";
   try
   {
      // Well-formed or not written: the check parses exactly the bytes that will land.
      const String e = SafeWriteTextFile( xpsmPath, BuildJourneyXpsm( store, journeyId ), SafeFileMode::Shared,
                                          []( const ByteArray& data ) -> String
                                          {
                                             try
                                             {
                                                XMLDocument doc;
                                                doc.Parse( FromU8( std::string( reinterpret_cast<const char*>( data.Begin() ), data.Length() ) ) );
                                                return doc.RootElement() != nullptr ? String() : String( "no root element" );
                                             }
                                             catch ( const pcl::Exception& x )
                                             {
                                                return "not well-formed XML: " + x.Message();
                                             }
                                          } );
      if ( !e.IsEmpty() )
         throw Error( e );
      r.xpsmOk = true;
   }
   catch ( const pcl::Exception& x ) { r.xpsmError = "could not write " + xpsmPath + ": " + x.Message(); }
   catch ( const std::exception& x ) { r.xpsmError = "could not write " + xpsmPath + ": " + WhatOf( x ); }

   try
   {
      const nlohmann::json recipe = BuildRecipe( store, journeyId, generator );
      std::string why;
      if ( !ValidateRecipe( recipe, why ) )
         throw Error( "the recipe does not validate (" + FromU8( why ) + ")" );   // a defect: never written invalid
      String e = SafeWriteTextFile( r.dir + "/recipe.json", recipe.dump( 2 ) + "\n", SafeFileMode::Shared );
      if ( e.IsEmpty() )
         e = SafeWriteTextFile( r.dir + "/recipe.schema.json", std::string( RecipeSchemaText() ), SafeFileMode::Shared );
      if ( !e.IsEmpty() )
         throw Error( e );
      r.recipeOk = true;
   }
   catch ( const pcl::Exception& x ) { r.recipeError = "could not write " + r.dir + "/recipe.json: " + x.Message(); }
   catch ( const std::exception& x ) { r.recipeError = "could not write " + r.dir + "/recipe.json: " + WhatOf( x ); }

   // thumbs/ -> export/thumbs/: the "thumbs/<n>.jpg" references in recipe.json and journey.md are relative to
   // export/, and the export copy takes export/ alone (pre-flight P25).
   try
   {
      const String thumbs = store.JourneyDir( journeyId ) + "/thumbs";
      if ( File::DirectoryExists( thumbs ) )
         CopyTree( thumbs, r.dir + "/thumbs", []( const String& d ) { return EnsurePrivateDirectory( d ); } );
      r.thumbsOk = true;
   }
   catch ( const pcl::Exception& x ) { r.thumbsError = "could not copy the thumbnails into " + r.dir + "/thumbs: " + x.Message(); }
   catch ( const std::exception& x ) { r.thumbsError = "could not copy the thumbnails into " + r.dir + "/thumbs: " + WhatOf( x ); }
   return r;
}

String CopyKeeperToExportFolder( JourneyStore& store, int64 journeyId, const String& exportFolder, String& copiedTo )
{
   copiedTo.Clear();
   const String root = exportFolder.Trimmed();
   if ( root.IsEmpty() )
      return "no export folder is set";
   if ( !root.StartsWith( '/' ) )
      return "the export folder must be an absolute folder: " + root;
   try
   {
      if ( !File::DirectoryExists( root ) )
         return "the export folder " + root + " does not exist (is the drive mounted?); the keeper is saved locally in "
                + store.JourneyDir( journeyId ) + " and can be copied again later";
      JourneyRow j;
      if ( !store.GetJourney( journeyId, j ) )
         return String().Format( "no journey #%lld", static_cast<long long>( journeyId ) );
      const String from = ExportDirOf( store, journeyId );
      if ( !File::DirectoryExists( from ) )
         return "the keeper files of journey \"" + FromU8( j.name ) + "\" have not been written yet (" + from + ")";
      const std::string date = (j.keptAt.empty() ? j.updated : j.keptAt).substr( 0, 10 );
      const String targetDir = root + "/" + String( SafeFolderName( j.target.empty() ? std::string( "unknown-target" ) : j.target ).c_str() );
      const String to = targetDir + "/" + String( (date + "-" + SafeFolderName( j.name )).c_str() );
      const String e = MakeCopyDirectory( targetDir );
      if ( !e.IsEmpty() )
         throw Error( e );
      CopyTree( from, to, MakeCopyDirectory );   // export/ only: it already holds thumbs/ (WriteKeeperFiles)
      copiedTo = to;
      return String();
   }
   catch ( const pcl::Exception& x )
   {
      return "could not copy the keeper into " + root + ": " + x.Message()
           + " (the keeper is saved locally in " + store.JourneyDir( journeyId ) + ")";
   }
   catch ( const std::exception& x )
   {
      return "could not copy the keeper into " + root + ": " + WhatOf( x )
           + " (the keeper is saved locally in " + store.JourneyDir( journeyId ) + ")";
   }
}

} // namespace pcl
