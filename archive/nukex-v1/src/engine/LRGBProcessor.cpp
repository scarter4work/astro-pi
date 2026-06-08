//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "LRGBProcessor.h"

#include <algorithm>
#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------
// LRGBProcessor Implementation
// ----------------------------------------------------------------------------

LRGBProcessor::LRGBProcessor( const LRGBConfig& config )
   : m_config( config )
{
}

// ----------------------------------------------------------------------------

double LRGBProcessor::ComputeLuminance( double r, double g, double b ) const
{
   return m_config.luminanceR * r +
          m_config.luminanceG * g +
          m_config.luminanceB * b;
}

// ----------------------------------------------------------------------------

void LRGBProcessor::SeparateLuminance( const Image& rgb,
                                        Image& luminance,
                                        Image& chrominance ) const
{
   if ( rgb.NumberOfNominalChannels() < 3 )
   {
      // Grayscale image
      luminance = rgb;
      chrominance.AllocateData( rgb.Width(), rgb.Height(), 3, ColorSpace::RGB );
      chrominance.White();
      return;
   }

   int width = rgb.Width();
   int height = rgb.Height();

   luminance.AllocateData( width, height, 1, ColorSpace::Gray );
   chrominance.AllocateData( width, height, 3, ColorSpace::RGB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double r = rgb( x, y, 0 );
         double g = rgb( x, y, 1 );
         double b = rgb( x, y, 2 );

         double L = ComputeLuminance( r, g, b );
         luminance( x, y, 0 ) = L;

         // Store color ratios (chrominance)
         if ( L > 1e-10 )
         {
            chrominance( x, y, 0 ) = r / L;
            chrominance( x, y, 1 ) = g / L;
            chrominance( x, y, 2 ) = b / L;
         }
         else
         {
            // Near black - neutral color
            chrominance( x, y, 0 ) = 1.0;
            chrominance( x, y, 1 ) = 1.0;
            chrominance( x, y, 2 ) = 1.0;
         }
      }
   }
}

// ----------------------------------------------------------------------------

void LRGBProcessor::CombineLuminance( const Image& luminance,
                                       const Image& chrominance,
                                       Image& rgb ) const
{
   int width = luminance.Width();
   int height = luminance.Height();

   rgb.AllocateData( width, height, 3, ColorSpace::RGB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double L = luminance( x, y, 0 );

         double chromaR = chrominance( x, y, 0 );
         double chromaG = chrominance( x, y, 1 );
         double chromaB = chrominance( x, y, 2 );

         double r, g, b;
         ApplyColorToLuminance( L, chromaR, chromaG, chromaB, r, g, b );

         rgb( x, y, 0 ) = std::max( 0.0, std::min( 1.0, r ) );
         rgb( x, y, 1 ) = std::max( 0.0, std::min( 1.0, g ) );
         rgb( x, y, 2 ) = std::max( 0.0, std::min( 1.0, b ) );
      }
   }
}

// ----------------------------------------------------------------------------

void LRGBProcessor::ApplyColorToLuminance( double L,
                                            double chromaR, double chromaG, double chromaB,
                                            double& outR, double& outG, double& outB ) const
{
   // Apply color ratios to luminance
   outR = L * chromaR;
   outG = L * chromaG;
   outB = L * chromaB;

   // Apply chroma boost if configured
   if ( m_config.boostChroma && m_config.chromaBoost > 0 )
   {
      double newL = ComputeLuminance( outR, outG, outB );
      if ( newL > 1e-10 )
      {
         // Increase saturation
         double boost = 1.0 + m_config.chromaBoost;
         outR = newL + (outR - newL) * boost;
         outG = newL + (outG - newL) * boost;
         outB = newL + (outB - newL) * boost;
      }
   }
}

// ----------------------------------------------------------------------------

