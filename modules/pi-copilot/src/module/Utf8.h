// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Utf8_h
#define PICopilot_Utf8_h

#include <pcl/String.h>

#include <string>

namespace pcl
{

// UTF-16 pcl::String -> UTF-8 std::string. Shared across increment-3 self-test
// sections (vision smoke, ViewContext, ProcessCatalog, ...) so it is defined
// exactly once, not duplicated per translation unit.
inline std::string U8( const String& s )
{
   return std::string( s.ToUTF8().c_str() );
}

} // namespace pcl

#endif // PICopilot_Utf8_h
