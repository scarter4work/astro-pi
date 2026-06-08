//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStretchInstance.h"
#include "NukeXStretchParameters.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Console.h>
#include <pcl/StandardStatus.h>
#include <pcl/View.h>

#include "engine/StretchLibrary.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStretchInstance::NukeXStretchInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_stretchAlgorithm( NXSStretchAlgorithm::Default )
   , p_autoBlackPoint( TheNXSAutoBlackPointParameter->DefaultValue() )
   , p_contrast( TheNXSContrastParameter->DefaultValue() )
   , p_saturation( TheNXSSaturationParameter->DefaultValue() )
   , p_blackPoint( TheNXSBlackPointParameter->DefaultValue() )
   , p_whitePoint( TheNXSWhitePointParameter->DefaultValue() )
   , p_gamma( TheNXSGammaParameter->DefaultValue() )
   , p_stretchStrength( TheNXSStretchStrengthParameter->DefaultValue() )
{
}

// ----------------------------------------------------------------------------

NukeXStretchInstance::NukeXStretchInstance( const NukeXStretchInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

// ----------------------------------------------------------------------------

void NukeXStretchInstance::Assign( const ProcessImplementation& p )
{
   const NukeXStretchInstance* x = dynamic_cast<const NukeXStretchInstance*>( &p );
   if ( x != nullptr )
   {
      p_stretchAlgorithm = x->p_stretchAlgorithm;
      p_autoBlackPoint   = x->p_autoBlackPoint;
      p_contrast         = x->p_contrast;
      p_saturation       = x->p_saturation;
      p_blackPoint       = x->p_blackPoint;
      p_whitePoint       = x->p_whitePoint;
      p_gamma            = x->p_gamma;
      p_stretchStrength  = x->p_stretchStrength;
   }
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::Validate( String& info )
{
   // Basic parameter validation
   if ( p_contrast < 0 || p_contrast > 1 )
   {
      info = "Contrast must be between 0 and 1.";
      return false;
   }
   if ( p_blackPoint >= p_whitePoint )
   {
      info = "Black point must be less than white point.";
      return false;
   }
   return true;
}

// ----------------------------------------------------------------------------

UndoFlags NukeXStretchInstance::UndoMode( const View& ) const
{
   return UndoFlag::PixelData;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::CanExecuteOn( const View& view, String& whyNot ) const
{
   if ( view.Image().IsComplexSample() )
   {
      whyNot = "NukeXStretch cannot be executed on complex images.";
      return false;
   }
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::ExecuteOn( View& view )
{
   try
   {
      AutoViewLock lock( view );

      Console console;
      console.WriteLn( "<end><cbr>NukeXStretch: Processing...\n" );

      // Get the image from the view
      ImageVariant imageVar = view.Image();

      // Ensure we're working with 32-bit float (standard for astro processing)
      if ( !imageVar.IsFloatSample() || imageVar.BitsPerSample() != 32 )
      {
         console.WarningLn( "Warning: Converting image to 32-bit float.\n" );
         ImageVariant result;
         result.CreateFloatImage( 32 );
         result.CopyImage( imageVar );
         imageVar.Free();
         view.Image().CopyImage( result );
         imageVar = view.Image();
      }

      // Get reference to the underlying 32-bit float Image
      Image& image = static_cast<Image&>( *imageVar );

      // Map algorithm enum to AlgorithmType
      AlgorithmType algoType;
      switch ( p_stretchAlgorithm )
      {
      case NXSStretchAlgorithm::MTF:         algoType = AlgorithmType::MTF;         break;
      case NXSStretchAlgorithm::Histogram:   algoType = AlgorithmType::Histogram;   break;
      case NXSStretchAlgorithm::GHS:         algoType = AlgorithmType::GHS;         break;
      case NXSStretchAlgorithm::ArcSinh:     algoType = AlgorithmType::ArcSinh;     break;
      case NXSStretchAlgorithm::Log:         algoType = AlgorithmType::Log;         break;
      case NXSStretchAlgorithm::Lumpton:     algoType = AlgorithmType::Lumpton;     break;
      case NXSStretchAlgorithm::RNC:         algoType = AlgorithmType::RNC;         break;
      case NXSStretchAlgorithm::Photometric: algoType = AlgorithmType::Photometric; break;
      case NXSStretchAlgorithm::OTS:         algoType = AlgorithmType::OTS;         break;
      case NXSStretchAlgorithm::SAS:         algoType = AlgorithmType::SAS;         break;
      case NXSStretchAlgorithm::Veralux:     algoType = AlgorithmType::Veralux;     break;
      default:                               algoType = AlgorithmType::Auto;        break;
      }

      // Create the algorithm
      auto algorithm = StretchLibrary::Instance().Create( algoType );
      if ( algorithm == nullptr )
      {
         console.CriticalLn( "Error: Stretch algorithm not available.\n" );
         return false;
      }

      console.WriteLn( "Algorithm: " + algorithm->Name() + "\n" );

      // Per-channel auto-configure and stretch to preserve color
      int numCh = image.NumberOfChannels();
      bool isColor = ( numCh >= 3 );

      if ( isColor )
      {
         console.WriteLn( "Per-channel stretch:\n" );
         for ( int ch = 0; ch < numCh; ++ch )
         {
            const char* label = ch == 0 ? "R" : ch == 1 ? "G" : ch == 2 ? "B" : "?";

            image.SelectChannel( ch );
            double median = image.Median();
            double mad = image.MAD( median );
            console.WriteLn( String().Format( "  %s: median=%.6f, MAD=%.6f\n", label, median, mad ) );

            auto chAlgo = algorithm->Clone();
            chAlgo->AutoConfigure( median, mad );

            Image::sample_iterator it( image, ch );
            for ( ; it; ++it )
               *it = chAlgo->Apply( *it );
         }
         image.ResetChannelRange();
      }
      else
      {
         double median = image.Median();
         double mad = image.MAD( median );
         console.WriteLn( String().Format( "Image median: %.6f, MAD: %.6f\n", median, mad ) );

         algorithm->AutoConfigure( median, mad );
         algorithm->ApplyToImage( image );
      }

      console.WriteLn( "NukeXStretch: Done.\n" );

      return true;
   }
   catch ( const std::bad_alloc& e )
   {
      Console().CriticalLn( "NukeXStretch: Out of memory \xe2\x80\x94 " + String( e.what() )
         + "\nTry closing other image windows to free memory.\n" );
      return false;
   }
   catch ( const ProcessAborted& )
   {
      throw;
   }
   catch ( const Error& e )
   {
      Console().CriticalLn( "NukeXStretch: " + e.Message() + "\n" );
      return false;
   }
   catch ( const std::exception& e )
   {
      Console().CriticalLn( "NukeXStretch: " + String( e.what() ) + "\n" );
      return false;
   }
   catch ( ... )
   {
      Console().CriticalLn( "NukeXStretch: Unknown error during stretch.\n" );
      return false;
   }
}

// ----------------------------------------------------------------------------

void* NukeXStretchInstance::LockParameter( const MetaParameter* p, size_type /*tableRow*/ )
{
   if ( p == TheNXSStretchAlgorithmParameter )  return &p_stretchAlgorithm;
   if ( p == TheNXSAutoBlackPointParameter )    return &p_autoBlackPoint;
   if ( p == TheNXSContrastParameter )          return &p_contrast;
   if ( p == TheNXSSaturationParameter )        return &p_saturation;
   if ( p == TheNXSBlackPointParameter )        return &p_blackPoint;
   if ( p == TheNXSWhitePointParameter )        return &p_whitePoint;
   if ( p == TheNXSGammaParameter )             return &p_gamma;
   if ( p == TheNXSStretchStrengthParameter )   return &p_stretchStrength;

   return nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::AllocateParameter( size_type /*sizeOrLength*/, const MetaParameter* /*p*/, size_type /*tableRow*/ )
{
   return true;
}

// ----------------------------------------------------------------------------

size_type NukeXStretchInstance::ParameterLength( const MetaParameter* /*p*/, size_type /*tableRow*/ ) const
{
   return 0;
}

// ----------------------------------------------------------------------------

} // namespace pcl
