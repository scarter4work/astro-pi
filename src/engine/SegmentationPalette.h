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
// This header provides a consistent color palette for the 7-class
// segmentation model (v2.0 taxonomy).
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
// Unified color palette for 7-class astronomical image segmentation.
//
// Design Rationale:
// - Perceptually distinct: All 7 classes are visually separable
// - Semantically meaningful: Colors match astronomical expectations
// - Colorblind-friendly: Major categories use different hues/luminances
//
// Color Categories:
// - BACKGROUND (0): Dark blue-gray - neutral, non-intrusive
// - BRIGHT COMPACT (1): Yellow - bright stellar cores + diffraction
// - FAINT COMPACT (2): Golden amber - faint stars + clusters
// - BRIGHT EXTENDED (3): Red/magenta - nebulae, galaxies, cirrus
// - DARK EXTENDED (4): Dark brown/maroon - dark nebulae, dust lanes
// - ARTIFACT (5): Bright red - hot pixels, satellite trails
// - STAR HALO (6): Pale yellow/gold - diffuse glow around stars
// ----------------------------------------------------------------------------

namespace SegmentationPalette
{

// Number of segmentation classes
constexpr int NumClasses = 7;

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
constexpr std::array<RGB8, NumClasses> Colors = {{
   { 26, 26, 51 },       // 0: Background - dark blue-gray
   { 255, 255, 0 },      // 1: BrightCompact - pure yellow
   { 255, 204, 51 },     // 2: FaintCompact - golden amber
   { 255, 51, 102 },     // 3: BrightExtended - red/magenta
   { 51, 26, 38 },       // 4: DarkExtended - dark brown/maroon
   { 255, 0, 51 },       // 5: Artifact - bright red
   { 200, 200, 100 },    // 6: StarHalo - pale yellow/gold
}};

// Class names (indexed by class ID)
constexpr const char* ClassNames[NumClasses] = {
   "background",
   "bright_compact",
   "faint_compact",
   "bright_extended",
   "dark_extended",
   "artifact",
   "star_halo",
};

// Human-readable display names
constexpr const char* ClassDisplayNames[NumClasses] = {
   "Background",
   "Bright Compact",
   "Faint Compact",
   "Bright Extended",
   "Dark Extended",
   "Artifact",
   "Star Halo",
};

// ----------------------------------------------------------------------------
// Utility Functions
// ----------------------------------------------------------------------------

/**
 * Get the color for a class ID as normalized 0.0-1.0 RGB values.
 *
 * @param classId Integer class ID (0-6)
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
 * @param classId Integer class ID (0-6)
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
 * @param classId Integer class ID (0-6)
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
 * @param classId Integer class ID (0-6)
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
 * @param classId Integer class ID (0-6)
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
 * Includes BrightCompact (1), FaintCompact (2), and StarHalo (6).
 */
inline bool IsStarClass( int classId )
{
   return classId == 1 || classId == 2 || classId == 6;
}

/**
 * Check if a class belongs to the "nebulae" category.
 * BrightExtended (3) covers all nebula types.
 */
inline bool IsNebulaClass( int classId )
{
   return classId == 3;
}

/**
 * Check if a class belongs to the "galaxies" category.
 * BrightExtended (3) covers all galaxy types.
 */
inline bool IsGalaxyClass( int classId )
{
   return classId == 3;
}

/**
 * Check if a class belongs to the "clusters" category.
 * FaintCompact (2) covers star clusters.
 */
inline bool IsClusterClass( int classId )
{
   return classId == 2;
}

/**
 * Check if a class belongs to the "artifacts" category.
 */
inline bool IsArtifactClass( int classId )
{
   return classId == 5;
}

/**
 * Check if a class belongs to the "dust" category (dark nebula + dust lanes).
 */
inline bool IsDustClass( int classId )
{
   return classId == 4;
}

/**
 * Check if a class is an extended emission feature.
 * BrightExtended (3) covers all extended emission.
 */
inline bool IsExtendedEmissionClass( int classId )
{
   return classId == 3;
}

} // namespace SegmentationPalette

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __SegmentationPalette_h
