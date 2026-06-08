// ----------------------------------------------------------------------------
// AstroStretchStudio Zone HDR Engine Header
// ----------------------------------------------------------------------------

#ifndef __AstroStretchStudioZoneHDR_h
#define __AstroStretchStudioZoneHDR_h

#include <pcl/Image.h>
#include <pcl/Array.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// Zone structure - represents a single brightness zone
// ----------------------------------------------------------------------------

struct Zone
{
   double centerValue;    // Luminance center of zone (0-1)
   double lowerBound;     // Start of zone (0-1)
   double upperBound;     // End of zone (0-1)
   double intensity;      // User adjustment (-1 to +1), 0 = no change
   double saturation;     // User adjustment (-1 to +1), 0 = no change
   String name;           // Display name (e.g., "Shadows", "Core", "Highlights")

   Zone()
      : centerValue( 0.5 )
      , lowerBound( 0.0 )
      , upperBound( 1.0 )
      , intensity( 0.0 )
      , saturation( 0.0 )
      , name( "Zone" )
   {
   }

   Zone( double center, double lower, double upper, const String& n = "Zone" )
      : centerValue( center )
      , lowerBound( lower )
      , upperBound( upper )
      , intensity( 0.0 )
      , saturation( 0.0 )
      , name( n )
   {
   }
};

// ----------------------------------------------------------------------------
// ZoneHDREngine - Main processing engine for zone-based HDR
// ----------------------------------------------------------------------------

class ZoneHDREngine
{
public:

   // Maximum number of zones to prevent UI overflow
   static const int MaxZones = 8;

   // Minimum number of zones (at least shadows/mids/highlights)
   static const int MinZones = 3;

   ZoneHDREngine();
   ~ZoneHDREngine();

   // -------------------------------------------------------------------------
   // Zone Detection
   // -------------------------------------------------------------------------

   // Detect zones from histogram peaks in the luminance image
   // Returns the number of zones detected
   int DetectZones( const Image& luminance );

   // Get the detected zones
   const Array<Zone>& Zones() const { return m_zones; }
   Array<Zone>& Zones() { return m_zones; }

   // Get zone count
   int ZoneCount() const { return int( m_zones.Length() ); }

   // -------------------------------------------------------------------------
   // Mask Generation
   // -------------------------------------------------------------------------

   // Generate soft masks for all zones based on luminance
   // Masks are stored internally and used by ApplyZoneAdjustments
   void GenerateMasks( const Image& luminance );

   // Get mask for a specific zone (for preview purposes)
   // Returns empty image if zone index is invalid or masks not generated
   Image GetZoneMask( int zoneIndex ) const;

   // -------------------------------------------------------------------------
   // Zone Adjustment Application
   // -------------------------------------------------------------------------

   // Apply zone intensity and saturation adjustments to the image
   // luminance: the stretched luminance channel
   // colorImage: the full color image (will be modified in-place)
   void ApplyZoneAdjustments( Image& luminance, Image& colorImage ) const;

   // -------------------------------------------------------------------------
   // Preview Support
   // -------------------------------------------------------------------------

   enum PreviewMode
   {
      PreviewOff = 0,       // Normal stretched image
      PreviewMaskOverlay,   // Overlay zone masks with colors
      PreviewSoloZone       // Show only selected zone's effect
   };

   // Generate preview image for zone visualization
   // sourceImage: the stretched image
   // mode: preview mode
   // selectedZone: zone index for SoloZone mode
   void GeneratePreview( Image& previewImage,
                         const Image& sourceImage,
                         PreviewMode mode,
                         int selectedZone ) const;

private:

   Array<Zone> m_zones;
   Array<Image> m_masks;  // Soft masks for each zone

   // -------------------------------------------------------------------------
   // Internal Methods
   // -------------------------------------------------------------------------

   // Compute histogram of luminance image
   void ComputeHistogram( const Image& luminance, FVector& histogram, int bins ) const;

   // Smooth histogram using gaussian kernel
   void SmoothHistogram( FVector& histogram, int kernelSize ) const;

   // Find peaks in histogram using derivative analysis
   void FindHistogramPeaks( const FVector& histogram, Array<int>& peakBins ) const;

   // Find valleys between peaks for zone boundaries
   void FindZoneBoundaries( const FVector& histogram,
                            const Array<int>& peakBins,
                            Array<double>& boundaries ) const;

   // Generate zone names based on luminance values
   String GenerateZoneName( double centerValue, int zoneIndex, int totalZones ) const;

   // Compute soft mask weight for a pixel at given luminance
   double ComputeMaskWeight( double luminance, const Zone& zone ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __AstroStretchStudioZoneHDR_h

// ----------------------------------------------------------------------------
