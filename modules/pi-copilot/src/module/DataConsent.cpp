// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "DataConsent.h"

#include <pcl/MessageBox.h>
#include <pcl/Settings.h>
#include <pcl/TimePoint.h>

namespace pcl { namespace DataConsent {

namespace
{
const char* const kVersionKey = "PICopilot/AnthropicConsentVersion";
const char* const kDateKey    = "PICopilot/AnthropicConsentDate";
}

bool Granted()
{
   int v = 0;
   return Settings::Read( kVersionKey, v ) && v == kVersion;
}

String GrantedAt()
{
   if ( !Granted() )
      return String();
   String d;
   Settings::Read( kDateKey, d );
   return d;
}

void Grant()
{
   Settings::Write( kVersionKey, kVersion );
   Settings::Write( kDateKey, TimePoint::Now().ToString( "%Y-%M-%D" ) );
}

void Revoke()
{
   Settings::Remove( kVersionKey );
   Settings::Remove( kDateKey );
}

String ConsentHtml( bool question )
{
   return String::UTF8ToUTF16(
      "<p><b>PI Copilot sends data to Anthropic</b></p>"
      "<p>PI Copilot works by sending your requests to <b>Anthropic, PBC</b> (the company that makes Claude), at "
      "<b>api.anthropic.com</b>, over an encrypted HTTPS connection, using your own API key. Nothing is sent until "
      "you agree here.</p>"
      "<p><b>What can be sent</b>, with each message:</p>"
      "<ul>"
      "<li>the text you type in the chat, and the conversation so far;</li>"
      "<li>with <i>Include view</i> on: an auto-stretched JPEG preview of the active image (at most 1024 pixels on "
      "its long side), its statistics (median, deviation, mean, minimum, maximum per channel), its size and view id, "
      "its <b>file name</b> (never the folder), and up to 60 of its FITS keywords &mdash; except keywords that name a "
      "place or a person (SITELAT, SITELONG, SITEELEV, OBSGEO-B/L/H, LAT-OBS, LONG-OBS, ALT-OBS, OBSERVER, OBSERVAT, "
      "GPS and similar, in any spelling) and COMMENT/HISTORY text, which are never sent, and with every folder path "
      "in a keyword reduced to its file name;</li>"
      "<li>results of what PI Copilot looks up or does for you: other images' previews and statistics, the processes "
      "and their parameters, an image's processing history, your workspace process icons, the scripts installed in "
      "PixInsight and the console output of a script you let it run (file names only, never folders);</li>"
      "<li>image journey data: the recorded steps, their statistics, acquisition facts (target, filter, camera, "
      "gain, exposure, sub count, dates) and file names &mdash; to the model when you ask about a journey, and to "
      "<b>Claude Haiku</b> when you keep a journey, to write its journey.md (no image is sent for that).</li>"
      "</ul>"
      "<p><b>Never sent:</b> your files themselves, full-resolution image data, folder paths, location or observer "
      "keywords. Your API key goes only to Anthropic, as the credential of each request.</p>"
      "<p>Anthropic handles API data under its own terms and privacy policy: "
      "<a href=\"https://www.anthropic.com/legal/privacy\">https://www.anthropic.com/legal/privacy</a>.</p>"
      "<p>You can withdraw this at any time in PI Copilot's settings (&#x2699;); nothing is sent after that until "
      "you agree again.</p>" )
        + (question ? String( "<p>Do you agree to send this data to Anthropic?</p>" ) : String());
}

String NotGrantedText()
{
   return "Nothing was sent: you have not agreed to PI Copilot sending data to Anthropic (api.anthropic.com). "
          "Send a message to see what is sent and agree, or allow it in PI Copilot's settings.";
}

bool Ask()
{
   const bool yes = MessageBox( ConsentHtml(),
                                String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 data sent to Anthropic" ),
                                StdIcon::Question, StdButton::Yes, StdButton::No, StdButton::NoButton,
                                1/*default: No*/, 1/*Esc: No*/ ).Execute() == StdButton::Yes;
   if ( yes )
      Grant();
   return yes;
}

} } // namespace pcl::DataConsent
