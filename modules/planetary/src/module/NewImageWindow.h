// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NewImageWindow_h
#define __NewImageWindow_h

#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Image.h>
#include <pcl/String.h>

namespace pcl
{

// The image windows Planetary processes open (SERViewer's frame,
// PlanetaryStack's stack): a new 32-bit float window holding a copy of `image`
// (1 channel, or 3 as RGB) with view id `id` (PixInsight makes it unique) and
// `keywords`, shown. Returns the new view's id. Errors carry the prefix `what`
// ("SERViewer: <file>").
//
// Every fallible step of the caller's -- reading, normalising, debayering,
// building the keywords -- is done before this is called, so the window exists
// only for PCL's own calls: create, check its format, copy the pixels, set the
// keywords, show. Nothing half-made is ever shown: if a step before Show
// fails, the window is force-closed and that first error rethrown (a failure
// to close never replaces it).
//
// The window's wrapper lives on the heap and is deliberately never deleted
// once its window has been force-closed. ForceClose does not clear the
// wrapper's handle (~/PCL/src/pcl/ImageWindow.cpp: CloseImageWindow only), and
// destroying a wrapper whose core object is gone reads the refcount of
// whatever the core has put at that address since and may detach and free it:
// a proven SIGSEGV (memory pcl-view-handle-use-after-free: pi-copilot's stale
// View freed the running PixelMath instance, core-dump pointer match, 3 crashes
// in 12 runs). PCL offers no way to drop a handle without touching the core
// object (UIObject's destructor, SetHandle and TransferHandle all ask the
// core for its refcount first). The price is one leaked wrapper on a path that
// is practically unreachable; every View of the window is gone before the close.
IsoString ShowNewImageWindow( const Image& image, const IsoString& id, const FITSKeywordArray& keywords, const String& what );

} // namespace pcl

#endif // __NewImageWindow_h
