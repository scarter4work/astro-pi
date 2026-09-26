// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "HistoryReader.h"      // Fnv1a64Hex (Task 3), HistoryStep
#include "JourneyConstants.h"   // PICopilotJourneyIntegrationIdInHistory (Task 1, Ruling 29)
#include "JourneyTypes.h"       // AsciiLower (Task 5)
#include "MasterFacts.h"
#include "Utf8.h"
#include "ViewContext.h"        // IsRedactedFitsKeyword

#include <pcl/File.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace pcl
{

namespace
{

// FITS text is 8-bit: decode as Latin-1 (String( const char* )) and re-encode.
std::string FitsU8( const IsoString& s )
{
   return U8( String( s.c_str() ) );
}

std::optional<double> Number( const std::string& s )
{
   if ( s.empty() )
      return std::nullopt;
   char* end = nullptr;
   const double v = std::strtod( s.c_str(), &end );
   if ( end == s.c_str() || !std::isfinite( v ) )   // "nan"/"inf" are not a fact
      return std::nullopt;
   return v;
}

// A frame count: a whole, positive number an int can hold.
std::optional<int> Count( const std::optional<double>& v )
{
   if ( !v || *v < 1 || *v > 1.0e9 || std::floor( *v ) != *v )
      return std::nullopt;
   return int( *v );
}

bool AsciiAlnum( unsigned char c )
{
   return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// "YYYY-MM-DD..." with digits where digits belong.
bool IsIsoDatePrefix( const std::string& s )
{
   if ( s.size() < 10 || s[4] != '-' || s[7] != '-' )
      return false;
   for ( int i : { 0, 1, 2, 3, 5, 6, 8, 9 } )
      if ( s[i] < '0' || s[i] > '9' )
         return false;
   return true;
}

// First HISTORY comment "ImageIntegration.<key>: <value>" -> value.
std::string IntegrationHistory( const FITSKeywordArray& keywords, const char* key )
{
   const std::string prefix = std::string( "ImageIntegration." ) + key + ":";
   for ( const FITSHeaderKeyword& k : keywords )
      if ( k.name.Trimmed().Uppercase() == "HISTORY" )
      {
         const std::string c = FitsU8( k.comment.Trimmed() );
         if ( c.rfind( prefix, 0 ) == 0 )
         {
            std::string v = c.substr( prefix.size() );
            v.erase( 0, v.find_first_not_of( ' ' ) );
            return v;
         }
      }
   return std::string();
}

} // namespace

bool IsIntegrationProcess( const std::string& id )
{
   return id == "ImageIntegration" || id == "DrizzleIntegration" || id == "FastIntegration";
}

bool IsIntegrationAuxiliary( const std::string& viewId, const std::vector<HistoryStep>& history )
{
   if ( !PICopilotJourneyIntegrationIdInHistory )
      return false;
   for ( const HistoryStep& h : history )
   {
      if ( h.processId == "Script" )
         continue;
      return IsIntegrationProcess( h.processId ) && !h.integrationImageId.empty() && h.integrationImageId != viewId;
   }
   return false;
}

std::string KeywordText( const FITSKeywordArray& keywords, const char* name )
{
   const IsoString want = IsoString( name ).Trimmed().Uppercase();
   if ( IsRedactedFitsKeyword( want ) )
      return std::string();
   for ( const FITSHeaderKeyword& k : keywords )
      if ( k.name.Trimmed().Uppercase() == want )
         return FitsU8( k.StripValueDelimiters().Trimmed() );
   return std::string();
}

MasterEvidence DetectMaster( const std::vector<std::string>& ids, const FITSKeywordArray& keywords )
{
   MasterEvidence e;
   for ( const std::string& id : ids )
   {
      if ( id == "Script" )
         continue;
      if ( IsIntegrationProcess( id ) )
      {
         e.isMaster = true;
         e.why = "history begins with " + id;
         return e;
      }
      break;   // the first non-Script step decides
   }
   const std::string type = KeywordText( keywords, "IMAGETYP" );
   const std::string lt = AsciiLower( type );
   if ( lt.find( "master" ) != std::string::npos && lt.find( "dark" ) == std::string::npos
     && lt.find( "flat" ) == std::string::npos && lt.find( "bias" ) == std::string::npos )
   {
      e.isMaster = true;
      e.why = "keyword IMAGETYP='" + type + "'";
      return e;
   }
   if ( !IntegrationHistory( keywords, "numberOfImages" ).empty() )
   {
      e.isMaster = true;
      e.why = "HISTORY ImageIntegration.numberOfImages";
      return e;
   }
   for ( const char* k : { "NCOMBINE", "STACKCNT" } )
   {
      // Same definition of a frame count as ExtractAcquisition (Count), so the
      // master evidence and the recorded subCount can never disagree.
      const std::string text = KeywordText( keywords, k );
      const std::optional<int> n = Count( Number( text ) );
      if ( n && *n > 1 )
      {
         e.isMaster = true;
         e.why = std::string( "keyword " ) + k + "=" + text;
         return e;
      }
   }
   return e;
}

std::string DeriveTarget( const FITSKeywordArray& keywords, const String& filePath, const std::string& viewId )
{
   const std::string object = KeywordText( keywords, "OBJECT" );
   if ( !object.empty() )
      return object;
   if ( !filePath.IsEmpty() )
   {
      // WBPP layout: …/<target>/master/<file>
      const String dir = File::ExtractDirectory( filePath );                 // …/<target>/master
      const String last = File::ExtractNameAndExtension( dir );              // "master"
      if ( last.CompareIC( "master" ) == 0 )
      {
         const String target = File::ExtractNameAndExtension( File::ExtractDirectory( dir ) );
         if ( !target.IsEmpty() )
            return U8( target );
      }
      const String base = File::ExtractName( filePath );
      if ( !base.IsEmpty() )
         return U8( base );
   }
   return viewId;
}

AcquisitionFacts ExtractAcquisition( const FITSKeywordArray& kw, const std::vector<HistoryStep>& history,
                                     const String& filePath, const std::string& viewId )
{
   AcquisitionFacts a;
   a.target = DeriveTarget( kw, filePath, viewId );
   a.filter = KeywordText( kw, "FILTER" );
   a.camera = KeywordText( kw, "INSTRUME" );
   a.gain = Number( KeywordText( kw, "GAIN" ) );
   a.offset = Number( KeywordText( kw, "OFFSET" ) );
   a.sensorTempC = Number( KeywordText( kw, "CCD-TEMP" ) );
   if ( !a.sensorTempC )
      a.sensorTempC = Number( KeywordText( kw, "SET-TEMP" ) );

   // Frame count, first usable source wins: the ImageIntegration HISTORY line,
   // NCOMBINE, STACKCNT, then the enabled rows of an integration step's images
   // table. A table with no enabled row gives no count (unknown, not 0).
   std::optional<int> count = Count( Number( IntegrationHistory( kw, "numberOfImages" ) ) );
   if ( !count ) count = Count( Number( KeywordText( kw, "NCOMBINE" ) ) );
   if ( !count ) count = Count( Number( KeywordText( kw, "STACKCNT" ) ) );
   if ( !count )
      for ( const HistoryStep& h : history )
         if ( IsIntegrationProcess( h.processId ) && h.tableParameters.is_object() && h.tableParameters.contains( "images" )
           && h.tableParameters.at( "images" ).is_array() )
         {
            int n = 0;
            for ( const nlohmann::json& row : h.tableParameters.at( "images" ) )
               if ( row.is_array() && !row.empty() && row.at( 0 ).is_boolean() && row.at( 0 ).get<bool>() )
                  ++n;
            count = Count( double( n ) );
            break;
         }
   a.subCount = count;

   const std::optional<double> live = Number( KeywordText( kw, "LIVETIME" ) );
   std::optional<double> exposure = Number( KeywordText( kw, "EXPTIME" ) );
   if ( !exposure )
      exposure = Number( KeywordText( kw, "EXPOSURE" ) );
   if ( live && a.subCount && *a.subCount > 0 )
   {
      a.totalIntegrationS = *live;                  // Siril: LIVETIME is the total, EXPTIME may be too
      a.subExposureS = *live/ *a.subCount;
   }
   else
   {
      a.subExposureS = exposure;
      if ( exposure && a.subCount )
         a.totalIntegrationS = *exposure * *a.subCount;
   }

   // The first of DATE-OBS, DATE-LOC that starts with an ISO date: a present
   // but non-ISO DATE-OBS (e.g. a Julian date) does not hide DATE-LOC.
   for ( const char* k : { "DATE-OBS", "DATE-LOC" } )
   {
      const std::string date = KeywordText( kw, k );
      if ( IsIsoDatePrefix( date ) )
      {
         a.sessionDate = date.substr( 0, 10 );
         break;
      }
   }
   return a;
}

std::string MasterFingerprint( int width, int height, int channels, int bitsPerSample, bool floatSample,
                               const std::vector<std::string>& baseIdentities, const FITSKeywordArray& keywords )
{
   std::string h;
   for ( const std::string& id : baseIdentities )
      h += id + "\n";
   h += "--\n";
   for ( const char* k : { "IMAGETYP", "OBJECT", "FILTER", "INSTRUME", "TELESCOP", "EXPTIME", "EXPOSURE", "DATE-OBS",
                           "NCOMBINE", "STACKCNT", "XBINNING" } )
      h += std::string( k ) + "=" + KeywordText( keywords, k ) + "\n";
   for ( const FITSHeaderKeyword& k : keywords )
      if ( k.name.Trimmed().Uppercase() == "HISTORY" )
      {
         const std::string c = FitsU8( k.comment.Trimmed() );
         if ( c.rfind( "ImageIntegration.", 0 ) == 0 )
            h += c + "\n";
      }
   char geo[64];
   std::snprintf( geo, sizeof geo, "%dx%dx%d:%c%d:", width, height, channels, floatSample ? 'f' : 'i', bitsPerSample );
   return std::string( geo ) + Fnv1a64Hex( h );
}

std::string DeriveJourneyName( const std::string& target, const std::string& filter, int masterCount, const std::string& dateIso )
{
   const std::string date = dateIso.substr( 0, 10 );
   if ( masterCount > 1 )
      return target + " " + std::to_string( masterCount ) + " masters " + date;
   return filter.empty() ? target + " " + date : target + " " + filter + " " + date;
}

std::string StripKind( const std::string& filter, int masterCount )
{
   if ( masterCount > 1 )
      return std::to_string( masterCount ) + " masters";
   return filter.empty() ? std::string( "master" ) : filter + " master";
}

std::string SafeFolderName( const std::string& name )
{
   std::string r;
   int pending = 0;   // continuation bytes still owed by the current UTF-8 lead byte
   for ( unsigned char c : name )
   {
      if ( (c & 0xC0) == 0x80 )
      {
         if ( pending > 0 )
         {
            --pending;   // tail of a code point that already became one '_'
            continue;
         }
         r += '_';       // unpaired continuation byte: its own '_'
      }
      else
      {
         pending = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
         r += AsciiAlnum( c ) || c == '.' || c == '_' || c == '-' ? char( c ) : '_';
      }
      if ( r.size() == 60 )
         break;
   }
   if ( r.empty() || r == "." || r == ".." )
      return "journey";
   if ( r[0] == '.' )
      r[0] = '_';
   return r;
}

} // namespace pcl
