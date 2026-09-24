// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "VisionTurn.h"
#include "Utf8.h"

namespace pcl
{

const char* const kPICopilotImageOmittedNote =
   "[An auto-stretched preview of the view was attached to this message when it was sent; "
   "it is omitted from the re-sent history.]\n";

namespace
{

const char* const kContextOpen  = "[PixInsight view context]\n";
const char* const kContextClose = "\n[/PixInsight view context]\n\n";

// Collapses the view-context block ComposeUserTurn() put at the start of
// content (after an optional imageNote prefix) to
//   {"collapsed":true,"fullId":...,"geometry":...}
// and keeps everything else -- the note and the user's own text -- byte for
// byte. The collapsed form collapses to itself, so this is idempotent.
// Content without a well-formed block at that position (plain text, a user
// who typed the marker mid-message, unparsable JSON) is returned unchanged.
String CollapseViewContext( const String& content, const String& imageNote )
{
   const String open = kContextOpen;
   const String close = kContextClose;
   const size_type at = content.StartsWith( imageNote ) ? imageNote.Length() : 0;
   if ( content.Find( open, at ) != at )
      return content;
   const size_type jsonAt = at + open.Length();
   const size_type closeAt = content.Find( close, jsonAt );
   if ( closeAt == String::notFound )
      return content;

   const nlohmann::json full = nlohmann::json::parse(
      U8( content.Substring( jsonAt, closeAt - jsonAt ) ), nullptr, false/*allow_exceptions*/ );
   if ( full.is_discarded() || !full.is_object() )
      return content;
   nlohmann::json minimal = { { "collapsed", true } };
   if ( full.contains( "fullId" ) )
      minimal["fullId"] = full["fullId"];
   if ( full.contains( "geometry" ) )
      minimal["geometry"] = full["geometry"];

   return content.Left( at ) + open
        + String::UTF8ToUTF16( minimal.dump().c_str() )
        + content.Substring( closeAt );
}

} // namespace

AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 )
{
   AnthropicMessage m;
   m.role = "user";
   if ( viewContext != nullptr )
      m.content = String( kContextOpen )
                + String::UTF8ToUTF16( viewContext->dump().c_str() )
                + String( kContextClose )
                + userText;
   else
      m.content = userText;
   m.imageJpegBase64 = jpegBase64;
   return m;
}

void StripOlderImages( Array<AnthropicMessage>& history )
{
   if ( history.Length() < 2 )
      return;
   const String note = String::UTF8ToUTF16( kPICopilotImageOmittedNote );
   for ( size_type i = 0; i + 1 < history.Length(); ++i )
   {
      AnthropicMessage& m = history[i];
      if ( m.role != "user" )
         continue;
      if ( !m.imageJpegBase64.IsEmpty() )
      {
         m.imageJpegBase64.Clear();
         m.content = note + m.content;
      }
      m.content = CollapseViewContext( m.content, note );
   }
}

} // namespace pcl
