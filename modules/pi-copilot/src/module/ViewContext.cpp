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

// Where a path may start: the value's start, or after one of these.
bool PathMayStartAfter( char c )
{
   return c == ' ' || c == '\t' || c == '\'' || c == '"' || c == '=' || c == '(' || c == '[' || c == ','
       || c == ';' || c == ':';
}

// The length of an absolute-path opener at s[i] ("/x", a double backslash,
// "~/", "C:\", "C:/"); 0 when none.
size_t PathOpener( const std::string& s, size_t i )
{
   if ( i > 0 && !PathMayStartAfter( s[i-1] ) )
      return 0;
   const size_t n = s.size();
   if ( s[i] == '/' && i+1 < n && s[i+1] != ' ' )
      return 1;
   if ( s[i] == '\\' && i+1 < n && s[i+1] == '\\' )
      return 2;
   if ( s[i] == '~' && i+1 < n && s[i+1] == '/' )
      return 2;
   if ( i+2 < n && std::isalpha( uint8( s[i] ) ) && s[i+1] == ':' && (s[i+2] == '\\' || s[i+2] == '/') )
      return 3;
   return 0;
}

} // namespace

std::string FitsValueWithoutPaths( const std::string& value )
{
   std::vector<size_t> starts;
   for ( size_t i = 0; i < value.size(); ++i )
      if ( PathOpener( value, i ) > 0 )
      {
         starts.push_back( i );
         // A URL's "//host" or a share's double backslash is one path, not two.
         while ( i+1 < value.size() && (value[i+1] == '/' || value[i+1] == '\\') )
            ++i;
      }
   if ( starts.empty() )
      return value;
   std::string out = value.substr( 0, starts.front() );
   for ( size_t k = 0; k < starts.size(); ++k )
   {
      const size_t a = starts[k];
      const size_t b = k+1 < starts.size() ? starts[k+1] : value.size();
      const std::string seg = value.substr( a, b - a );
      // The segment runs to the next path (or the end): keep what follows its
      // last separator -- the file name and any text after it.
      const size_t sep = seg.find_last_of( "/\\" );
      out += sep == std::string::npos ? seg : seg.substr( sep + 1 );
   }
   return out;
}

bool IsRedactedFitsKeyword( const IsoString& name )
{
   const IsoString n = name.Trimmed().Uppercase();
   for ( const char* r : PICopilotRedactedFitsKeywords )
      if ( n == r )
         return true;
   return false;
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
   size_type kept = 0, redacted = 0, scrubbed = 0;
   for ( const FITSHeaderKeyword& kw : keywords )
   {
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
      nlohmann::json entry = { { "name", FitsU8( kw.name ) } };
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
   ctx["fitsKeywordsOmitted"] = keywords.Length() - redacted - kept;
   ctx["fitsValuesPathsScrubbed"] = scrubbed;
   return ctx;
}

} // namespace pcl
