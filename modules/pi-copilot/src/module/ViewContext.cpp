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

#include <string>

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

} // namespace

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
   size_type kept = 0, redacted = 0;
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
      IsoString value = kw.StripValueDelimiters();
      nlohmann::json entry = { { "name", FitsU8( kw.name ) } };
      if ( value.Length() > size_type( PICopilotMaxFitsValueChars ) )
      {
         entry["value"] = FitsU8( value.Left( PICopilotMaxFitsValueChars ) );
         entry["valueTruncated"] = true;
      }
      else
         entry["value"] = FitsU8( value );
      fits.push_back( entry );
   }
   ctx["fitsKeywords"] = fits;
   ctx["fitsKeywordsTotal"] = keywords.Length();
   ctx["fitsKeywordsRedacted"] = redacted;
   ctx["fitsKeywordsOmitted"] = keywords.Length() - redacted - kept;
   return ctx;
}

} // namespace pcl
