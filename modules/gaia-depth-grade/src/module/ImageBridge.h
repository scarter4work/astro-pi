// ImageBridge: PixInsight images <-> gdg::Image (plan Task 11).
//
// gdg::Image is the Python core's frame: data[(y*width + x)*channels + c], the numpy array
// astropy read from the FITS file PixInsight saved. PixInsight writes FITS with ROWORDER =
// 'TOP-DOWN' by default, so data[0] is PixInsight's top row (measured 2026-11-10): the core keeps
// PixInsight's row order and no flip is needed in either direction (the plan's "vertical-flip
// bridge" assumed bottom-up FITS; the probe `sky_adapter_convention` checks this). Samples are
// normalised to [0,1] doubles (PixelTraits::FromSample), as astropy reads a PixInsight-saved
// floating-point FITS. Only nominal channels are copied (no alpha).
#ifndef __GDG_ImageBridge_h
#define __GDG_ImageBridge_h

#include "core/Image.h"

#include <pcl/Image.h>
#include <pcl/ImageVariant.h>

namespace pcl
{

class ImageBridge
{
public:

   /// A real-valued PixInsight image (any sample type) as a core image. Throws pcl::Error for
   /// a complex or empty image.
   static gdg::Image ToCore( const ImageVariant& image );

   /// A core image (1 or 3 channels) into a 32-bit float PixInsight image, reallocated to
   /// the core image's geometry (grayscale or RGB).
   static void ToPcl( const gdg::Image& core, pcl::Image& out );
};

} // namespace pcl

#endif // __GDG_ImageBridge_h
