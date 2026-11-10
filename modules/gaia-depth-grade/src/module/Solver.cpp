// The module's native plate solver (see Solver.h). Port of ImageSolver 6.6.0
// (ImageSolverEngine.js: solveImage, findLimitMagnitudeAndCatalog, initialAlignment,
// generateReferenceStarsCSVFile, matchStars, matchWithPredictedPositions, newLinearSolution,
// newSplineSolution, optimizeSolution; AstrometricMetadata.js: ExtractMetadata, SearchRadius;
// AstronomicalCatalogs.js: GaiaXPSDCatalogBase.Load). Copyright (c) 2013-2026 Andres del Pozo,
// Copyright (c) 2019-2026 Juan Conejero (PTeam); PixInsight Class Library License Version 2.0.
#include "Solver.h"

#include "LocalGaia.h"
#include "SolverAlign.h"
#include "SolverBlind.h"
#include "SolverDetect.h"

#include "core/Format.h"
#include "core/SolveMath.h"

#include <pcl/AstrometricMetadata.h>
#include <pcl/ElapsedTime.h>
#include <pcl/ErrorHandler.h>
#include <pcl/File.h>
#include <pcl/GaiaDatabaseFile.h>
#include <pcl/GnomonicProjection.h>
#include <pcl/Homography.h>
#include <pcl/Position.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProjectionFactory.h>
#include <pcl/View.h>
#include <pcl/WCSKeywords.h>
#include <pcl/WorldTransformation.h>

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace pcl
{

const char* const kUnsolvableMessage =
   "Plate solve failed: the image has no usable coordinates (RA/DEC, focal length, pixel size) and no "
   "astrometric index is selected in Gaia preferences. Add the metadata, or select an Astrometric Index File "
   "in the Gaia process preferences.";

static const char* const kAlignmentFailure = "Failed to perform the initial field alignment.";

namespace
{

constexpr double kGaiaDR3Epoch = 2457389.0; // J2016.0, GaiaDR3XPSDCatalog's catalogEpoch
constexpr int    kMaxCatalogStars = 100000; // WCS_MAX_CATALOG_STARS
constexpr int    kMinCatalogStars = 4;      // WCS_MIN_CATALOG_STARS
constexpr double kSearchExpansion = 1.05;   // GaiaXPSDCatalogBase.searchExpansion
constexpr int    kMaxIterations = 100;      // solverCfg.maxIterations
constexpr double kOutlierRadius = 160;      // outlierDetectionRadius
constexpr double kOutlierMinThreshold = 4;  // outlierDetectionMinThreshold
constexpr double kOutlierSigma = 5;         // outlierDetectionSigma

double NormalizeRA( double ra )
{
   while ( ra < 0 )
      ra += 360;
   while ( ra >= 360 )
      ra -= 360;
   return ra;
}

/// A private, unpredictable working directory (mkdtemp: mode 0700), removed with its files.
class PrivateTempDir
{
public:
   PrivateTempDir()
   {
      const char* t = ::getenv( "TMPDIR" );
      std::string base = (t != nullptr && t[0] == '/') ? t : "/tmp";
      std::vector<char> tmpl( base.begin(), base.end() );
      const char* suffix = "/gdg-solver-XXXXXX";
      tmpl.insert( tmpl.end(), suffix, suffix + std::strlen( suffix ) + 1 );
      if ( ::mkdtemp( tmpl.data() ) == nullptr )
         throw Error( "Plate solver: cannot create a private working directory under " + String( base.c_str() ) );
      m_dir = String::UTF8ToUTF16( tmpl.data() );
   }
   ~PrivateTempDir()
   {
      const IsoString d = m_dir.ToUTF8();
      if ( DIR* dir = ::opendir( d.c_str() ) )
      {
         while ( dirent* e = ::readdir( dir ) )
            if ( std::strcmp( e->d_name, "." ) != 0 && std::strcmp( e->d_name, ".." ) != 0 )
               ::unlink( (d + '/' + e->d_name).c_str() );
         ::closedir( dir );
      }
      ::rmdir( d.c_str() );
   }
   String Path( const char* name ) const { return m_dir + '/' + name; }
private:
   String m_dir;
};

struct CatStar
{
   DPoint rd;  // place at the observation time, degrees
   double mag; // G
};

/// The local Gaia DR3 catalogue (GaiaXPSDCatalogBase.Load on pcl::GaiaDatabaseFile): every file,
/// G in [-1.5, magHigh], no source without proper motions, sorted by G, star places for the
/// observation time in the image's reference system.
class GaiaCatalog
{
public:
   GaiaCatalog( const StringList& paths, double observationJD, const IsoString& referenceSystem )
      : m_jd( observationJD ), m_refsys( referenceSystem.Uppercase() )
   {
      for ( const String& p : paths )
         m_files.push_back( std::make_unique<GaiaDatabaseFile>( p ) );
      if ( m_files.empty() )
         throw Error( "Plate solver: no Gaia database file" );
   }

   double MagnitudeHigh() const
   {
      double m = -100;
      for ( const auto& f : m_files )
         m = Max( m, double( f->MagnitudeHigh() ) );
      return m;
   }

   /// Stars of G <= mag within `radius` of the centre (a count search: sourceLimit 0).
   long Count( double ra, double dec, double radius, double mag, long excessLimit ) const
   {
      long n = 0;
      for ( const auto& f : m_files )
      {
         GaiaSearchData data;
         data.centerRA = ra;
         data.centerDec = dec;
         data.radius = radius;
         data.magnitudeLow = -1.5f;
         data.magnitudeHigh = float( mag );
         data.sourceLimit = 0;
         data.excessLimit = uint32( Max( 1L, excessLimit ) );
         data.exclusionFlags = GaiaStarFlag::NoPM;
         f->Search( data );
         n += long( data.excessCount );
      }
      return n;
   }

   void Load( double ra, double dec, double radius, double magHigh )
   {
      m_center = DPoint( ra, dec );
      m_radius = radius;
      struct Raw { double ra, dec; float parx, pmra, pmdec, magG; };
      std::vector<Raw> raw;
      for ( const auto& f : m_files )
      {
         GaiaSearchData data;
         data.centerRA = ra;
         data.centerDec = dec;
         data.radius = Min( 180.0, radius*kSearchExpansion );
         data.magnitudeLow = -1.5f;
         data.magnitudeHigh = float( magHigh );
         data.sourceLimit = 20000000;
         data.exclusionFlags = GaiaStarFlag::NoPM;
         f->Search( data );
         for ( const GaiaStarData& s : data.stars )
            raw.push_back( Raw{ s.ra, s.dec, s.parx, s.pmra, s.pmdec, s.magG } );
      }
      std::stable_sort( raw.begin(), raw.end(), []( const Raw& a, const Raw& b ) { return a.magG < b.magG; } );

      Position P( TimePoint( m_jd ), "UTC" );
      m_stars.clear();
      m_stars.reserve( raw.size() );
      for ( const Raw& s : raw )
      {
         const StarPosition S( s.ra, s.dec, s.pmra, s.pmdec, Max( 0.0, double( s.parx )/1000 ), 0,
                               TimePoint( kGaiaDR3Epoch ) );
         Vector u;
         if ( m_refsys == "GCRS" )
            u = P.Proper( S );
         else if ( m_refsys == "TRUE" || m_refsys == "MEAN" || m_refsys == "APPARENT" || m_refsys == "GAPPT" )
            u = P.Apparent( S );
         else
            u = P.Astrometric( S ); // ICRS, and anything else (placeFunctionForReferenceSystem)
         double lon, lat;
         u.ToSpherical2Pi( lon, lat );
         m_stars.push_back( CatStar{ DPoint( Deg( lon ), Deg( lat ) ), double( s.magG ) } );
      }
      m_loaded = true;
   }

   bool Loaded() const { return m_loaded; }
   const DPoint& Center() const { return m_center; }
   double Radius() const { return m_radius; }
   const std::vector<CatStar>& Stars() const { return m_stars; }

private:
   std::vector<std::unique_ptr<GaiaDatabaseFile>> m_files;
   double                                         m_jd;
   IsoString                                      m_refsys;
   std::vector<CatStar>                           m_stars;
   DPoint                                         m_center = 0.0;
   double                                         m_radius = 0;
   bool                                           m_loaded = false;
};

/// The solution under refinement (ImageSolver's metadata object): a gnomonic projection about
/// (ra0, dec0) and an image <-> projection-plane (G) transformation, projective (linear mode)
/// or a SplineWorldTransformation (distortion mode).
struct Model
{
   double ra0 = 0, dec0 = 0;                                // projection origin
   Homography linearIG, linearGI;                           // ref_I_G_linear and its inverse
   std::shared_ptr<SplineWorldTransformation> spline;       // ref_I_G (W = G), or null
   double ra = 0, dec = 0;                                  // metadata.ra / metadata.dec
   double resolution = 0;                                   // degrees per pixel
   Array<DPoint> cpI, cpG;                                  // control points

   DPoint IG( const DPoint& pI ) const { return spline ? spline->Inverse( pI ) : linearIG( pI ); }
   DPoint GI( const DPoint& pG ) const { return spline ? spline->Direct( pG ) : linearGI( pG ); }

   bool ItoRD( const DPoint& pI, DPoint& rd ) const
   {
      GnomonicProjection P( ra0, dec0 );
      if ( !P.Inverse( rd, IG( pI ) ) )
         return false;
      rd.x = NormalizeRA( rd.x );
      return true;
   }

   bool RDtoI( const DPoint& rd, DPoint& pI ) const
   {
      GnomonicProjection P( ra0, dec0 );
      DPoint g;
      if ( !P.Direct( g, rd ) )
         return false;
      pI = GI( g );
      return true;
   }
};

/// (resx + resy)/2 of a projective matrix normalized to H33 = 1.
double ResolutionOf( const Homography& H )
{
   const Matrix& M = H.TransformationMatrix();
   const double s = (M[2][2] != 0) ? M[2][2] : 1.0;
   const double resx = std::sqrt( (M[0][0]/s)*(M[0][0]/s) + (M[0][1]/s)*(M[0][1]/s) );
   const double resy = std::sqrt( (M[1][0]/s)*(M[1][0]/s) + (M[1][1]/s)*(M[1][1]/s) );
   return (resx + resy)/2;
}

/// Angular distance in degrees (DMath.Distance: the haversine formula).
double Distance( const DPoint& a, const DPoint& b )
{
   const double d1 = Rad( a.y ), d2 = Rad( b.y );
   const double sdd = std::sin( (d2 - d1)/2 ), sda = std::sin( Rad( b.x - a.x )/2 );
   return Deg( 2*std::asin( std::sqrt( sdd*sdd + std::cos( d1 )*std::cos( d2 )*sda*sda ) ) );
}

struct StarMatch
{
   double ra0 = 0, dec0 = 0;                // the projection of coordsG (metadata centre)
   std::vector<std::optional<DPoint>> actual, coordsG;
   int numValid = 0;
   double rms = 0, score = 0;
};

class Engine
{
public:

   Engine( ImageWindow& work, const SolverParams& p, const gdg::Log& log )
      : m_work( work ), m_p( p ), m_log( log )
   {
   }

   SolveReport Run();

private:

   ImageWindow&                 m_work;
   const SolverParams&          m_p;
   const gdg::Log&              m_log;
   int                          m_W = 0, m_H = 0;
   ImageVariant                 m_gray;
   DetectedStars                m_det;
   DetectParams                 m_dp;
   gdg::Seed                    m_seed;
   bool                         m_seedFromImage = false; // centre and scale both from the image
   double                       m_obsStart = 0;
   std::optional<double>        m_obsEnd;
   double                       m_obsJD = 0;
   IsoString                    m_refsys = "ICRS";
   std::unique_ptr<GaiaCatalog> m_catalog;
   double                       m_limitMag = 0;
   String                       m_indexPath;
   bool                         m_indexLooked = false;
   std::optional<BlindResult>   m_blind;
   bool                         m_matchCatalogLoaded = false;
   SolveReport                  m_report;

   void Info( const std::string& s ) const { gdg::log_info( m_log, s ); }
   void Warn( const std::string& s ) const { gdg::log_warning( m_log, s ); }

   const String& IndexPath()
   {
      if ( !m_indexLooked )
      {
         m_indexPath = m_p.indexFile.IsEmpty() ? ConfiguredAstrometricIndex() : m_p.indexFile;
         if ( !m_indexPath.IsEmpty() && !File::Exists( m_indexPath ) )
         {
            Warn( "The astrometric index file does not exist: " + std::string( m_indexPath.ToUTF8().c_str() ) );
            m_indexPath.Clear();
         }
         m_indexLooked = true;
      }
      return m_indexPath;
   }

   void ExtractMetadata();
   void Detect( const DetectParams& dp );
   void SolveBlind();
   void FindLimitMagnitude();
   bool InitialAlignment( Model& out );
   StarMatch MatchStars( const Model& m );
   Model NewSolution( const Model& m, const StarMatch& s ) const;
   double MetadataDelta( const Model& a, const Model& b, const DPoint& pI ) const;
   Model Optimize( Model current, StarMatch stars );
   void Write( const Model& m );
};

void Engine::ExtractMetadata()
{
   View view = m_work.MainView();
   const FITSKeywordArray keywords = m_work.Keywords();
   const WCSKeywords wcs( view.StorableProperties(), keywords );

   if ( !wcs.radesys.IsEmpty() )
      m_refsys = wcs.radesys;

   // Observation time (WCSKeywords.Read): the exposure middle; without a time, J2000.0.
   std::optional<double> exposure;
   for ( const FITSHeaderKeyword& k : keywords )
      if ( k.name == "EXPOSURE" || k.name == "EXPTIME" )
      {
         double v;
         if ( k.GetNumericValue( v ) )
         {
            exposure = v;
            break;
         }
      }
   std::optional<double> start, end;
   if ( wcs.dateobs.IsDefined() )
      start = wcs.dateobs();
   if ( wcs.dateend.IsDefined() )
      end = wcs.dateend();
   const std::optional<double> jd = gdg::observation_jd( start, end, exposure );
   if ( jd )
   {
      m_obsJD = *jd;
      m_obsStart = Min( *start, end.value_or( *start ) );
      if ( end )
         m_obsEnd = Max( *start, *end );
   }
   else
   {
      Warn( "The image provides no observation time; using J2000.0. The astrometric solution may be inaccurate "
            "because of wrongly calculated star proper motions and parallaxes." );
      m_obsJD = m_obsStart = 2451545.0;
   }

   std::optional<double> pixel;
   if ( wcs.xpixsz.IsDefined() && wcs.xpixsz() > 0 )
      pixel = wcs.xpixsz();

   // 1. An astrometric solution the image already has.
   if ( m_p.useExistingSolution && m_work.HasAstrometricSolution() )
   {
      const AstrometricMetadata A( m_work );
      DPoint c;
      if ( A.IsValid() && A.ImageCenterToCelestial( c ) )
      {
         m_seed.ra = c.x;
         m_seed.dec = c.y;
         m_seed.scale_deg = A.Resolution();
         m_seed.pixel_um = pixel;
         m_seedFromImage = true;
         Info( gdg::strformat( "seed: the image's astrometric solution, centre (%.6f, %.6f), %.3f arcsec/px",
                               c.x, c.y, *m_seed.scale_deg*3600 ) );
         return;
      }
   }

   // 2. Linear WCS keywords (ExtractMetadata: a CTYPE RA--/DEC- pair, CRPIX/CRVAL and CD or CDELT).
   if ( wcs.ctype1.StartsWith( "RA--" ) && wcs.ctype2.StartsWith( "DEC-" ) && wcs.crpix1.IsDefined() &&
        wcs.crpix2.IsDefined() && wcs.crval1.IsDefined() && wcs.crval2.IsDefined() )
   {
      WCSKeywords w = wcs;
      LinearTransformation IW;
      std::unique_ptr<ProjectionBase> proj( ProjectionFactory::Create( w ) );
      if ( proj && w.ExtractWorldTransformation( IW, m_H ) )
      {
         DPoint c;
         if ( proj->Inverse( c, IW.Transform( DPoint( m_W/2.0, m_H/2.0 ) ) ) )
         {
            m_seed.ra = NormalizeRA( c.x );
            m_seed.dec = c.y;
            m_seed.scale_deg = (std::sqrt( IW.A00()*IW.A00() + IW.A01()*IW.A01() ) +
                                std::sqrt( IW.A10()*IW.A10() + IW.A11()*IW.A11() ))/2;
            m_seed.pixel_um = pixel;
            m_seedFromImage = true;
            Info( gdg::strformat( "seed: WCS keywords, centre (%.6f, %.6f), %.3f arcsec/px", *m_seed.ra, *m_seed.dec,
                                  *m_seed.scale_deg*3600 ) );
            return;
         }
      }
   }

   // 3. Acquisition metadata: OBJCTRA/OBJCTDEC (or RA/DEC), FOCALLEN, XPIXSZ.
   std::optional<double> ra, dec, focal;
   if ( wcs.objctra.IsDefined() )
      ra = wcs.objctra();
   if ( wcs.objctdec.IsDefined() )
      dec = wcs.objctdec();
   if ( wcs.focallen.IsDefined() )
      focal = wcs.focallen();
   m_seed = gdg::seed_from_metadata( ra, dec, focal, pixel );
   m_seedFromImage = m_seed.ra && m_seed.dec && m_seed.scale_deg && *m_seed.scale_deg > 0;
   if ( m_seedFromImage )
      Info( gdg::strformat( "seed: acquisition metadata, centre (%.6f, %.6f), focal %.2f mm, pixel %.2f um, "
                            "%.3f arcsec/px", *m_seed.ra, *m_seed.dec, *m_seed.focal_mm, *m_seed.pixel_um,
                            *m_seed.scale_deg*3600 ) );
}

void Engine::Detect( const DetectParams& dp )
{
   m_dp = dp;
   m_det = DetectImageStars( m_gray, dp );
   m_report.detected = m_det.rawCount;
   if ( m_det.rawCount < 6 )
      throw Error( String().Format( "Insufficient stars detected: found %d; at least 6 stars are required.",
                                    m_det.rawCount ) );
   if ( m_det.size() < 6 )
      throw Error( String().Format( "Insufficient number of stars: found %d; at least 6 are required.",
                                    int( m_det.size() ) ) );
   Info( gdg::strformat( "%d stars detected, %d valid PSF fits, %d without conflicts; matching tolerance %d px",
                         m_det.rawCount, m_det.psfFits, int( m_det.size() ), m_det.tolerance ) );
}

void Engine::SolveBlind()
{
   const String& index = IndexPath();
   if ( index.IsEmpty() )
      throw Error( kUnsolvableMessage );
   Info( "blind astrometric solution, index " + std::string( index.ToUTF8().c_str() ) );
   BlindResult B;
   if ( !BlindSolve( m_gray, m_det, m_dp, index, B, m_log ) )
      throw Error( String().Format( "The blind solver could not find a valid solution (%d attempt%s, %llu asterisms "
                                    "tried, %llu candidates verified, best log-odds %.1f, %.1f s). The image may not "
                                    "be covered by the astrometric index at a suitable scale, or the detected stars "
                                    "may be unreliable.", B.attempts, (B.attempts > 1) ? "s" : "",
                                    (unsigned long long)B.quadsTried, (unsigned long long)B.candidates,
                                    B.bestLogOdds, B.seconds ) );
   m_seed.ra = B.solution.ra;
   m_seed.dec = B.solution.dec;
   m_seed.scale_deg = B.solution.scale/3600;
   m_refsys = "ICRS"; // blind solutions are referred to the ICRS of the index
   m_blind = B;
   m_report.blindSeconds = B.seconds;
   m_report.blindAttempt = B.attempt;
   Info( gdg::strformat( "blind solution: centre (%.6f, %.6f), %.3f arcsec/px, index level %d, log-odds %.1f "
                         "(%d matched stars), attempt %d", B.solution.ra, B.solution.dec, B.solution.scale,
                         B.solution.level, B.solution.logOdds, B.solution.matches, B.attempt ) );
}

void Engine::FindLimitMagnitude()
{
   const double res = *m_seed.scale_deg;
   const double fov = res*Max( m_W, m_H );
   double m = gdg::auto_limit_magnitude( fov );
   if ( m_p.magLimit > 0 )
      m = m_p.magLimit;
   else
   {
      // The adaptive search for the magnitude limit that gives 1.25 x the detected stars.
      const double radius = res*std::sqrt( 2.0 )*std::sqrt( double( m_W )*m_H )/2;
      const long target = long( std::round( m_det.rawCount*1.25 ) );
      m = gdg::optimal_limit_magnitude( m, m_catalog->MagnitudeHigh(), target, [&]( double mag )
      {
         return m_catalog->Count( *m_seed.ra, *m_seed.dec, radius, mag, long( 1.5*target ) );
      } );
   }
   m_limitMag = m;
   m_report.limitMagnitude = m;
   Info( gdg::strformat( "limit magnitude G %.2f", m ) );
}

bool Engine::InitialAlignment( Model& out )
{
   PrivateTempDir tmp;
   const String refCSV = tmp.Path( "stars-r.csv" ), tgtCSV = tmp.Path( "stars-t.csv" );

   // The target star list: the conflict-free detections, brightest first.
   {
      Array<AlignStar> t;
      for ( size_t i = 0; i < m_det.size(); ++i )
         t << AlignStar{ m_det.pos[i].x, m_det.pos[i].y, m_det.flux[i] };
      File::WriteTextFile( tgtCSV, StarListCSV( m_W, m_H, t ) );
   }

   // The catalogue template: a square of min(W,H) pixels at the seed scale about the seed.
   const double res = *m_seed.scale_deg, ra = *m_seed.ra, dec = *m_seed.dec;
   const int ts = Min( m_W, m_H );
   GnomonicProjection P( ra, dec );
   m_catalog->Load( ra, dec, Max( m_W, m_H )*res, m_limitMag );
   auto G_S = [&]( const DPoint& g ) { return DPoint( ts/2.0 - g.x/res, ts/2.0 - g.y/res ); };
   auto S_G = [&]( const DPoint& s ) { return DPoint( -res*s.x + res*ts/2, -res*s.y + res*ts/2 ); };
   auto writeReference = [&]( bool mirrored )
   {
      Array<AlignStar> r;
      for ( const CatStar& c : m_catalog->Stars() )
      {
         DPoint g;
         if ( !P.Direct( g, c.rd ) )
            continue;
         const DPoint s = G_S( g );
         if ( s.x > 0 && s.x < ts && s.y > 0 && s.y < ts )
            r << AlignStar{ mirrored ? std::fabs( s.x - ts ) : s.x, s.y, std::pow( 2.512, -1.5 - c.mag ) };
      }
      if ( r.Length() < 3 )
         throw Error( "Found too few stars. The limit magnitude could be too low, or the catalog server could be "
                      "malfunctioning." );
      File::WriteTextFile( refCSV, StarListCSV( ts, ts, r ) );
      return int( r.Length() );
   };

   const bool always = double( Max( m_W, m_H ) )/Min( m_W, m_H ) >= 2.0;
   AlignPairs pairs;
   View view = m_work.MainView();
   int numStars = writeReference( false );
   bool ok = RegisterWithTemplate( view, refCSV, tgtCSV, numStars, m_p.distortion, always, pairs );
   if ( !ok )
   {
      // The most frequent reason is a mirrored image: try the mirrored reference.
      Info( "initial alignment failed; trying with mirrored reference coordinates" );
      numStars = writeReference( true );
      ok = RegisterWithTemplate( view, refCSV, tgtCSV, numStars, m_p.distortion, always, pairs );
      if ( ok )
         for ( DPoint& s : pairs.pS )
            s.x = std::fabs( s.x - ts );
   }
   if ( !ok || pairs.pI.Length() < 4 )
      return false;

   // A projection with the origin at the centre of the image.
   Array<DPoint> pG;
   for ( const DPoint& s : pairs.pS )
      pG << S_G( s );
   const Homography SG( pairs.pI, pG );
   DPoint centerRD;
   P.Inverse( centerRD, SG( DPoint( m_W/2.0, m_H/2.0 ) ) );
   GnomonicProjection P2( centerRD.x, centerRD.y );
   Array<DPoint> pG2;
   for ( const DPoint& g : pG )
   {
      DPoint rd, g2;
      P.Inverse( rd, g );
      P2.Direct( g2, rd );
      pG2 << g2;
   }

   out = Model();
   out.ra0 = centerRD.x;
   out.dec0 = centerRD.y;
   out.linearIG = Homography( pairs.pI, pG2 );
   out.linearGI = out.linearIG.Inverse();
   if ( m_p.distortion )
      out.spline = std::make_shared<SplineWorldTransformation>( pG2, pairs.pI );
   out.cpI = pairs.pI;
   out.cpG = pG2;
   DPoint c;
   if ( !out.ItoRD( DPoint( m_W/2.0, m_H/2.0 ), c ) )
      return false;
   out.ra = c.x;
   out.dec = c.y;
   out.resolution = ResolutionOf( out.linearIG );
   Info( gdg::strformat( "initial alignment: %d star pairs, centre (%.6f, %.6f), %.3f arcsec/px",
                         int( pairs.pI.Length() ), out.ra, out.dec, out.resolution*3600 ) );
   return true;
}

StarMatch Engine::MatchStars( const Model& m )
{
   // Reload the catalogue when the centre moved more than 5% of the search radius.
   const DPoint center( m.ra, m.dec );
   double radius = 0;
   {
      DPoint c;
      if ( !m.ItoRD( DPoint( m_W/2.0, m_H/2.0 ), c ) )
         radius = 180;
      else
         for ( const DPoint& corner : { DPoint( 0, 0 ), DPoint( m_W, 0 ), DPoint( 0, m_H ), DPoint( m_W, m_H ) } )
         {
            DPoint q;
            if ( !m.ItoRD( corner, q ) )
            {
               radius = 180;
               break;
            }
            radius = Max( radius, Distance( c, q ) );
         }
   }
   if ( !m_matchCatalogLoaded || Distance( m_catalog->Center(), center ) > m_catalog->Radius()*(kSearchExpansion - 1) )
   {
      m_catalog->Load( m.ra, m.dec, radius, m_limitMag );
      m_matchCatalogLoaded = true;
   }
   const std::vector<CatStar>& cat = m_catalog->Stars();
   if ( int( cat.size() ) < kMinCatalogStars )
      throw Error( "Insufficient stars found in catalog: Gaia DR3 (XPSD)" );
   m_report.catalogStars = int( cat.size() );

   StarMatch r;
   r.ra0 = m.ra;
   r.dec0 = m.dec;
   GnomonicProjection Pm( m.ra, m.dec );
   std::vector<DPoint> predicted;
   for ( int i = 0, n = Min( kMaxCatalogStars, int( cat.size() ) ); i < n; ++i )
   {
      DPoint pI;
      if ( !m.RDtoI( cat[i].rd, pI ) )
         continue;
      if ( pI.x >= 0 && pI.y >= 0 && pI.x <= m_W && pI.y <= m_H )
      {
         DPoint pG;
         if ( Pm.Direct( pG, cat[i].rd ) )
         {
            r.coordsG.push_back( pG );
            predicted.push_back( pI );
         }
      }
   }
   if ( predicted.size() < 4 )
      throw Error( "Unable to define a valid set of reference star positions." );

   // matchWithPredictedPositions: the nearest detection whose PSF box meets the search box.
   const double R = m_det.tolerance;
   r.actual.assign( predicted.size(), std::nullopt );
   for ( size_t i = 0; i < predicted.size(); ++i )
   {
      const DPoint& p = predicted[i];
      const std::vector<int> s = m_det.tree->search( { p.x - R, p.y - R, p.x + R, p.y + R } );
      if ( s.empty() )
         continue;
      int best = s[0];
      double d2 = (m_det.pos[best].x - p.x)*(m_det.pos[best].x - p.x) + (m_det.pos[best].y - p.y)*(m_det.pos[best].y - p.y);
      for ( size_t k = 1; k < s.size(); ++k )
      {
         const gdg::Pt& q = m_det.pos[s[k]];
         const double e = (q.x - p.x)*(q.x - p.x) + (q.y - p.y)*(q.y - p.y);
         if ( e < d2 )
         {
            best = s[k];
            d2 = e;
         }
      }
      r.actual[i] = DPoint( m_det.pos[best].x, m_det.pos[best].y );
   }

   if ( m_p.distortion )
   {
      // Adaptive outlier rejection by local sparsity: detected stars / matched points within
      // 160 px; reject above max(4, median + 5 * 1.1926 Sn).
      std::vector<size_t> idx;
      std::vector<gdg::Box> boxes;
      for ( size_t i = 0; i < r.actual.size(); ++i )
         if ( r.actual[i] )
         {
            idx.push_back( i );
            boxes.push_back( { r.actual[i]->x - 0.5, r.actual[i]->y - 0.5, r.actual[i]->x + 0.5, r.actual[i]->y + 0.5 } );
         }
      const gdg::RectIndex T( boxes );
      std::vector<float> S( idx.size() );
      for ( size_t k = 0; k < idx.size(); ++k )
      {
         const DPoint& a = *r.actual[idx[k]];
         const gdg::Box q{ a.x - kOutlierRadius, a.y - kOutlierRadius, a.x + kOutlierRadius, a.y + kOutlierRadius };
         S[k] = float( m_det.tree->count( q ) )/float( T.count( q ) );
      }
      if ( !S.empty() )
      {
         const double med = Median( S.data(), S.data() + S.size() );
         std::vector<float> Sc( S );
         const double sn = 1.1926*Sn( Sc.data(), Sc.data() + Sc.size() );
         const double d = Max( kOutlierMinThreshold, med + kOutlierSigma*sn );
         for ( size_t k = 0; k < idx.size(); ++k )
            if ( S[k] > d )
               r.actual[idx[k]].reset();
      }
   }

   // Remove control points with identical coordinates (image, then projection plane).
   auto dedupe = [&]( const std::vector<std::optional<DPoint>>& v )
   {
      std::vector<size_t> A;
      for ( size_t i = 0; i < v.size(); ++i )
         if ( r.actual[i] && r.coordsG[i] )
            A.push_back( i );
      std::stable_sort( A.begin(), A.end(), [&]( size_t a, size_t b )
                        { return (v[a]->x != v[b]->x) ? v[a]->x < v[b]->x : v[a]->y < v[b]->y; } );
      for ( size_t k = 1; k < A.size(); ++k )
         if ( v[A[k]]->x == v[A[k - 1]]->x && v[A[k]]->y == v[A[k - 1]]->y )
         {
            r.actual[A[k]].reset();
            r.coordsG[A[k]].reset();
         }
   };
   dedupe( r.actual );
   dedupe( r.coordsG );

   double sum2 = 0;
   for ( size_t i = 0; i < predicted.size(); ++i )
      if ( r.actual[i] && r.coordsG[i] )
      {
         const double ex = predicted[i].x - r.actual[i]->x, ey = predicted[i].y - r.actual[i]->y;
         sum2 += ex*ex + ey*ey;
         ++r.numValid;
      }
   r.rms = (r.numValid > 0) ? std::sqrt( sum2/r.numValid ) : 0;
   r.score = gdg::match_score( r.numValid, r.rms );
   return r;
}

Model Engine::NewSolution( const Model& m, const StarMatch& s ) const
{
   Array<DPoint> pI, pG;
   for ( size_t i = 0; i < s.actual.size(); ++i )
      if ( s.actual[i] && s.coordsG[i] )
      {
         pI << *s.actual[i];
         pG << *s.coordsG[i];
      }
   if ( pI.Length() < 4 )
      throw Error( "Failed to find a valid set of star pair matches." );

   Model n;
   n.ra0 = s.ra0;
   n.dec0 = s.dec0;
   n.linearIG = Homography( pI, pG );
   n.linearGI = n.linearIG.Inverse();
   if ( m_p.distortion )
      n.spline = std::make_shared<SplineWorldTransformation>( pG, pI );
   n.cpI = pI;
   n.cpG = pG;
   DPoint c;
   if ( !n.ItoRD( DPoint( m_W/2.0, m_H/2.0 ), c ) )
      throw Error( "Plate solver: the image centre cannot be projected" );
   // The centre moves by two thirds of the step (newSplineSolution / newLinearSolution).
   n.ra = (std::fabs( m.ra - c.x ) < 1) ? (m.ra + c.x*2)/3 : c.x;
   n.dec = (std::fabs( m.dec - c.y ) < 1) ? (m.dec + c.y*2)/3 : c.y;
   n.resolution = ResolutionOf( n.linearIG );
   return n;
}

double Engine::MetadataDelta( const Model& a, const Model& b, const DPoint& pI ) const
{
   DPoint r2, r1;
   double delta1 = 0;
   if ( b.ItoRD( pI, r2 ) && a.ItoRD( pI, r1 ) )
      delta1 = std::hypot( (r1.x - r2.x)*std::cos( Rad( r2.y ) ), r1.y - r2.y )*3600;
   const double delta2 = std::hypot( (b.ra - a.ra)*std::cos( Rad( b.dec ) ), b.dec - a.dec )*3600;
   return Max( delta1, delta2 );
}

Model Engine::Optimize( Model current, StarMatch stars )
{
   int iteration = 1, noImprovement = 0;
   Model best = current;
   double bestScore = stars.score, bestRMS = stars.rms;
   int bestCount = stars.numValid;
   m_report.matches = stars.numValid;
   m_report.rmsPx = stars.rms;
   for ( ;; )
   {
      Model result = NewSolution( current, stars );
      stars = MatchStars( result );

      const double W = m_W, H = m_H;
      double delta = 0;
      for ( const DPoint& p : { DPoint( 0, 0 ), DPoint( W, 0 ), DPoint( 0, H ), DPoint( W, H ), DPoint( W/2, 0 ),
                                DPoint( W/2, H ), DPoint( 0, H/2 ), DPoint( W, H/2 ) } )
         delta = Max( delta, MetadataDelta( current, result, p ) );
      const double deltaPx = delta/(result.resolution*3600);

      const bool converged = deltaPx < 0.005 && std::fabs( stars.rms - bestRMS ) < 0.01;
      if ( converged || (stars.numValid > bestCount && (stars.rms <= bestRMS || stars.rms - bestRMS < 0.01)) )
         stars.score = Max( stars.score, bestScore + 1 );

      Info( gdg::strformat( "iteration %d: delta %.3f arcsec (%.2f px), %.3f arcsec/px, RMS %.3f px (%d stars), "
                            "score %.3f", iteration, delta, deltaPx, result.resolution*3600, stars.rms,
                            stars.numValid, stars.score ) );
      if ( stars.numValid < 4 )
         throw Error( "Failed to find a valid set of star pair matches." );

      current = result;
      if ( stars.score > bestScore )
      {
         noImprovement = 0;
         best = result;
         bestScore = stars.score;
         bestRMS = stars.rms;
         bestCount = stars.numValid;
         m_report.matches = stars.numValid;
         m_report.rmsPx = stars.rms;
      }
      else
      {
         if ( iteration == 1 )
         {
            best = result;
            m_report.matches = stars.numValid;
            m_report.rmsPx = stars.rms;
         }
         ++noImprovement;
      }

      m_report.iterations = iteration;
      if ( converged || noImprovement > 4 )
      {
         Info( converged ? gdg::strformat( "convergence reached after %d iterations", iteration )
                         : gdg::strformat( "process stalled after %d iterations", iteration ) );
         break;
      }
      if ( iteration > kMaxIterations )
      {
         Warn( "reached the maximum number of iterations without convergence" );
         break;
      }
      ++iteration;
   }
   return best;
}

void Engine::Write( const Model& m )
{
   WorldTransformation* T;
   if ( m.spline )
      T = new SplineWorldTransformation( *m.spline );
   else
   {
      // The linear solution ImageSolver serializes: the 2x2 part of ref_I_G_linear about the
      // image point of the projection origin.
      const Matrix& M = m.linearIG.TransformationMatrix();
      const double s = (M[2][2] != 0) ? M[2][2] : 1.0;
      const DPoint I0 = m.linearGI( DPoint( 0, 0 ) );
      const double a00 = M[0][0]/s, a01 = M[0][1]/s, a10 = M[1][0]/s, a11 = M[1][1]/s;
      T = new LinearWorldTransformation( LinearTransformation( a00, a01, -(a00*I0.x + a01*I0.y),
                                                               a10, a11, -(a10*I0.x + a11*I0.y) ) );
   }
   AstrometricMetadata A( new GnomonicProjection( m.ra0, m.dec0 ), T, m_W, m_H );
   A.SetReferenceSystem( m_refsys == "GCRS" ? IsoString( "GCRS" ) : IsoString( "ICRS" ) );
   A.SetObservationStartTime( TimePoint( m_obsStart ) );
   if ( m_obsEnd )
      A.SetObservationEndTime( TimePoint( *m_obsEnd ) );
   if ( m_seed.pixel_um )
      A.SetPixelSize( *m_seed.pixel_um );
   A.Write( m_work );
   if ( !m_work.RegenerateAstrometricSolution( false/*allowGUIMessages*/ ) )
      throw Error( "Plate solver: PixInsight could not regenerate the written astrometric solution" );
}

SolveReport Engine::Run()
{
   ElapsedTime T;
   try
   {
      View view = m_work.MainView();
      const ImageVariant image = view.Image();
      m_W = image.Width();
      m_H = image.Height();
      ExtractMetadata();

      m_gray = WorkingGrayscale( image );
      Detect( DetectParams() );

      // Blind solution: always, never, or (automatic) when the image provides no trusted centre
      // and scale -- and an index exists.
      bool blind = false;
      if ( m_p.blind == BlindMode::Always )
         blind = true;
      else if ( m_p.blind == BlindMode::Automatic )
      {
         const bool trusted = gdg::seed_is_valid( m_seed ) && m_seedFromImage;
         if ( !trusted )
         {
            if ( !IndexPath().IsEmpty() )
               blind = true;
            else if ( gdg::seed_is_valid( m_seed ) )
               Warn( "No astrometric index file is available for blind solving - using the available initial "
                     "center coordinates and image scale." );
         }
      }
      if ( blind )
         SolveBlind();
      if ( !gdg::seed_is_valid( m_seed ) )
         throw Error( kUnsolvableMessage );

      const StringList files = m_p.gaiaDatabaseFiles.IsEmpty() ? DiscoverGaiaDatabases() : m_p.gaiaDatabaseFiles;
      m_catalog = std::make_unique<GaiaCatalog>( files, m_obsJD, m_refsys );
      FindLimitMagnitude();

      // Initial alignment; in automatic blind mode a failure with the image's seed is retried
      // with a blind solution, and a blind solution that needed permissive detection parameters
      // re-detects the stars with them (alignWithBlindStars).
      Model model;
      bool aligned = InitialAlignment( model );
      if ( !aligned && m_p.blind == BlindMode::Automatic && !m_blind && !IndexPath().IsEmpty() )
      {
         Warn( "The initial alignment failed with the initial center coordinates and image scale - solving blindly." );
         SolveBlind();
         FindLimitMagnitude();
         aligned = InitialAlignment( model );
      }
      if ( !aligned && m_blind && m_blind->attempt >= 2 && !(m_dp.Permissive() == m_dp) )
      {
         Warn( "The initial alignment failed with the stars detected with the current parameters - detecting stars "
               "again with the parameters of the blind solver." );
         Detect( m_dp.Permissive() );
         FindLimitMagnitude();
         aligned = InitialAlignment( model );
      }
      if ( !aligned )
         throw Error( kAlignmentFailure );

      m_matchCatalogLoaded = false;
      StarMatch stars = MatchStars( model );
      Info( gdg::strformat( "initial matching: RMS %.3f px (%d stars), score %.3f", stars.rms, stars.numValid,
                            stars.score ) );
      const Model best = Optimize( model, stars );
      Write( best );

      m_report.ok = true;
      m_report.method = m_blind ? "blind+seeded" : "seeded";
   }
   catch ( const Exception& e )
   {
      m_report.ok = false;
      m_report.error = e.Message();
   }
   catch ( const std::exception& e )
   {
      m_report.ok = false;
      m_report.error = String( e.what() );
   }
   m_report.seconds = T();
   return m_report;
}

} // namespace

String ConfiguredAstrometricIndex()
{
   // ImageSolver's astrometricIndexFilePath(): Gaia get-info. No Gaia process: no index.
   bool installed = false;
   for ( const Process& P : Process::AllProcesses() )
      if ( P.Id() == "Gaia" )
      {
         installed = true;
         break;
      }
   if ( !installed )
      return String();

   Process G_( IsoString( "Gaia" ) );
   ProcessInstance G( G_ );
   // Row 0: the core refuses string values set with the default "no row" index (as in
   // SolverAlign); non-table parameters ignore the row.
   if ( !G.SetParameterValue( String( "get-info" ), IsoString( "command" ), 0 ) ||
        !G.SetParameterValue( int32( 0 ), IsoString( "dataRelease" ), 0 ) || // DataRelease_BestAvailable
        !G.SetParameterValue( int32( 0 ), IsoString( "verbosity" ), 0 ) )
      throw Error( "Plate solver: cannot query the Gaia process for its astrometric index file" );
   if ( !G.ExecuteGlobal() )
      throw Error( "Plate solver: the Gaia process get-info command failed" );
   return G.ParameterValue( IsoString( "astrometricIndexFilePath" ), 0 ).ToString().Trimmed();
}

SolveReport SolveImage( ImageWindow& work, const SolverParams& p, const gdg::Log& log )
{
   Engine E( work, p, log );
   return E.Run();
}

} // namespace pcl
