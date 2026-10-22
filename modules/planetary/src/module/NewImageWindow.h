// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NewImageWindow_h
#define __NewImageWindow_h

#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Image.h>
#include <pcl/String.h>

#include <functional>

namespace pcl
{

// The image windows Planetary processes open (SERViewer's frame,
// PlanetaryStack's stack): a hidden 32-bit float window of `channels` (RGB when
// 3) with view id `id` (PixInsight makes it unique), its image filled by
// `fill`, `keywords` set, then shown. Returns the new view's id. Errors carry
// the prefix `what` ("SERViewer: <file>").
//
// Nothing half-made is ever shown: if creating, checking or filling fails, the
// still-hidden window is closed and that first error is rethrown -- a failure to
// close it never replaces it. The closed window's handle is never used again:
// PCL does not clear the handle of a window the core has destroyed (memory
// pcl-view-handle-use-after-free), so its wrapper is destroyed straight after
// the close, before anything else runs, and every View of it before the close.
IsoString ShowNewImageWindow( int width, int height, int channels, const IsoString& id, const FITSKeywordArray& keywords,
                              const std::function<void( Image& )>& fill, const String& what );

} // namespace pcl

#endif // __NewImageWindow_h
