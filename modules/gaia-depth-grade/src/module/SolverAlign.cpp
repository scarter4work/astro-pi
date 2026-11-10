// Initial alignment through the stock StarAlignment process. Port of ImageSolver 6.6.0's
// registerWithTemplate() (ImageSolverEngine.js l.376-443; Copyright (c) 2013-2026 Andres del Pozo,
// Copyright (c) 2019-2026 Juan Conejero (PTeam); PCL License 2.0).
#include "SolverAlign.h"

#include <pcl/ErrorHandler.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pcl
{

IsoString StarListCSV( int width, int height, const Array<AlignStar>& stars )
{
   IsoString s;
   char b[ 96 ];
   std::snprintf( b, sizeof( b ), "%d,%d\n", width, height );
   s += b;
   for ( const AlignStar& a : stars )
   {
      std::snprintf( b, sizeof( b ), "%.4f,%.4f,%.3e\n", a.x, a.y, a.flux );
      s += b;
   }
   return s;
}

namespace
{

void Set( ProcessInstance& P, const IsoString& id, const Variant& v )
{
   // Row 0, not the "no row" default: the core rejects string values set with the default row
   // index (measured: referenceImage refused, 2026-11-10); non-table parameters ignore it.
   if ( !P.SetParameterValue( v, id, 0 ) )
      throw Error( "StarAlignment: cannot set parameter '" + String( id ) + "'" );
}

/// One coordinate column of outputData row 0: a vector Variant, or a Block of packed float64
/// (or float32) values, `count` of them. Anything else is an error, never a silent guess.
DVector StarCoordinates( const ProcessInstance& SA, const ProcessParameter& table, const char* column, int count )
{
   const Variant v = SA.ParameterValue( ProcessParameter( table, IsoString( column ) ), 0 );
   if ( v.Type() == VariantType::ByteArray )
   {
      const ByteArray b = v.ToByteArray();
      DVector out( count );
      if ( b.Length() == size_type( count )*sizeof( double ) )
         ::memcpy( out.Begin(), b.Begin(), b.Length() );
      else if ( b.Length() == size_type( count )*sizeof( float ) )
      {
         const float* f = reinterpret_cast<const float*>( b.Begin() );
         for ( int i = 0; i < count; ++i )
            out[i] = f[i];
      }
      else
         throw Error( String().Format( "StarAlignment outputData.%s: %u bytes for %d stars", column,
                                       unsigned( b.Length() ), count ) );
      return out;
   }
   if ( v.CanConvertToDVector() )
   {
      DVector out = v.ToDVector();
      if ( out.Length() < count )
         throw Error( String().Format( "StarAlignment outputData.%s: %d values for %d stars", column,
                                       out.Length(), count ) );
      return out;
   }
   throw Error( String().Format( "StarAlignment outputData.%s: unexpected value type %d", column, int( v.Type() ) ) );
}

} // namespace

bool RegisterWithTemplate( View& view, const String& referenceCSV, const String& targetCSV, int numStars,
                           bool distortionCorrection, bool intersectionAlways, AlignPairs& pairs )
{
   Process SA_( IsoString( "StarAlignment" ) );
   ProcessInstance SA( SA_ );

   // ImageSolver's solverCfg defaults: structureLayers 5, minStructureSize 0, hot pixel filter 1,
   // noise reduction 0, sensitivity 0.5, peak response 0.5, bright threshold 3, distortion 0.6.
   Set( SA, "referenceImage", referenceCSV );
   Set( SA, "referenceIsFile", true );
   Set( SA, "targetCSVFilePath", targetCSV );
   Set( SA, "distortionCorrection", distortionCorrection );
   Set( SA, "mode", int32( StarAlignmentEnum::OutputMatrix ) );
   Set( SA, "writeKeywords", false );
   Set( SA, "structureLayers", int32( 5 ) );
   Set( SA, "minStructureSize", int32( 0 ) );
   Set( SA, "hotPixelFilterRadius", int32( 1 ) );
   Set( SA, "noiseReductionFilterRadius", int32( 0 ) );
   Set( SA, "sensitivity", 0.5 );
   Set( SA, "peakResponse", 0.5 );
   Set( SA, "brightThreshold", 3.0 );
   Set( SA, "maxStarDistortion", 0.6 );
   Set( SA, "allowClusteredSources", true ); // match as many stars as possible at this stage
   Set( SA, "ransacTolerance", 2.0 );
   Set( SA, "ransacMaxIterations", int32( 5000 ) );
   Set( SA, "ransacMaximizeInliers", 1.0 );
   Set( SA, "ransacMaximizeOverlapping", 0.0 );
   Set( SA, "ransacMaximizeRegularity", 0.0 );
   Set( SA, "ransacMinimizeError", 0.0 );
   Set( SA, "useTriangles", false ); // polygonal descriptors, for robustness
   Set( SA, "polygonSides", int32( 7 ) );
   // Exhaustive star matching for small star sets; 1.25 x the reference stars, up to 2000,
   // otherwise.
   Set( SA, "maxStars", int32( (numStars <= 2000) ? 0 : std::min( 2000, int( std::trunc( 1.25*numStars ) ) ) ) );
   Set( SA, "descriptorsPerStar", int32( 100 ) );
   Set( SA, "restrictToPreviews", false );
   Set( SA, "intersection", int32( intersectionAlways ? StarAlignmentEnum::Always : StarAlignmentEnum::NoIntersection ) );

   if ( !SA.ExecuteOn( view, false/*swapFile*/ ) )
      return false;

   // outputData: one row per target; the column ids are StarAlignment's own (probe
   // `introspect`): totalPairMatches (UInt32), referenceStarX/Y and targetStarX/Y (Block
   // columns: packed coordinate arrays, decoded by StarCoordinates()).
   const ProcessParameter table( SA_, IsoString( "outputData" ) );
   if ( SA.TableRowCount( table ) < 1 )
      throw Error( "StarAlignment produced no output data" );
   const int total = SA.ParameterValue( ProcessParameter( table, IsoString( "totalPairMatches" ) ), 0 ).ToInt();
   if ( total <= 0 )
      return false;
   const DVector rx = StarCoordinates( SA, table, "referenceStarX", total );
   const DVector ry = StarCoordinates( SA, table, "referenceStarY", total );
   const DVector tx = StarCoordinates( SA, table, "targetStarX", total );
   const DVector ty = StarCoordinates( SA, table, "targetStarY", total );
   const int n = std::min( total, 4000 );
   pairs.pS.Clear();
   pairs.pI.Clear();
   for ( int i = 0; i < n; ++i )
   {
      pairs.pS << DPoint( rx[i], ry[i] );
      pairs.pI << DPoint( tx[i] + 0.5, ty[i] + 0.5 );
   }
   return n > 0;
}

} // namespace pcl
