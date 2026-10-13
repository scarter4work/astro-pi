// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewContext_h
#define PICopilot_ViewContext_h

#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <string>

namespace pcl
{

// FITS keyword caps for the context sent to the model (token cost).
constexpr int PICopilotMaxFitsKeywords   = 60;
constexpr int PICopilotMaxFitsValueChars = 80;

// FITS keywords never sent to the model (privacy): observing-site location
// and observer identity. Matched on the trimmed, upper-cased keyword name.
// Redacted keywords do not consume a PICopilotMaxFitsKeywords slot; they are
// counted in the context's "fitsKeywordsRedacted" (NOT in
// "fitsKeywordsOmitted", which counts only cap overflow).
constexpr const char* PICopilotRedactedFitsKeywords[] =
{
   "SITELAT", "SITELONG", "SITEELEV",
   "OBSGEO-B", "OBSGEO-L", "OBSGEO-H",
   "LAT-OBS", "LONG-OBS", "ALT-OBS",
   "OBSERVER"
};

// True iff name (trimmed, any case) is in PICopilotRedactedFitsKeywords.
bool IsRedactedFitsKeyword( const IsoString& name );

// A FITS keyword value with every absolute filesystem path in it reduced to
// its file name (privacy: a path usually names the user's account; e.g. a
// HISTORY or FILENAME value written by a stacker). A path starts with '/', a
// double backslash (network share), "~/", or a drive ("X:\" or "X:/"), at the
// start of the value or after a space, tab, quote, '=', '(', '[', ',', ';' or
// ':'; it is cut back to what follows its last '/' or '\' before the next
// path. "1/2", "$T/2", dates such as "2026/10/13" and plain names are
// unchanged. Pure.
std::string FitsValueWithoutPaths( const std::string& value );

// What the context carries for the image's file: the file name only
// (File::ExtractNameAndExtension), never the directory -- a local path
// usually contains the user's account name. Empty for an empty path.
String ViewContextFileName( const String& filePath );

/*
 * JSON description of a view for the model: identity (view ids and the
 * file NAME, see ViewContextFileName), geometry, robust per-channel
 * statistics of the REAL (usually linear) data, and the first
 * PICopilotMaxFitsKeywords FITS keywords that are not redacted
 * (PICopilotRedactedFitsKeywords), each value with its paths reduced to file
 * names (FitsValueWithoutPaths; counted in "fitsValuesPathsScrubbed"). There is deliberately no process
 * history: PCL exposes no API for it.
 *
 * Root thread only. Reads the image under AutoViewWriteLock (blocks writers,
 * readers unaffected) and never modifies it: statistics use explicit
 * (rect, channel, channel) arguments -- no SelectChannel() calls.
 * Throws pcl::Error for a null view, a view with no image, or a complex image.
 */
nlohmann::json BuildViewContext( const View& view );

} // namespace pcl

#endif // PICopilot_ViewContext_h
