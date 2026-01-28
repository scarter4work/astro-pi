//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Unified Segmentation Palette
//
// This header provides a consistent color palette for the 21-class
// segmentation model, matching the Python implementation in:
// training_data/scripts/segmentation_palette.py
//
// IMPORTANT: Any changes to this palette must be synchronized with
// the Python version to maintain visualization consistency.

#ifndef __SegmentationPalette_h
#define __SegmentationPalette_h

#include "RegionStatistics.h"  // For RegionClass enum

#include <array>
#include <cstdint>

namespace pcl
{

// ----------------------------------------------------------------------------
// Segmentation Palette
//
// Unified color palette for 21-class astronomical image segmentation.
//
// Design Rationale:
// - Perceptually distinct: All 21 classes are visually separable
// - Semantically meaningful: Colors match astronomical expectations
//   (red for emission nebulae, blue for reflection, etc.)
// - Colorblind-friendly: Major categories use different hues/luminances
//
// Color Categories:
// - BACKGROUND (0): Dark blue-gray - neutral, non-intrusive
// - STARS (1-4): Yellow to white spectrum - bright, attention-grabbing
// - NEBULAE (5-8): Spectral colors matching real appearance
// - GALAXIES (9-12): Warm orange/purple tones
// - DUST LANES (13): Dark brown
// - STAR CLUSTERS (14-15): Distinct blue/peach tones
// - ARTIFACTS (16-20): Warning colors
// ----------------------------------------------------------------------------

namespace SegmentationPalette
{

// Number of segmentation classes
constexpr int NumClasses = 21;

// RGB color structure (0-255 values)
struct RGB8
{
   uint8_t r;
   uint8_t g;
   uint8_t b;

