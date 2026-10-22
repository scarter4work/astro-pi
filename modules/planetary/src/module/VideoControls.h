// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// What the SERViewer and PlanetaryStack interfaces share: their exception
// guard, the Open... dialog, the Bayer pattern combo, and Save pattern....

#ifndef __VideoControls_h
#define __VideoControls_h

#include <pcl/ComboBox.h>
#include <pcl/Exception.h>
#include <pcl/String.h>

#include <exception>
#include <string>

#include "FrameSource.h"
#include "SerInfoText.h"

namespace pcl
{

inline String FromUtf8( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

inline std::string ToUtf8( const String& s )
{
   return s.ToUTF8().c_str();
}

// Runs f (an event handler or core callback body). PCL does not catch
// exceptions across the API boundary into the PixInsight core, so anything f
// throws is stopped here and handed to onFail as a message.
template <class F, class OnFail>
void GuardedCall( F&& f, OnFail&& onFail )
{
   try
   {
      f();
   }
   catch ( const Exception& e )
   {
      onFail( e.Message() );
   }
   catch ( ... )
   {
      onFail( FromUtf8( planetary::preview::DescribeException( std::current_exception() ) ) );
   }
}

// Open...: the planetary video dialog (SER and AVI), starting in the folder of
// `current` when there is one. True with the chosen file in `path`.
bool AskForVideo( const String& current, String& path );

// The Bayer pattern combo's items (item index == BayerPatternEnumeration
// value) and tooltip.
void SetUpBayerCombo( ComboBox& combo );

// Save pattern... applies to an AVI whose reader found no sidecar next to it
// (there is none to read, and never one to overwrite).
bool AviWithoutSidecar( const planetary::ser::FileInfo& info );

enum class SavePatternResult { Written, Declined, Failed };

// Save pattern...: asks (a message box captioned `process`), then writes
// `pattern` to the AVI's .txt sidecar (WriteAsiairSidecar, which never
// overwrites). A failure is written to the Process Console and shown in a
// message box; its text is returned in `failure` for the caller's info block.
SavePatternResult SaveBayerPattern( const String& aviPath, planetary::ser::ColorId pattern, const char* process,
                                    String& failure );

} // namespace pcl

#endif // __VideoControls_h
