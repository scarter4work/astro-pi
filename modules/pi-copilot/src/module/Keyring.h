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
//
// A known transient (keyring-flake-investigation.md, T-keyring): ~1/256
// secret-service sessions derive a mismatched key (gnutls strips a
// leading-zero DH shared-secret byte; KDE ksecretd pads it). Each
// secret-tool call opens its own session, so a fresh process is an
// independent chance. KeyringStore/KeyringLookup retry this specific,
// unambiguous signature internally (bounded); callers never see it unless
// every attempt fails.
struct KeyringResult
{
   bool      ok = false;
   bool      found = false;
   bool      notInstalled = false;
   IsoString secret;
   String    error;

   // Only meaningful when ok && !found: every lookup attempt (bounded, see
   // PICopilotKeyringMaxAttempts below) silently missed, but the last
   // disambiguating `secret-tool search` before giving up still found the
   // item -- so this is not a genuine "no key", the item is there but could
   // not be read this time (KeyStore::Load() words it that way, distinct
   // from NoKeyNote(), via its own Where::Unreadable).
   bool      existsButUnreadable = false;
};

// Existence-only answer from `secret-tool search` (no --unlock, so it never
// prompts and skips locked items). secret-tool search prints the secret in
// plaintext when it finds an item, so this never carries it -- exists is
// the only thing callers can learn.
struct KeyringExistsResult
{
   bool   ok = false;
   bool   exists = false;
   bool   notInstalled = false;
   String error;
};

// How long a keyring call may take, including the desktop's unlock prompt.
constexpr int PICopilotKeyringTimeoutMs = 60000;

// A fresh secret-tool process opens a fresh DH session (keyring-flake-
// investigation.md: failure is per-session and independent across processes,
// p ~= 1/256), so a bounded retry of the same call is a correct remedy for
// that one signature. KeyringStore/KeyringLookup retry up to this many
// attempts total (review m2: exposed here, not just in Keyring.cpp's
// anonymous namespace, so KeyStore's user-facing note can never drift from
// the actual bound).
constexpr int PICopilotKeyringMaxAttempts = 3;

// Root thread only (pcl::ExternalProcess). Each call runs
// /bin/sh -c '...' sh <errfile> <program> <args...>, which sets `program`'s
// real stderr (fd 2) to a private, per-call file before exec'ing
// /usr/bin/env -u LD_LIBRARY_PATH <program> ... (PixInsight's own library
// path would otherwise be inherited and can break system binaries) --
// pcl::ExternalProcess reports the child's real stderr through
// StandardOutput() in this PixInsight build/version (measured, GUI and
// automation mode both; RedirectStandardError() is a silent no-op), so this
// is the only way to keep stdout and stderr genuinely separate. Pumps events
// (user input excluded) while waiting, and never throws.
KeyringResult KeyringLookup( const KeyringId& id );
KeyringResult KeyringStore( const KeyringId& id, const String& label, const IsoString& secret );
KeyringResult KeyringClear( const KeyringId& id );

// `secret-tool search` for the same attributes, without --unlock. Used
// internally by KeyringLookup to disambiguate a silent miss from the
// session-mismatch transient; exposed for tests.
KeyringExistsResult KeyringSearchExists( const KeyringId& id );

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