Image LRGBProcessor::ApplyStretchedLuminance( const Image& originalRGB,
                                               const Image& stretchedLuminance ) const
{
   if ( originalRGB.NumberOfNominalChannels() < 3 )
   {
      // Grayscale - just return stretched luminance
      return stretchedLuminance;
   }

   int width = originalRGB.Width();
   int height = originalRGB.Height();

   Image result( width, height, pcl::ColorSpace::RGB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double origR = originalRGB( x, y, 0 );
         double origG = originalRGB( x, y, 1 );
         double origB = originalRGB( x, y, 2 );

         double origL = ComputeLuminance( origR, origG, origB );
         double newL = stretchedLuminance( x, y, 0 );

         double newR, newG, newB;

         if ( origL > 1e-10 )
         {
            // Scale colors proportionally
            double scale = newL / origL;

            if ( m_config.preserveChroma )
            {
               // Preserve color ratios exactly
               ColorSpace::ScaleWithColorPreservation( origR, origG, origB, newL,
                                                        newR, newG, newB );
            }
            else
            {
               // Simple scaling
               newR = origR * scale;
               newG = origG * scale;
               newB = origB * scale;
            }

            // Chroma boost
            if ( m_config.boostChroma && m_config.chromaBoost > 0 )
            {
               double boost = 1.0 + m_config.chromaBoost;
               double L2 = ComputeLuminance( newR, newG, newB );
               newR = L2 + (newR - L2) * boost;
               newG = L2 + (newG - L2) * boost;
               newB = L2 + (newB - L2) * boost;
            }

            // Chroma protection
            if ( m_config.chromaProtection > 0 )
            {
               double sat = ColorSpace::ComputeSaturation( origR, origG, origB );
               double protection = sat * m_config.chromaProtection;

               // Blend toward original colors for saturated pixels
               newR = newR * (1 - protection) + origR * protection;
               newG = newG * (1 - protection) + origG * protection;
               newB = newB * (1 - protection) + origB * protection;
            }
         }
         else
         {
            // Original was black - keep neutral
            newR = newG = newB = newL;
         }

         // Apply luminance/color weights
         if ( m_config.luminanceWeight != 1.0 || m_config.colorWeight != 1.0 )
         {
            double lw = m_config.luminanceWeight;
            double cw = m_config.colorWeight;
            double total = lw + cw;

            if ( total > 0 )
            {
               newR = (newR * lw + origR * cw) / total;
               newG = (newG * lw + origG * cw) / total;
               newB = (newB * lw + origB * cw) / total;
            }
         }

         result( x, y, 0 ) = std::max( 0.0, std::min( 1.0, newR ) );
         result( x, y, 1 ) = std::max( 0.0, std::min( 1.0, newG ) );
         result( x, y, 2 ) = std::max( 0.0, std::min( 1.0, newB ) );
      }
   }

   // Neutralize background if configured
   if ( m_config.neutralBackground )
   {
      NeutralizeBackground( result, m_config.neutralThreshold );
   }

   return result;
}

// ----------------------------------------------------------------------------

Image LRGBProcessor::ExtractLuminance( const Image& rgb ) const
{
   int width = rgb.Width();
   int height = rgb.Height();
   int numChannels = rgb.NumberOfNominalChannels();

   Image luminance( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         if ( numChannels >= 3 )
         {
            luminance( x, y, 0 ) = ComputeLuminance(
               rgb( x, y, 0 ), rgb( x, y, 1 ), rgb( x, y, 2 ) );
         }
         else
         {
            luminance( x, y, 0 ) = rgb( x, y, 0 );
         }
      }
   }

   return luminance;
}

// ----------------------------------------------------------------------------

