// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "KeyStore.h"
#include "Utf8.h"

#include <pcl/Settings.h>

namespace pcl { namespace KeyStore {

namespace
{

KeyringId  g_id;
IsoString  g_settingsKey = "PICopilot/AnthropicApiKey";
bool       g_cached = false;
State      g_cache;

const char* const kLabel = "PI Copilot: Anthropic API key";

bool IsPrintableAscii( const IsoString& s )
{
   if ( s.IsEmpty() )
      return false;
   for ( const char c : s )
      if ( static_cast<unsigned char>( c ) < 0x21 || static_cast<unsigned char>( c ) > 0x7E )
         return false;
   return true;
}

// Writes the key to the keyring and reads it back. "" when verified, else why not.
String StoreVerified( const String& key )
{
   const IsoString bytes( U8( key ).c_str() );   // printable ASCII (ConfigDialog validates)
   const KeyringResult w = KeyringStore( g_id, kLabel, bytes );
   if ( !w.ok )
      return w.error;
   const KeyringResult r = KeyringLookup( g_id );
   if ( !r.ok )
      return "the key was written but could not be read back: " + r.error;
   if ( !r.found || r.secret != bytes )
      return "the key read back from the keyring did not match what was written";
   return String();
}

State Remember( const State& s )
{
   g_cache = s;
   g_cached = !s.key.IsEmpty();   // retry next time when nothing was found (e.g. a locked keyring)
   return s;
}

} // namespace

State Load()
{
   if ( g_cached )
      return g_cache;
   State st;
   String plain;
   Settings::Read( g_settingsKey, plain );
   if ( !plain.IsEmpty() )
   {
      st.key = plain;
      const String why = StoreVerified( plain );
      if ( why.IsEmpty() )
      {
         Settings::Remove( g_settingsKey );
         st.where = Where::Keyring;
         st.note = "Your API key was moved from PixInsight's settings (plain text) into the system keyring.";
      }
      else
      {
         st.where = Where::Settings;
         st.note = "The system keyring could not be used (" + why + "), so your API key stays in PixInsight's "
                   "settings in plain text.";
      }
      return Remember( st );
   }
   const KeyringResult r = KeyringLookup( g_id );
   if ( r.ok && r.found )
   {
      if ( IsPrintableAscii( r.secret ) )
      {
         st.key = String( r.secret );
         st.where = Where::Keyring;
      }
      else
         st.note = "The API key entry in the system keyring is not a valid key; enter it again in PI Copilot's settings.";
   }
   else if ( !r.ok )
      st.note = "Could not read the system keyring (" + r.error + ").";
   return Remember( st );
}

State Save( const String& key )
{
   State st;
   st.key = key;
   const String why = StoreVerified( key );
   if ( why.IsEmpty() )
   {
      Settings::Remove( g_settingsKey );
      st.where = Where::Keyring;
   }
   else
   {
      Settings::Write( g_settingsKey, key );
      st.where = Where::Settings;
      st.note = "The system keyring could not be used (" + why + "), so your API key is stored in PixInsight's "
                "settings in plain text.";
   }
   return Remember( st );
}

String Clear()
{
   Settings::Remove( g_settingsKey );
   g_cache = State();
   g_cached = false;
   const KeyringResult r = KeyringClear( g_id );
   return r.ok ? String() : "Could not remove the key from the system keyring (" + r.error + ").";
}

String DescribeWhere( const State& s )
{
   switch ( s.where )
   {
   case Where::Keyring:  return "stored in the system keyring";
   case Where::Settings: return "stored in PixInsight's settings (plain text)";
   default:              return "not set";
   }
}

void SetKeyringForSelfTest( const KeyringId& id, const IsoString& settingsKey )
{
   g_id = id;
   g_settingsKey = settingsKey;
   g_cache = State();
   g_cached = false;
}

} } // namespace pcl::KeyStore
