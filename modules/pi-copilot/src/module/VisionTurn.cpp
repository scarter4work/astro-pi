// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "VisionTurn.h"

namespace pcl
{

const char* const kPICopilotImageOmittedNote =
   "[An auto-stretched preview of the view was attached to this message when it was sent; "
   "it is omitted from the re-sent history.]\n";

AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 )
{
   AnthropicMessage m;
   m.role = "user";
   if ( viewContext != nullptr )
      m.content = "[PixInsight view context]\n"
                + String::UTF8ToUTF16( viewContext->dump().c_str() )
                + "\n[/PixInsight view context]\n\n"
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
      if ( !history[i].imageJpegBase64.IsEmpty() )
      {
         history[i].imageJpegBase64.Clear();
         history[i].content = note + history[i].content;
      }
}

} // namespace pcl