   // Convert to normalized 0.0-1.0 values
   constexpr double R() const { return r / 255.0; }
   constexpr double G() const { return g / 255.0; }
   constexpr double B() const { return b / 255.0; }
};

// The unified color palette (indexed by class ID)
// These values MUST match the Python CLASS_COLORS_RGB array exactly
constexpr std::array<RGB8, NumClasses> Colors = {{
   // 0: background - dark blue-gray
   { 26, 26, 51 },

   // Stars - yellow/gold/white spectrum
   { 255, 255, 0 },      // 1: star_bright - pure yellow
   { 255, 204, 51 },     // 2: star_medium - golden amber
   { 230, 191, 102 },    // 3: star_faint - pale gold
   { 255, 255, 255 },    // 4: star_saturated - pure white

   // Nebulae - spectral colors matching real appearance
   { 255, 51, 102 },     // 5: nebula_emission - red/magenta (H-alpha)
   { 102, 153, 255 },    // 6: nebula_reflection - blue
   { 51, 26, 38 },       // 7: nebula_dark - dark brown/maroon
   { 0, 204, 204 },      // 8: nebula_planetary - cyan/teal (OIII)

   // Galaxies - orange/purple spectrum
   { 255, 153, 51 },     // 9: galaxy_spiral - orange
   { 230, 128, 128 },    // 10: galaxy_elliptical - salmon/coral
   { 153, 102, 204 },    // 11: galaxy_irregular - violet
   { 255, 204, 0 },      // 12: galaxy_core - bright amber/yellow

   // Structural features
   { 102, 51, 26 },      // 13: dust_lane - dark brown
   { 153, 204, 255 },    // 14: star_cluster_open - light sky blue
   { 255, 179, 128 },    // 15: star_cluster_globular - peach/apricot

   // Artifacts - warning colors
   { 255, 0, 51 },       // 16: artifact_hot_pixel - bright red
   { 51, 255, 51 },      // 17: artifact_satellite - bright green
   { 255, 51, 255 },     // 18: artifact_diffraction - magenta
   { 153, 153, 51 },     // 19: artifact_gradient - olive/khaki
   { 102, 102, 102 },    // 20: artifact_noise - medium gray
}};

// Class names (indexed by class ID)
constexpr const char* ClassNames[NumClasses] = {
   "background",
   "star_bright",
   "star_medium",
   "star_faint",
   "star_saturated",
   "nebula_emission",
   "nebula_reflection",
   "nebula_dark",
   "nebula_planetary",
   "galaxy_spiral",
   "galaxy_elliptical",
   "galaxy_irregular",
   "galaxy_core",
   "dust_lane",
   "star_cluster_open",
   "star_cluster_globular",
   "artifact_hot_pixel",
   "artifact_satellite",
   "artifact_diffraction",
   "artifact_gradient",
   "artifact_noise",
};

// Human-readable display names
constexpr const char* ClassDisplayNames[NumClasses] = {
   "Background",
   "Bright Star",
   "Medium Star",
   "Faint Star",
   "Saturated Star",
   "Emission Nebula",
   "Reflection Nebula",
   "Dark Nebula",
   "Planetary Nebula",
   "Spiral Galaxy",
   "Elliptical Galaxy",
   "Irregular Galaxy",
   "Galaxy Core",
   "Dust Lane",
   "Open Cluster",
   "Globular Cluster",
   "Hot Pixel",
   "Satellite Trail",
   "Diffraction Spike",
   "Gradient",
   "Noise",
};

// ----------------------------------------------------------------------------
// Utility Functions
// ----------------------------------------------------------------------------

/**
 * Get the color for a class ID as normalized 0.0-1.0 RGB values.
 *
 * @param classId Integer class ID (0-20)
 * @param r Output: Red component (0.0-1.0)
 * @param g Output: Green component (0.0-1.0)
 * @param b Output: Blue component (0.0-1.0)
 */
inline void GetColor( int classId, double& r, double& g, double& b )
{
   if ( classId >= 0 && classId < NumClasses )
   {
      const RGB8& c = Colors[classId];
      r = c.R();
      g = c.G();
      b = c.B();
   }
   else
   {
      // Magenta for unknown classes (obvious error indicator)
      r = 1.0;
      g = 0.0;
      b = 1.0;
   }
}

/**
 * Get the color for a class ID as 0-255 RGB values.
 *
 * @param classId Integer class ID (0-20)
 * @param r Output: Red component (0-255)
 * @param g Output: Green component (0-255)
 * @param b Output: Blue component (0-255)
 */
inline void GetColor8( int classId, uint8_t& r, uint8_t& g, uint8_t& b )
{
   if ( classId >= 0 && classId < NumClasses )
   {
      const RGB8& c = Colors[classId];
      r = c.r;
      g = c.g;
      b = c.b;
   }
   else
   {
      // Magenta for unknown classes
      r = 255;
      g = 0;
      b = 255;
   }
}

/**
 * Get the color for a RegionClass enum value.
 *
 * @param rc RegionClass enum value
 * @param r Output: Red component (0.0-1.0)
 * @param g Output: Green component (0.0-1.0)
 * @param b Output: Blue component (0.0-1.0)
 */
inline void GetColorForRegion( RegionClass rc, double& r, double& g, double& b )
{
   GetColor( static_cast<int>( rc ), r, g, b );
}

/**
 * Get the class name for a class ID.
 *
 * @param classId Integer class ID (0-20)
 * @return Class name string, or "unknown" for invalid IDs
 */
inline const char* GetClassName( int classId )
{
   if ( classId >= 0 && classId < NumClasses )
      return ClassNames[classId];
   return "unknown";
}

/**
 * Get the display name for a class ID.
 *
 * @param classId Integer class ID (0-20)
 * @return Human-readable display name, or "Unknown" for invalid IDs
 */
inline const char* GetClassDisplayName( int classId )
{
   if ( classId >= 0 && classId < NumClasses )
      return ClassDisplayNames[classId];
   return "Unknown";
}

/**
 * Get the display name for a RegionClass enum value.
 *
 * @param rc RegionClass enum value
 * @return Human-readable display name
 */
inline const char* GetRegionDisplayName( RegionClass rc )
{
   return GetClassDisplayName( static_cast<int>( rc ) );
}

/**
 * Convert a class ID to a RegionClass enum value.
 *
 * @param classId Integer class ID (0-20)
 * @return Corresponding RegionClass, or RegionClass::Background for invalid IDs
 */
inline RegionClass ClassIdToRegionClass( int classId )
{
   if ( classId >= 0 && classId < NumClasses )
      return static_cast<RegionClass>( classId );
   return RegionClass::Background;
}

// ----------------------------------------------------------------------------
// Category Definitions
// ----------------------------------------------------------------------------

/**
 * Check if a class belongs to the "stars" category.
 */
inline bool IsStarClass( int classId )
{
   return classId >= 1 && classId <= 4;
}

/**
 * Check if a class belongs to the "nebulae" category.
 */
inline bool IsNebulaClass( int classId )
{
   return classId >= 5 && classId <= 8;
}

/**
 * Check if a class belongs to the "galaxies" category.
 */
inline bool IsGalaxyClass( int classId )
{
   return classId >= 9 && classId <= 12;
}

/**
 * Check if a class belongs to the "clusters" category.
 */
inline bool IsClusterClass( int classId )
{
   return classId == 14 || classId == 15;
}

/**
 * Check if a class belongs to the "artifacts" category.
 */
inline bool IsArtifactClass( int classId )
{
   return classId >= 16 && classId <= 20;
}

/**
 * Check if a class belongs to the "dust" category (dark nebula + dust lanes).
 */
inline bool IsDustClass( int classId )
{
   return classId == 7 || classId == 13;
}

} // namespace SegmentationPalette

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __SegmentationPalette_h
