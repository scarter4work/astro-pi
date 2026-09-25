// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Keyring_h
#define PICopilot_Keyring_h

#include <pcl/String.h>

namespace pcl
{

// Which keyring item holds the key: secret-tool attributes
// "service <service> account <account>". program is looked up on PATH.
struct KeyringId
{
   String program = "secret-tool";
   String service = "picopilot";
   String account = "anthropic-api-key";
};

// ok: secret-tool ran and answered (found says whether an item exists).
// error: why it could not be used -- never contains the secret.
struct KeyringResult
{
   bool      ok = false;
   bool      found = false;
   IsoString secret;
   String    error;
};

// How long a keyring call may take, including the desktop's unlock prompt.
constexpr int PICopilotKeyringTimeoutMs = 60000;

// Root thread only (pcl::ExternalProcess). Each call runs
// /usr/bin/env -u LD_LIBRARY_PATH <program> ... (PixInsight's own library
// path would otherwise be inherited and can break system binaries), pumps
// events (user input excluded) while waiting, and never throws.
KeyringResult KeyringLookup( const KeyringId& id );
KeyringResult KeyringStore( const KeyringId& id, const String& label, const IsoString& secret );
KeyringResult KeyringClear( const KeyringId& id );

} // namespace pcl

#endif // PICopilot_Keyring_h
