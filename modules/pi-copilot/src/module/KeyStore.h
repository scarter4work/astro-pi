// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_KeyStore_h
#define PICopilot_KeyStore_h

#include <pcl/String.h>

namespace pcl { namespace KeyStore {

// Persists the user's Anthropic API key in PixInsight's local Settings
// store (per-user, not global — see pcl::Settings local-space Read/Write).
String Load();               // "" if unset
void   Save( const String& );

} } // namespace pcl::KeyStore

#endif // PICopilot_KeyStore_h
