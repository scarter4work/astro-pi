// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_SafeFileWrite_h
#define PICopilot_SafeFileWrite_h

#include <pcl/ByteArray.h>
#include <pcl/String.h>

#include <functional>
#include <string>

namespace pcl
{

/*
 * Atomic, symlink-safe file output (CWE-59) for every file PI Copilot writes
 * at a predictable path (journey thumbnails, .xpsm, recipe.json, journey.md,
 * keeper copies). Contract, for every function below:
 *   - the TARGET is refused, untouched, if it exists as a symbolic link or as
 *     anything but a regular file (lstat; never followed, never replaced);
 *   - the bytes are checked first (optional `check`: "" = acceptable, else
 *     why not), then written to a fresh sibling temp in the SAME directory,
 *     created with mkstemp (O_CREAT|O_EXCL, so a planted name or symlink makes
 *     it fail closed), fsync'ed, given mode 0666 & ~umask, and rename()d over
 *     the target (atomic: a reader sees the old file or the new one, never a
 *     partial one);
 *   - on ANY failure the temp is removed and the target is left as it was.
 * The parent directory must exist (the caller creates it). Returns "" when
 * written, else a message naming the path and the reason. Never throws.
 * Root thread or any thread (POSIX calls only; the render variant's renderer
 * decides its own thread rules, e.g. Bitmap is root-thread only).
 */
using SafeFileCheck    = std::function<String( const ByteArray& data )>;
using SafeFileRenderer = std::function<void( const String& tempPath )>;

String SafeWriteFile( const String& path, const ByteArray& data, const SafeFileCheck& check = nullptr );

// UTF-8 text (e.g. nlohmann::json::dump(), U8(markdown)) -- same contract.
String SafeWriteTextFile( const String& path, const std::string& utf8, const SafeFileCheck& check = nullptr );

// For writers that only save to a path (Bitmap::Save, FileFormatInstance):
// render( tempPath ) writes into a private 0700 mkdtemp directory under the
// system temp dir (tempPath keeps the target's file name, so the extension
// selects the format); the bytes are read back without following links and
// go through SafeWriteFile( path, bytes, check ). The private directory is
// removed on every path, including a renderer that throws.
String SafeRenderFile( const String& path, const SafeFileRenderer& render, const SafeFileCheck& check = nullptr );

// Ready-made check: the bytes start with the JPEG SOI marker (FF D8).
String SafeCheckJpeg( const ByteArray& data );

// Self-test only: make the next SafeWriteFile fail after its temp is fully
// written, before the rename (proves the temp is cleaned up). One-shot.
void SetSafeFileWriteFailBeforeRenameForSelfTest( bool on );

} // namespace pcl

#endif // PICopilot_SafeFileWrite_h
