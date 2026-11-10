// EZTools -- EZHazeKill process instance.
// Copyright and licence: see src/EZToolsLicense.h.

#include "EZHazeKillInstance.h"
#include "EZHazeKillParameters.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/View.h>

#include <vector>

namespace pcl
{

using namespace eztools;

EZHazeKillInstance::EZHazeKillInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_amount( TheEZHKAmountParameter->DefaultValue() )
   , p_neutralize( TheEZHKNeutralizeParameter->DefaultValue() )
   , p_targetLevel( TheEZHKTargetLevelParameter->DefaultValue() )
{
}

EZHazeKillInstance::EZHazeKillInstance( const EZHazeKillInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void EZHazeKillInstance::Assign( const ProcessImplementation& p )
{
   const EZHazeKillInstance* x = dynamic_cast<const EZHazeKillInstance*>( &p );
   if ( x != nullptr )
   {
      p_amount = x->p_amount;
      p_neutralize = x->p_neutralize;
      p_targetLevel = x->p_targetLevel;
   }
}

hazekill::Settings EZHazeKillInstance::Settings() const
{
   hazekill::Settings s;
   s.amount = p_amount;
   s.neutralize = p_neutralize;
   s.target = p_targetLevel;
   return s;
}

bool EZHazeKillInstance::CanExecuteOn( const View& view, String& whyNot ) const
{
   if ( view.IsNull() )
   {
      whyNot = "EZHazeKill needs an image.";
      return false;
   }
   ImageVariant image = view.Image();
   if ( !image || image.Width() <= 0 || image.Height() <= 0 )
   {
      whyNot = "EZHazeKill needs a view with an image.";
      return false;
   }
   if ( image.IsComplexSample() )
   {
      whyNot = "EZHazeKill cannot be executed on complex images.";
      return false;
   }
   return true;
}

namespace
{

template <class P>
hazekill::SkyEstimate MeasureT( const GenericImage<P>& img )
{
   const int nc = img.NumberOfNominalChannels();
   const int w = img.Width(), h = img.Height();
   // Every sample type has its engine overload; integers are converted tile by tile (x * 1/max, as
   // P::FromSample), so no whole-image double copy is made.
   std::vector<const typename P::sample*> ch;
   for ( int c = 0; c < nc; ++c )
      ch.push_back( img.PixelData( c ) );
   return hazekill::MeasureSky( ch, w, h );
}

template <class P>
void ApplyT( GenericImage<P>& img, const hazekill::Model& m )
{
   const int nc = Min( img.NumberOfNominalChannels(), int( m.offset.size() ) );
   const size_t n = img.NumberOfPixels();
   if constexpr ( std::is_same_v<typename P::sample, float> || std::is_same_v<typename P::sample, double> )
   {
      std::vector<typename P::sample*> ch;
      for ( int c = 0; c < nc; ++c )
         ch.push_back( img.PixelData( c ) );
      hazekill::Apply( ch, n, m );
   }
   else
   {
      for ( int c = 0; c < nc; ++c )
      {
         typename P::sample* s = img.PixelData( c );
         const double off = m.offset[c];
         for ( size_t i = 0; i < n; ++i )
         {
            double x;
            P::FromSample( x, s[i] );
            s[i] = P::ToSample( hazekill::ApplySample( x, off, m.eps ) );
         }
      }
   }
}

} // namespace

hazekill::SkyEstimate EZHazeKillInstance::MeasureImage( const ImageVariant& v )
{
   hazekill::SkyEstimate e;
   if ( !v || v.IsComplexSample() )
   {
      e.error = "EZHazeKill needs a real (not complex) image";
      return e;
   }
   if ( v.IsFloatSample() )
      switch ( v.BitsPerSample() )
      {
      case 32: return MeasureT( static_cast<const Image&>( *v ) );
      case 64: return MeasureT( static_cast<const DImage&>( *v ) );
      }
   else
      switch ( v.BitsPerSample() )
      {
      case  8: return MeasureT( static_cast<const UInt8Image&>( *v ) );
      case 16: return MeasureT( static_cast<const UInt16Image&>( *v ) );
      case 32: return MeasureT( static_cast<const UInt32Image&>( *v ) );
      }
   e.error = "EZHazeKill: unsupported sample type";
   return e;
}

bool EZHazeKillInstance::ComputeModel( const ImageVariant& v, const hazekill::Settings& s, hazekill::Model& m,
                                       String& error )
{
   hazekill::SkyEstimate e = MeasureImage( v );
   if ( !e.ok )
   {
      error = String( e.error.c_str() );
      return false;
   }
   m = hazekill::BuildModel( e, s );
   return true;
}

void EZHazeKillInstance::ApplyModel( ImageVariant& v, const hazekill::Model& m )
{
   if ( v.IsComplexSample() )
      return;
   if ( v.IsFloatSample() )
      switch ( v.BitsPerSample() )
      {
      case 32: ApplyT( static_cast<Image&>( *v ), m ); break;
      case 64: ApplyT( static_cast<DImage&>( *v ), m ); break;
      }
   else
      switch ( v.BitsPerSample() )
      {
      case  8: ApplyT( static_cast<UInt8Image&>( *v ), m ); break;
      case 16: ApplyT( static_cast<UInt16Image&>( *v ), m ); break;
      case 32: ApplyT( static_cast<UInt32Image&>( *v ), m ); break;
      }
}

void EZHazeKillInstance::WriteReport( const hazekill::Model& m )
{
   Console console;
   String text( hazekill::Report( m ).c_str() );
   StringList lines;
   text.Break( lines, '\n' );
   for ( const String& line : lines )
      if ( line.StartsWith( "Note:" ) )
         console.NoteLn( line );
      else
         console.WriteLn( line );
}

bool EZHazeKillInstance::ExecuteOn( View& view )
{
   AutoViewLock lock( view );

   ImageVariant image = view.Image();
   if ( image.IsComplexSample() )
      return false;

   Console console;

   hazekill::Model m;
   String error;
   if ( !ComputeModel( image, Settings(), m, error ) )
      throw Error( "EZHazeKill: " + error );   // the image is left unchanged

   WriteReport( m );
   ApplyModel( image, m );
   console.NoteLn( "Haze removed. (Ctrl+Z to undo)" );
   return true;
}

void* EZHazeKillInstance::LockParameter( const MetaParameter* p, size_type /*tableRow*/ )
{
   if ( p == TheEZHKAmountParameter )
      return &p_amount;
   if ( p == TheEZHKNeutralizeParameter )
      return &p_neutralize;
   if ( p == TheEZHKTargetLevelParameter )
      return &p_targetLevel;
   return nullptr;
}

} // namespace pcl
