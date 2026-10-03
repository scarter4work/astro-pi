// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// Console text the module writes, built as plain std::string so it can be
// unit-tested without a PixInsight process.  Nothing here includes PCL.
//
// Both builders return UTF-8.  PCL decodes a plain const char* as
// ISO-8859-1, so every caller MUST wrap the result in
// pcl::String::UTF8ToUTF16(...) before handing it to Console::WriteLn --
// otherwise the banner's block characters ship as garbage.

#ifndef __NukeXConsoleText_hpp
#define __NukeXConsoleText_hpp

#include <string>

namespace nukex {

/*!
 * The console text for one per-frame detail line, indented two spaces per
 * nesting level.
 *
 * Always opens with the "<end><cbr>" tag pair: <end> moves the cursor past
 * the last character in the console and <cbr> breaks the line only if the
 * cursor is not already at the start of an empty one.  Without it, the
 * progress monitor -- which writes its percentage IN PLACE on the current
 * line -- overwrites the detail text mid-word.
 *
 * \a depth of zero or less produces no indent.
 */
std::string console_detail_line( int depth, const std::string& detail );

/*!
 * The NukeX startup banner: six rows of block-art wordmark under a
 * cyan-to-violet horizontal gradient, then a line carrying the module
 * version and author.
 *
 * Returns the rows joined by '\n' with no trailing newline; the caller
 * writes each one separately.  Every line ends with an explicit colour
 * reset so nothing bleeds into later console output.  The version comes
 * from NUKEX_VERSION_STRING, so it cannot drift from the build.
 */
std::string nukex_banner();

} // namespace nukex

#endif // __NukeXConsoleText_hpp
