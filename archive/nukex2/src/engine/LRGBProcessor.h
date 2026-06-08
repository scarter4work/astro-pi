//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// LRGB Processor - Luminance/Chrominance processing

#ifndef __LRGBProcessor_h
#define __LRGBProcessor_h

#include <pcl/Image.h>
#include <pcl/String.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// LRGB Configuration
// ----------------------------------------------------------------------------

struct LRGBConfig
{
   // Processing mode
   bool enabled = true;                   // Enable LRGB mode
   bool processLuminanceOnly = true;      // Apply stretch only to L

   // Luminance coefficients (Rec. 709 by default)
   double luminanceR = 0.2126;
   double luminanceG = 0.7152;
   double luminanceB = 0.0722;

   // Chrominance handling
   bool preserveChroma = true;            // Preserve color ratios
   bool boostChroma = false;              // Boost saturation
   double chromaBoost = 0.0;              // Saturation boost amount (0-1)
   double chromaProtection = 0.0;         // Protect saturated colors (0-1)

   // Color balance
   bool preserveColorBalance = true;      // Maintain R/G/B ratios
   bool neutralBackground = false;        // Force neutral background
   double neutralThreshold = 0.05;        // Threshold for background detection

   // Advanced
   double luminanceWeight = 1.0;          // Weight of stretched L vs original
   double colorWeight = 1.0;              // Weight of original color
};

// ----------------------------------------------------------------------------
// LRGB Processor
//
// Separates luminance and chrominance for independent processing.
// Allows stretching luminance while preserving color information.
// ----------------------------------------------------------------------------

class LRGBProcessor
{
public:

   LRGBProcessor( const LRGBConfig& config = LRGBConfig() );

   // Separate RGB into L, a, b (or similar color space)
   void SeparateLuminance( const Image& rgb, Image& luminance, Image& chrominance ) const;

   // Combine luminance and chrominance back to RGB
   void CombineLuminance( const Image& luminance, const Image& chrominance, Image& rgb ) const;

   // Apply stretched luminance to original RGB
   // Takes original RGB and stretched luminance, returns new RGB
   Image ApplyStretchedLuminance( const Image& originalRGB,
                                   const Image& stretchedLuminance ) const;

   // Extract luminance from RGB
   Image ExtractLuminance( const Image& rgb ) const;

   // Extract chrominance (color ratios)
   Image ExtractChrominance( const Image& rgb ) const;

   // Apply saturation boost
   void ApplySaturationBoost( Image& rgb, double amount ) const;

   // Protect saturated colors during stretch
   Image CreateSaturationMask( const Image& rgb ) const;

   // Neutralize background
   void NeutralizeBackground( Image& rgb, double threshold ) const;

   // Convert between color spaces
   static void RGBtoLab( double r, double g, double b,
                          double& L, double& a, double& labB );
   static void LabToRGB( double L, double a, double labB,
                          double& r, double& g, double& b );

   static void RGBtoHSL( double r, double g, double b,
                          double& h, double& s, double& l );
   static void HSLtoRGB( double h, double s, double l,
                          double& r, double& g, double& b );

   // Configuration
   const LRGBConfig& Config() const { return m_config; }
   void SetConfig( const LRGBConfig& config ) { m_config = config; }

private:

   LRGBConfig m_config;

   // Compute luminance for a single pixel
   double ComputeLuminance( double r, double g, double b ) const;

   // Apply color from chrominance to luminance
   void ApplyColorToLuminance( double L, double chromaR, double chromaG, double chromaB,
                                double& outR, double& outG, double& outB ) const;
};

// ----------------------------------------------------------------------------
// Color Space Utilities
// ----------------------------------------------------------------------------

namespace ColorSpace
{
   // RGB to/from CIE XYZ
   void RGBtoXYZ( double r, double g, double b, double& x, double& y, double& z );
   void XYZtoRGB( double x, double y, double z, double& r, double& g, double& b );

   // CIE Lab
   void XYZtoLab( double x, double y, double z, double& L, double& a, double& b );
   void LabToXYZ( double L, double a, double b, double& x, double& y, double& z );

   // CIE LCh (cylindrical Lab)
   void LabToLCh( double L, double a, double b, double& Lout, double& C, double& h );
   void LChToLab( double L, double C, double h, double& Lout, double& a, double& b );

   // HSV/HSB
   void RGBtoHSV( double r, double g, double b, double& h, double& s, double& v );
   void HSVtoRGB( double h, double s, double v, double& r, double& g, double& b );

   // Compute saturation
   double ComputeSaturation( double r, double g, double b );

   // Preserve color ratios when scaling luminance
   void ScaleWithColorPreservation( double origR, double origG, double origB,
                                     double newLuminance,
                                     double& newR, double& newG, double& newB );
}

// ----------------------------------------------------------------------------
// Chrominance Blend Modes
// ----------------------------------------------------------------------------

enum class ChromaBlendMode
{
   Normal,           // Standard recombination
   Preserve,         // Preserve original color ratios
   Boost,            // Increase saturation
   Desaturate,       // Decrease saturation
   ColorOnly,        // Use new L, keep original color
   LuminanceOnly     // Use new L, neutral color
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __LRGBProcessor_h