Image LRGBProcessor::ExtractChrominance( const Image& rgb ) const
{
   int width = rgb.Width();
   int height = rgb.Height();
   int numChannels = rgb.NumberOfNominalChannels();

   Image chrominance( width, height, pcl::ColorSpace::RGB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         if ( numChannels >= 3 )
         {
            double r = rgb( x, y, 0 );
            double g = rgb( x, y, 1 );
            double b = rgb( x, y, 2 );
            double L = ComputeLuminance( r, g, b );

            if ( L > 1e-10 )
            {
               chrominance( x, y, 0 ) = r / L;
               chrominance( x, y, 1 ) = g / L;
               chrominance( x, y, 2 ) = b / L;
            }
            else
            {
               chrominance( x, y, 0 ) = 1.0;
               chrominance( x, y, 1 ) = 1.0;
               chrominance( x, y, 2 ) = 1.0;
            }
         }
         else
         {
            chrominance( x, y, 0 ) = 1.0;
            chrominance( x, y, 1 ) = 1.0;
            chrominance( x, y, 2 ) = 1.0;
         }
      }
   }

   return chrominance;
}

// ----------------------------------------------------------------------------

void LRGBProcessor::ApplySaturationBoost( Image& rgb, double amount ) const
{
   if ( rgb.NumberOfNominalChannels() < 3 || std::abs( amount ) < 0.001 )
      return;

   double boost = 1.0 + amount;

   for ( int y = 0; y < rgb.Height(); ++y )
   {
      for ( int x = 0; x < rgb.Width(); ++x )
      {
         double r = rgb( x, y, 0 );
         double g = rgb( x, y, 1 );
         double b = rgb( x, y, 2 );

         double L = ComputeLuminance( r, g, b );

         r = L + (r - L) * boost;
         g = L + (g - L) * boost;
         b = L + (b - L) * boost;

         rgb( x, y, 0 ) = std::max( 0.0, std::min( 1.0, r ) );
         rgb( x, y, 1 ) = std::max( 0.0, std::min( 1.0, g ) );
         rgb( x, y, 2 ) = std::max( 0.0, std::min( 1.0, b ) );
      }
   }
}

// ----------------------------------------------------------------------------

Image LRGBProcessor::CreateSaturationMask( const Image& rgb ) const
{
   int width = rgb.Width();
   int height = rgb.Height();

   Image mask( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double r = rgb( x, y, 0 );
         double g = rgb( x, y, 1 );
         double b = rgb( x, y, 2 );

         mask( x, y, 0 ) = ColorSpace::ComputeSaturation( r, g, b );
      }
   }

   return mask;
}

// ----------------------------------------------------------------------------

void LRGBProcessor::NeutralizeBackground( Image& rgb, double threshold ) const
{
   if ( rgb.NumberOfNominalChannels() < 3 )
      return;

   for ( int y = 0; y < rgb.Height(); ++y )
   {
      for ( int x = 0; x < rgb.Width(); ++x )
      {
         double r = rgb( x, y, 0 );
         double g = rgb( x, y, 1 );
         double b = rgb( x, y, 2 );

         double L = ComputeLuminance( r, g, b );

         if ( L < threshold )
         {
            // Blend toward neutral
            double blend = 1.0 - (L / threshold);
            rgb( x, y, 0 ) = r * (1 - blend) + L * blend;
            rgb( x, y, 1 ) = g * (1 - blend) + L * blend;
            rgb( x, y, 2 ) = b * (1 - blend) + L * blend;
         }
      }
   }
}

// ----------------------------------------------------------------------------

void LRGBProcessor::RGBtoLab( double r, double g, double b,
                               double& L, double& a, double& labB )
{
   double x, y, z;
   ColorSpace::RGBtoXYZ( r, g, b, x, y, z );
   ColorSpace::XYZtoLab( x, y, z, L, a, labB );
}

// ----------------------------------------------------------------------------

void LRGBProcessor::LabToRGB( double L, double a, double labB,
                               double& r, double& g, double& b )
{
   double x, y, z;
   ColorSpace::LabToXYZ( L, a, labB, x, y, z );
   ColorSpace::XYZtoRGB( x, y, z, r, g, b );
}

// ----------------------------------------------------------------------------

