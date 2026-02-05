//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "IStretchAlgorithm.h"

#include <pcl/StandardStatus.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// IStretchAlgorithm default implementation
// ----------------------------------------------------------------------------

void IStretchAlgorithm::ApplyToImage( Image& image, const Image* mask ) const
{
   int width = image.Width();
   int height = image.Height();
   int channels = image.NumberOfChannels();

   for ( int c = 0; c < channels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double original = image( x, y, c );
            double stretched = Apply( original );

            if ( mask != nullptr )
            {
               // Blend based on mask value
               double maskValue = (*mask)( x, y, 0 );
               image( x, y, c ) = original * (1.0 - maskValue) + stretched * maskValue;
            }
            else
            {
               image( x, y, c ) = stretched;
            }
         }
      }
   }
}

// ----------------------------------------------------------------------------
// StretchAlgorithmBase implementation
// ----------------------------------------------------------------------------

void StretchAlgorithmBase::ApplyToImage( Image& image, const Image* mask ) const
{
   int width = image.Width();
   int height = image.Height();
   int channels = image.NumberOfChannels();

   for ( int c = 0; c < channels; ++c )
   {
      Image::sample_iterator i( image, c );

      if ( mask != nullptr )
      {
         Image::const_sample_iterator m( *mask, 0 );
         for ( ; i; ++i, ++m )
         {
            double original = *i;
            double stretched = Apply( original );
            double maskValue = *m;
            *i = original * (1.0 - maskValue) + stretched * maskValue;
         }
      }
      else
      {
         for ( ; i; ++i )
         {
            *i = Apply( *i );
         }
      }
   }
}

// ----------------------------------------------------------------------------

bool StretchAlgorithmBase::SetParameter( const IsoString& id, double value )
{
   AlgorithmParameter* param = FindParameter( id );
   if ( param != nullptr )
   {
      param->value = Clamp( value, param->minValue, param->maxValue );
      return true;
   }
   return false;
}

// ----------------------------------------------------------------------------

double StretchAlgorithmBase::GetParameter( const IsoString& id ) const
{
   const AlgorithmParameter* param = FindParameter( id );
   return ( param != nullptr ) ? param->value : 0.0;
}

// ----------------------------------------------------------------------------

void StretchAlgorithmBase::ResetParameters()
{
   for ( AlgorithmParameter& param : m_parameters )
   {
      param.value = param.defaultValue;
   }
}

// ----------------------------------------------------------------------------

AlgorithmParameter* StretchAlgorithmBase::FindParameter( const IsoString& id )
{
   for ( AlgorithmParameter& param : m_parameters )
   {
      if ( param.id == id )
         return &param;
   }
   return nullptr;
}

// ----------------------------------------------------------------------------

const AlgorithmParameter* StretchAlgorithmBase::FindParameter( const IsoString& id ) const
{
   for ( const AlgorithmParameter& param : m_parameters )
   {
      if ( param.id == id )
         return &param;
   }
   return nullptr;
}

// ----------------------------------------------------------------------------

} // namespace pcl
