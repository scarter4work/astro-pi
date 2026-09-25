// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Keyring_h
#define PICopilot_Keyring_h

#include <pcl/String.h>

#include <functional>

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
// notInstalled: the program could not be found (env exit 127), so no keyring
// item can have been written through it either.
//
// A lookup whose unlock prompt was dismissed is expected to end like "no such
// item" (secret-tool returns 1 with no output when libsecret yields no value;
// not reproducible headlessly -- it needs a locked keyring on a live
// desktop), so found == false may also mean a locked keyring: callers word
// it that way (KeyStore::NoKeyNote()).
struct KeyringResult
{
   bool      ok = false;
   bool      found = false;
   bool      notInstalled = false;
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

// A keyring call that is still running after this long (typically the
// desktop's unlock prompt) is announced through the current wait notifier.
constexpr int PICopilotKeyringWaitNoticeMs = 300;

// Installs a notifier for the lifetime of the scope (the previous one is
// restored after): notify(true) once a keyring call has run for
// PICopilotKeyringWaitNoticeMs, notify(false) when that call ends. Calls that
// finish sooner notify nothing, so nothing flickers. The notifier runs on the
// root thread inside the call's event-pumping loop, so a caption it sets is
// painted while the call blocks. Root thread only.
class KeyringWaitScope
{
public:
   explicit KeyringWaitScope( std::function<void( bool waiting )> notify );
   ~KeyringWaitScope();
   KeyringWaitScope( const KeyringWaitScope& ) = delete;
   KeyringWaitScope& operator =( const KeyringWaitScope& ) = delete;
private:
   std::function<void( bool )> m_previous;
};

} // namespace pcl

#endif // PICopilot_Keyring_h