void LRGBProcessor::RGBtoHSL( double r, double g, double b,
                               double& h, double& s, double& l )
{
   double maxC = std::max( { r, g, b } );
   double minC = std::min( { r, g, b } );
   double delta = maxC - minC;

   l = (maxC + minC) / 2.0;

   if ( delta < 1e-10 )
   {
      h = s = 0;
      return;
   }

   s = l > 0.5 ? delta / (2.0 - maxC - minC) : delta / (maxC + minC);

   if ( maxC == r )
   {
      h = std::fmod( (g - b) / delta, 6.0 );
   }
   else if ( maxC == g )
   {
      h = (b - r) / delta + 2.0;
   }
   else
   {
      h = (r - g) / delta + 4.0;
   }

   h *= 60.0;
   if ( h < 0 ) h += 360.0;
}

// ----------------------------------------------------------------------------

void LRGBProcessor::HSLtoRGB( double h, double s, double l,
                               double& r, double& g, double& b )
{
   if ( s < 1e-10 )
   {
      r = g = b = l;
      return;
   }

   auto hueToRGB = []( double p, double q, double t ) {
      if ( t < 0 ) t += 1;
      if ( t > 1 ) t -= 1;
      if ( t < 1.0/6 ) return p + (q - p) * 6 * t;
      if ( t < 1.0/2 ) return q;
      if ( t < 2.0/3 ) return p + (q - p) * (2.0/3 - t) * 6;
      return p;
   };

   double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
   double p = 2 * l - q;
   double hNorm = h / 360.0;

   r = hueToRGB( p, q, hNorm + 1.0/3 );
   g = hueToRGB( p, q, hNorm );
   b = hueToRGB( p, q, hNorm - 1.0/3 );
}

// ----------------------------------------------------------------------------
// ColorSpace Implementation
// ----------------------------------------------------------------------------

