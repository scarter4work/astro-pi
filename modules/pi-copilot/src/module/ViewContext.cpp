// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ViewContext.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace pcl
{

namespace
{

// FITS text is 8-bit; String( const char* ) decodes ISO-8859-1, so any
// high byte survives as a code point and is re-encoded as valid UTF-8.
std::string FitsU8( const IsoString& s )
{
   return U8( String( s.c_str() ) );
}

bool IsAlnum( char c )
{
   return std::isalnum( uint8( c ) ) != 0;
}

// The length of an absolute-path opener at s[i]; 0 when none. FAIL-CLOSED:
// a path may start after ANY character that is not a letter or digit (or at
// the start), so no delimiter list can be bypassed. Openers: "/x", a double
// backslash (a network share), "~/" or "~\", a drive ("C:\" or "C:/").
// "1/2", "$T/2" and "Ha/OIII" are not openers (a letter or digit before the
// slash); relative paths are handled by RelativeTokenPath().
size_t PathOpener( const std::string& s, size_t i )
{
   const size_t n = s.size();
   if ( i > 0 && (IsAlnum( s[i-1] ) || s[i-1] == '.') )   // "./x", "../x": relative (RelativeTokenPath)
   {
      // A drive letter follows a non-alphanumeric (or starts the value); a
      // slash right after an alphanumeric is never an opener.
      return 0;
   }
   if ( s[i] == '/' && i+1 < n && s[i+1] != ' ' )
      return 1;
   if ( s[i] == '\\' && i+1 < n && s[i+1] == '\\' )
      return 2;
   if ( s[i] == '~' && i+1 < n && (s[i+1] == '/' || s[i+1] == '\\') )
      return 2;
   if ( i+2 < n && std::isalpha( uint8( s[i] ) ) && s[i+1] == ':' && (s[i+2] == '\\' || s[i+2] == '/') )
      return 3;
   return 0;
}

// What a path is reduced to: its file name when the last component looks like
// one (a name with an extension: "light_001.fits"), else "[path]" -- the last
// component of a folder ("/mnt/qnap/astro_data") or of a path cut short
// ("/home/u/data/M31/sessi") is a folder name, never kept. `rest` is the text
// after the last separator; the file name ends at a space, quote, comma,
// semicolon or bracket, and everything after it is kept as it was.
std::string ReducedTail( const std::string& rest )
{
   size_t end = 0;
   while ( end < rest.size() && std::strchr( " \t'\",;)]}>|", rest[end] ) == nullptr )
      ++end;
   const std::string name = rest.substr( 0, end );
   const size_t dot = name.rfind( '.' );
   bool file = dot != std::string::npos && dot > 0 && dot + 1 < name.size() && name.size() - dot - 1 <= 10;
   for ( size_t k = dot == std::string::npos ? name.size() : dot + 1; file && k < name.size(); ++k )
      file = IsAlnum( name[k] );
   return (file ? name : std::string( "[path]" )) + rest.substr( end );
}

// A whitespace/quote/punctuation-delimited token that is a RELATIVE path:
// two or more separators ("data/M31/raw", "../x/y.fits"), or one separator
// and a file name with an extension after it ("M31/light.fits"). "1/2",
// "2026/10/13", "N/A", "Ha/OIII" and "$T/2" are kept (no file name, one
// separator). Returns the reduced token, or the token unchanged.
std::string RelativeTokenPath( const std::string& tok )
{
   size_t seps = 0;
   for ( char c : tok )
      if ( c == '/' || c == '\\' )
         ++seps;
   if ( seps == 0 )
      return tok;
   const size_t last = tok.find_last_of( "/\\" );
   const std::string tail = tok.substr( last + 1 );
   const std::string reduced = ReducedTail( tail );
   const bool hasFile = reduced.compare( 0, 6, "[path]" ) != 0 && !tail.empty();
   bool letterBefore = false;
   for ( size_t k = 0; k < last; ++k )
      letterBefore = letterBefore || std::isalpha( uint8( tok[k] ) );
   if ( seps >= 2 && letterBefore )
      return reduced;
   if ( seps >= 1 && hasFile && letterBefore )
      return reduced;
   return tok;
}

// Pass 1: absolute paths (and file:// URLs), from each opener to the next
// one, keep only what follows the last separator (ReducedTail).
std::string AbsolutePathsReduced( const std::string& value )
{
   std::vector<size_t> starts;
   for ( size_t i = 0; i < value.size(); ++i )
      if ( PathOpener( value, i ) > 0 )
      {
         starts.push_back( i );
         // The rest of this path's word is part of it: a URL's "//host", a
         // share's double backslash, "file:///C:/..." never start a second path.
         while ( i+1 < value.size() && value[i+1] != ' ' && value[i+1] != '\t' )
            ++i;
      }
   std::string out;
   if ( starts.empty() )
      out = value;
   else
   {
      out = value.substr( 0, starts.front() );
      for ( size_t k = 0; k < starts.size(); ++k )
      {
         const size_t a = starts[k];
         const size_t b = k+1 < starts.size() ? starts[k+1] : value.size();
         // "file:" before a path is part of it (a file URL): dropped.
         if ( out.size() >= 5 )
         {
            std::string scheme = out.substr( out.size() - 5 );
            for ( char& c : scheme )
               c = char( std::tolower( uint8( c ) ) );
            if ( scheme == "file:" )
               out.erase( out.size() - 5 );
         }
         const std::string seg = value.substr( a, b - a );
         const size_t sep = seg.find_last_of( "/\\" );
         out += ReducedTail( sep == std::string::npos ? seg : seg.substr( sep + 1 ) );
      }
   }
   return out;
}

// Pass 2: relative paths left in the text, token by token.
std::string RelativePathsReduced( const std::string& out )
{
   std::string res;
   size_t i = 0;
   while ( i < out.size() )
   {
      if ( std::strchr( " \t'\",;()[]{}<>|=", out[i] ) != nullptr )
      {
         res += out[i++];
         continue;
      }
      size_t j = i;
      while ( j < out.size() && std::strchr( " \t'\",;()[]{}<>|=", out[j] ) == nullptr )
         ++j;
      res += RelativeTokenPath( out.substr( i, j - i ) );
      i = j;
   }
   return res;
}

std::string LineWise( const std::string& text, std::string (*f)( const std::string& ) )
{
   std::string out;
   size_t i = 0;
   while ( i <= text.size() )
   {
      size_t j = text.find( '\n', i );
      if ( j == std::string::npos )
         j = text.size();
      out += f( text.substr( i, j - i ) );
      if ( j < text.size() )
         out += '\n';
      i = j + 1;
   }
   return out;
}

nlohmann::json JsonWalk( const nlohmann::json& v, std::string (*f)( const std::string& ) )
{
   if ( v.is_string() )
      return f( v.get<std::string>() );
   if ( v.is_array() )
   {
      nlohmann::json a = nlohmann::json::array();
      for ( const nlohmann::json& e : v )
         a.push_back( JsonWalk( e, f ) );
      return a;
   }
   if ( v.is_object() )
   {
      nlohmann::json o = nlohmann::json::object();
      for ( auto it = v.begin(); it != v.end(); ++it )
         o[f( it.key() )] = JsonWalk( it.value(), f );
      return o;
   }
   return v;
}

} // namespace

std::string FitsValueWithoutPaths( const std::string& value )
{
   return RelativePathsReduced( AbsolutePathsReduced( value ) );
}

std::string TextWithoutFolders( const std::string& text )
{
   return LineWise( text, &FitsValueWithoutPaths );
}

std::string TextWithoutAbsolutePaths( const std::string& text )
{
   return LineWise( text, &AbsolutePathsReduced );
}

nlohmann::json JsonWithoutFolders( const nlohmann::json& v )
{
   return JsonWalk( v, &TextWithoutFolders );
}

nlohmann::json JsonWithoutAbsolutePaths( const nlohmann::json& v )
{
   return JsonWalk( v, &TextWithoutAbsolutePaths );
}

bool IsRedactedFitsKeyword( const IsoString& name )
{
   // FAIL-CLOSED: any case, any separator (HIERARCH "ESO TEL GEOLAT",
   // "SITE-LAT", "Observation:Location:Latitude"), any keyword that names a
   // place or a person. Whole tokens and telling substrings both count.
   const IsoString trimmed = name.Trimmed();
   std::string n = trimmed.Uppercase().c_str();
   for ( const char* r : PICopilotRedactedFitsKeywords )
      if ( n == r )
         return true;
   static const char* const kSubstrings[] = {
      "OBSGEO", "GEOLAT", "GEOLON", "GEOELEV", "GEOALT", "GEODETIC", "LATITUD", "LONGITUD", "ELEVATIO",
      "GPS", "OBSERVER", "OBSERVAT", "LOCATION", "ADDRESS", "AUTHOR", "EMAIL", "TELEPHON" };
   for ( const char* s : kSubstrings )
      if ( n.find( s ) != std::string::npos )
         return true;
   // SITE anywhere (SITELAT, SITENAME, OBSSITE), but not inside COMPOSITE /
   // POSITION / DEPOSIT.
   for ( size_t p = n.find( "SITE" ); p != std::string::npos; p = n.find( "SITE", p + 1 ) )
      if ( !(p >= 2 && n.compare( p - 2, 2, "PO" ) == 0) )
         return true;
   // Whole tokens, for FITS-style names only (no lowercase letter: keywords
   // and HIERARCH names). The same function screens camelCase process
   // parameter ids for the journey library (JourneyStore), where "alt" or
   // "origin" could be ordinary parameters; the substring rules above still
   // apply to those.
   bool hasLower = false;
   for ( size_type i = 0; i < trimmed.Length(); ++i )
      hasLower = hasLower || (trimmed[i] >= 'a' && trimmed[i] <= 'z');
   if ( hasLower )
      return false;
   static const char* const kTokens[] = {
      "LAT", "LON", "LONG", "LNG", "ELEV", "ALT", "CITY", "COUNTRY", "TOWN", "PLACE", "OWNER", "USER", "USERNAME",
      "CONTACT", "OBSNAME", "PI", "PINAME", "OPERATOR", "ORIGIN" };
   std::string tok;
   for ( size_t i = 0; i <= n.size(); ++i )
   {
      if ( i < n.size() && IsAlnum( n[i] ) )
      {
         tok += n[i];
         continue;
      }
      for ( const char* t : kTokens )
         if ( tok == t )
            return true;
      tok.clear();
   }
   return false;
}

// COMMENT, HISTORY and blank keywords carry free text (often software
// settings with file paths): never sent, whatever is in them.
bool IsCommentaryFitsKeyword( const IsoString& name )
{
   const IsoString n = name.Trimmed().Uppercase();
   return n.IsEmpty() || n == "COMMENT" || n == "HISTORY";
}

String ViewContextFileName( const String& filePath )
{
   return filePath.IsEmpty() ? String() : File::ExtractNameAndExtension( filePath );
}

nlohmann::json BuildViewContext( const View& view )
{
   if ( view.IsNull() )
      throw Error( "BuildViewContext: null view" );

   View v = view;                       // alias; locking needs a non-const View
   ImageWindow window = v.Window();

   nlohmann::json ctx;
   ctx["viewId"] = std::string( v.Id().c_str() );
   ctx["fullId"] = std::string( v.FullId().c_str() );
   ctx["isPreview"] = v.IsPreview();
   ctx["fileName"] = window.IsNull() ? std::string() : U8( ViewContextFileName( window.FilePath() ) );

   {
      AutoViewWriteLock lock( v );
      ImageVariant img = v.Image();
      if ( !img )
         throw Error( "BuildViewContext: view has no image" );
      if ( img.IsComplexSample() )
         throw Error( "BuildViewContext: complex-sample images are not supported" );

      const Rect r = img.Bounds();
      const int nominal = img.NumberOfNominalChannels();
      ctx["geometry"] = {
         { "width", img.Width() },
         { "height", img.Height() },
         { "channels", img.NumberOfChannels() },
         { "nominalChannels", nominal },
         { "bitsPerSample", img.BitsPerSample() },
         { "floatSample", img.IsFloatSample() },
         { "color", img.IsColor() }
      };

      nlohmann::json stats = nlohmann::json::array();
      for ( int c = 0; c < nominal; ++c )
      {
         const double median = img.Median( r, c, c );
         stats.push_back( {
            { "channel", c },
            { "median", median },
            { "mad", img.MAD( median, r, c, c ) },
            { "mean", img.Mean( r, c, c ) },
            { "min", img.MinimumSampleValue( r, c, c ) },
            { "max", img.MaximumSampleValue( r, c, c ) }
         } );
      }
      ctx["channelStats"] = stats;
      ctx["statsNote"] = "Statistics of the real image data (normalized [0,1] sample range), "
                         "not of the preview. mad is the RAW median absolute deviation "
                         "(multiply by 1.4826 for a Gaussian-equivalent sigma).";
   }

   const FITSKeywordArray keywords = window.IsNull() ? FITSKeywordArray() : window.Keywords();
   nlohmann::json fits = nlohmann::json::array();
   size_type kept = 0, redacted = 0, scrubbed = 0, commentary = 0;
   for ( const FITSHeaderKeyword& kw : keywords )
   {
      if ( IsCommentaryFitsKeyword( kw.name ) )
      {
         ++commentary;
         continue;
      }
      if ( IsRedactedFitsKeyword( kw.name ) )
      {
         ++redacted;
         continue;
      }
      if ( kept == size_type( PICopilotMaxFitsKeywords ) )
         continue;   // counted below as omitted
      ++kept;
      // Paths out first (file names only), then the length cap.
      const std::string raw = FitsU8( kw.StripValueDelimiters() );
      const std::string clean = FitsValueWithoutPaths( raw );
      if ( clean != raw )
         ++scrubbed;
      const String value = FromU8( clean );
      nlohmann::json entry = { { "name", FitsValueWithoutPaths( FitsU8( kw.name ) ) } };
      if ( value.Length() > size_type( PICopilotMaxFitsValueChars ) )
      {
         entry["value"] = U8( value.Left( PICopilotMaxFitsValueChars ) );
         entry["valueTruncated"] = true;
      }
      else
         entry["value"] = clean;
      fits.push_back( entry );
   }
   ctx["fitsKeywords"] = fits;
   ctx["fitsKeywordsTotal"] = keywords.Length();
   ctx["fitsKeywordsRedacted"] = redacted;
   ctx["fitsKeywordsOmitted"] = keywords.Length() - redacted - commentary - kept;
   ctx["fitsCommentaryKeywordsOmitted"] = commentary;
   ctx["fitsValuesPathsScrubbed"] = scrubbed;
   return ctx;
}

} // namespace pcl
