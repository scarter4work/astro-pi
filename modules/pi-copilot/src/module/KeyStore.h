// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_KeyStore_h
#define PICopilot_KeyStore_h

#include "Keyring.h"

#include <pcl/String.h>

namespace pcl { namespace KeyStore {

enum class Where { None, Keyring, Settings };

// The key and where it lives. note: a plain-language sentence for the user
// (a migration that happened, or why the keyring could not be used); never
// contains the key.
struct State
{
   String key;
   Where  where = Where::None;
   String note;
};

// Keyring first. A key found in PixInsight Settings (0.1.1.x and earlier, or a
// fallback) is trimmed and must be printable ASCII (else: no key, a note, and
// nothing is migrated); a valid one is written to the keyring, READ BACK and
// compared, and only then removed from Settings. Cached once a key is known (the next Save()/Clear()
// or SetKeyringForSelfTest() refreshes it). Root thread only.
State Load();

// Keyring (verified) and the Settings copy removed; if the keyring cannot be
// used, Settings + a note saying why. The caller validates the key.
State Save( const String& key );

// Removes the key from both places (the Settings copy always). note: "" when
// done, else a plain sentence; warning: the keyring entry may still be there
// (show it as a warning). With secret-tool not installed there can be no
// keyring copy made by PI Copilot, so that note is informational only.
struct Cleared
{
   String note;
   bool   warning = false;
};
Cleared Clear();

// The panel's notice when Load() found no key. A dismissed keyring unlock
// prompt looks exactly like "no key" (see KeyringResult), so it says so.
String NoKeyNote();

// "stored in the system keyring" / "stored in PixInsight's settings (plain
// text)" / "not set".
String DescribeWhere( const State& state );

// Self-test only: another keyring item and Settings key, and a dropped cache.
void SetKeyringForSelfTest( const KeyringId& id, const IsoString& settingsKey );

} } // namespace pcl::KeyStore

#endif // PICopilot_KeyStore_h