namespace ColorSpace
{

void RGBtoXYZ( double r, double g, double b, double& x, double& y, double& z )
{
   // sRGB to XYZ (D65 illuminant)
   // Linearize sRGB
   auto linearize = []( double v ) {
      return v <= 0.04045 ? v / 12.92 : std::pow( (v + 0.055) / 1.055, 2.4 );
   };

   r = linearize( r );
   g = linearize( g );
   b = linearize( b );

   x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
   y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
   z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
}

// ----------------------------------------------------------------------------

void XYZtoRGB( double x, double y, double z, double& r, double& g, double& b )
{
   // XYZ to sRGB
   r =  3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
   g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
   b =  0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

   // Apply sRGB gamma
   auto gamma = []( double v ) {
      return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow( v, 1.0/2.4 ) - 0.055;
   };

   r = gamma( std::max( 0.0, r ) );
   g = gamma( std::max( 0.0, g ) );
   b = gamma( std::max( 0.0, b ) );
}

// ----------------------------------------------------------------------------

void XYZtoLab( double x, double y, double z, double& L, double& a, double& b )
{
   // D65 reference white
   const double Xn = 0.95047;
   const double Yn = 1.0;
   const double Zn = 1.08883;

   auto f = []( double t ) {
      const double delta = 6.0/29.0;
      return t > delta*delta*delta ?
         std::cbrt( t ) : t / (3*delta*delta) + 4.0/29.0;
   };

   double fx = f( x / Xn );
   double fy = f( y / Yn );
   double fz = f( z / Zn );

   L = 116 * fy - 16;
   a = 500 * (fx - fy);
   b = 200 * (fy - fz);
}

// ----------------------------------------------------------------------------

void LabToXYZ( double L, double a, double b, double& x, double& y, double& z )
{
   const double Xn = 0.95047;
   const double Yn = 1.0;
   const double Zn = 1.08883;

   auto fInv = []( double t ) {
      const double delta = 6.0/29.0;
      return t > delta ? t*t*t : 3*delta*delta*(t - 4.0/29.0);
   };

   double fy = (L + 16) / 116;
   double fx = a / 500 + fy;
   double fz = fy - b / 200;

   x = Xn * fInv( fx );
   y = Yn * fInv( fy );
   z = Zn * fInv( fz );
}

// ----------------------------------------------------------------------------

void LabToLCh( double L, double a, double b, double& Lout, double& C, double& h )
{
   Lout = L;
   C = std::sqrt( a*a + b*b );
   h = std::atan2( b, a ) * 180.0 / 3.14159265358979323846;
   if ( h < 0 ) h += 360;
}

// ----------------------------------------------------------------------------

void LChToLab( double L, double C, double h, double& Lout, double& a, double& b )
{
   double hRad = h * 3.14159265358979323846 / 180.0;
   Lout = L;
   a = C * std::cos( hRad );
   b = C * std::sin( hRad );
}

// ----------------------------------------------------------------------------

void RGBtoHSV( double r, double g, double b, double& h, double& s, double& v )
{
   double maxC = std::max( { r, g, b } );
   double minC = std::min( { r, g, b } );
   double delta = maxC - minC;

   v = maxC;
   s = maxC > 0 ? delta / maxC : 0;

   if ( delta < 1e-10 )
   {
      h = 0;
      return;
   }

   if ( maxC == r )
   {
      h = 60 * std::fmod( (g - b) / delta, 6.0 );
   }
   else if ( maxC == g )
   {
      h = 60 * ((b - r) / delta + 2);
   }
   else
   {
      h = 60 * ((r - g) / delta + 4);
   }

   if ( h < 0 ) h += 360;
}

// ----------------------------------------------------------------------------

void HSVtoRGB( double h, double s, double v, double& r, double& g, double& b )
{
   if ( s < 1e-10 )
   {
      r = g = b = v;
      return;
   }

   double hNorm = h / 60.0;
   int i = static_cast<int>( hNorm ) % 6;
   double f = hNorm - i;
   double p = v * (1 - s);
   double q = v * (1 - f * s);
   double t = v * (1 - (1 - f) * s);

   switch ( i )
   {
   case 0: r = v; g = t; b = p; break;
   case 1: r = q; g = v; b = p; break;
   case 2: r = p; g = v; b = t; break;
   case 3: r = p; g = q; b = v; break;
   case 4: r = t; g = p; b = v; break;
   case 5: r = v; g = p; b = q; break;
   }
}

// ----------------------------------------------------------------------------

double ComputeSaturation( double r, double g, double b )
{
   double maxC = std::max( { r, g, b } );
   double minC = std::min( { r, g, b } );

   return maxC > 0 ? (maxC - minC) / maxC : 0;
}

// ----------------------------------------------------------------------------

void ScaleWithColorPreservation( double origR, double origG, double origB,
                                  double newLuminance,
                                  double& newR, double& newG, double& newB )
{
   // Rec. 709 coefficients
   const double Lr = 0.2126;
   const double Lg = 0.7152;
   const double Lb = 0.0722;

   double origL = Lr * origR + Lg * origG + Lb * origB;

   if ( origL < 1e-10 )
   {
      newR = newG = newB = newLuminance;
      return;
   }

   // Simple scaling
   double scale = newLuminance / origL;
   newR = origR * scale;
   newG = origG * scale;
   newB = origB * scale;

   // Check for clipping
   double maxC = std::max( { newR, newG, newB } );
   if ( maxC > 1.0 )
   {
      // Desaturate to bring within gamut
      double desat = (maxC - 1.0) / (maxC - newLuminance);
      desat = std::min( 1.0, std::max( 0.0, desat ) );

      newR = newR * (1 - desat) + newLuminance * desat;
      newG = newG * (1 - desat) + newLuminance * desat;
      newB = newB * (1 - desat) + newLuminance * desat;
   }
}

} // namespace ColorSpace

// ----------------------------------------------------------------------------

} // namespace pcl
