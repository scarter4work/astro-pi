// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "KeyStore.h"

#include <pcl/Settings.h>

namespace pcl { namespace KeyStore {

static const IsoString kAnthropicApiKeyKey = "PICopilot/AnthropicApiKey";

String Load()
{
   String s; // pre-init: Settings::Read leaves s untouched if the key is unset
   Settings::Read( kAnthropicApiKeyKey, s );
   return s;
}

void Save( const String& key )
{
   Settings::Write( kAnthropicApiKeyKey, key );
}

void Clear()
{
   Settings::Remove( kAnthropicApiKeyKey );
}

} } // namespace pcl::KeyStore
