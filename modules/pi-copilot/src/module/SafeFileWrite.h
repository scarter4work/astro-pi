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
 *     created with a random name and O_CREAT|O_EXCL|O_NOFOLLOW (a planted
 *     name or symlink makes it fail closed) and the mode of its class (below),
 *     fsync'ed, and rename()d over the target (atomic: a reader sees the old
 *     file or the new one, never a partial one);
 *   - on ANY failure the temp is removed and the target is left as it was.
 * The parent directory must exist (the caller creates it, e.g. with
 * EnsurePrivateDirectory). Returns "" when written, else a message naming the
 * path and the reason. Never throws.
 * Root thread or any thread (POSIX calls only; the process umask is never
 * read or changed; the render variant's renderer decides its own thread
 * rules, e.g. Bitmap is root-thread only).
 */

// The final mode is fixed by the file's class, minus the umask, which the
// kernel applies when the temp is created (never inherited from a replaced
// file). Every caller states the class; there is no default.
enum class SafeFileMode
{
   Shared,    // 0644: thumbnails, exported keeper files (.xpsm, recipe.json, journey.md, copies)
   Private    // 0600: anything that must stay the user's own
};

using SafeFileCheck    = std::function<String( const ByteArray& data )>;
using SafeFileRenderer = std::function<void( const String& tempPath )>;

String SafeWriteFile( const String& path, const ByteArray& data, SafeFileMode mode, const SafeFileCheck& check = nullptr );

// UTF-8 text (e.g. nlohmann::json::dump(), U8(markdown)) -- same contract.
String SafeWriteTextFile( const String& path, const std::string& utf8, SafeFileMode mode, const SafeFileCheck& check = nullptr );

// For writers that only save to a path (Bitmap::Save, FileFormatInstance):
// render( tempPath ) writes into a private 0700 mkdtemp directory under the
// system temp dir (tempPath keeps the target's file name, so the extension
// selects the format); the bytes are read back without following links and
// go through SafeWriteFile( path, bytes, mode, check ). The private directory
// is removed on every path, including a renderer that throws.
String SafeRenderFile( const String& path, SafeFileMode mode, const SafeFileRenderer& render, const SafeFileCheck& check = nullptr );

// A directory PI Copilot owns (journey library root, <journey>/thumbs,
// <journey>/export): every missing component of the absolute path is created
// 0700 with mkdir(), which never follows a link. The final component must then
// be a real directory (not a symbolic link), owned by this user, and not
// writable by other users (group-writable only for the user's own group), so
// nobody else can plant a name inside it. Pre-existing ancestors are the
// user's own layout and are not judged. "" = ready, else why not. Never throws.
String EnsurePrivateDirectory( const String& dir );

// Ready-made check: the bytes start with the JPEG SOI marker (FF D8).
String SafeCheckJpeg( const ByteArray& data );

// Self-test only: make the next SafeWriteFile fail after its temp is fully
// written, before the rename (proves the temp is cleaned up). One-shot.
void SetSafeFileWriteFailBeforeRenameForSelfTest( bool on );

} // namespace pcl

#endif // PICopilot_SafeFileWrite_h
