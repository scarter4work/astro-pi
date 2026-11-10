// The solver's star detection: ImageSolver 6.6's detectImageStars() / detectStarsForBlindRetry()
// with pcl::StarDetector and pcl::PSFFit (plan Tasks 14-15).
#ifndef __GDG_SolverDetect_h
#define __GDG_SolverDetect_h

#include "core/SolveMath.h"

#include <pcl/ImageVariant.h>

#include <memory>
#include <vector>

namespace pcl
{

/// StarDetector parameters ImageSolver varies between attempts (the rest are its defaults:
/// hot pixel filter 1, no noise reduction, sensitivity 0.5, no clustered sources, minimum
/// structure size 0).
struct DetectParams
{
   int    structureLayers = 5;
   double peakResponse = 0.5;
   double brightThreshold = 3.0;
   double maxStarDistortion = 0.6;

   bool operator ==( const DetectParams& ) const = default;

   /// blindRetryDetectionParameters(): one more scale (up to 8), the most permissive peak
   /// response, bright threshold and distortion.
   DetectParams Permissive() const
   {
      return DetectParams{ std::min( 8, structureLayers + 1 ), 1.0, 1.0, 1.0 };
   }
};

struct DetectedStars
{
   std::vector<gdg::Pt>  pos;      // PSF centroids, image coordinates, decreasing PSF signal
   std::vector<gdg::Box> rect;     // centroid +- (sx/2, sy/2)
   std::vector<double>   flux;     // PSF signal
   int    rawCount = 0;            // StarDetector detections, before PSF fitting
   int    psfFits = 0;             // valid PSF fits
   double brightFWHM = 0;          // median FWHM of the 200 brightest fits, pixels
   int    tolerance = 2;           // star matching tolerance, pixels

   std::unique_ptr<gdg::RectIndex> tree; // over rect (the conflict-free stars)

   size_t size() const { return pos.size(); }
};

/// Detection + PSF fitting (Gaussian, elliptic, in parallel) + removal of conflicting sources +
/// matching tolerance, on a one-channel image. With binning k > 1 the image is first reduced by
/// k x k block averaging and the positions are scaled back to the image (binnedImage()).
DetectedStars DetectImageStars( const ImageVariant& gray, const DetectParams& p, int binning = 1 );

/// The HSI intensity of `image` (its only channel when it is grayscale), as a 32-bit float image:
/// ImageSolver's generateWorkingGrayscaleImage().
ImageVariant WorkingGrayscale( const ImageVariant& image );

} // namespace pcl

#endif // __GDG_SolverDetect_h
