//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX3 - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// FrameLoader - Reads FITS/XISF frames into a SubCube

#include "FrameLoader.h"

#include <pcl/ErrorHandler.h>

#include <omp.h>
#include <string>
#include <cstring>
#include <algorithm>

namespace nukex {

// ----------------------------------------------------------------------------

SubCube FrameLoader::Load( const std::vector<FramePath>& frames )
{
    // 1. Filter to enabled frames only
    std::vector<const FramePath*> enabled;
    for ( const auto& f : frames )
    {
        if ( f.enabled )
            enabled.push_back( &f );
    }

    if ( enabled.empty() )
        throw pcl::Error( "FrameLoader: no enabled frames to load" );

    pcl::Console console;
    console.WriteLn( pcl::String().Format(
        "<end><cbr>FrameLoader: loading %d of %d frames",
        int( enabled.size() ), int( frames.size() ) ) );

    // 2. Open the first frame to get reference dimensions
    pcl::String ext0 = pcl::File::ExtractExtension( enabled[0]->path ).Lowercase();
    pcl::FileFormat format0( ext0, true/*read*/, false/*write*/ );
    pcl::FileFormatInstance file0( format0 );
    pcl::ImageDescriptionArray images0;

    if ( !file0.Open( images0, enabled[0]->path ) )
        throw pcl::Error( "FrameLoader: failed to open: " + enabled[0]->path );

    if ( images0.IsEmpty() )
    {
        file0.Close();
        throw pcl::Error( "FrameLoader: no image data in: " + enabled[0]->path );
    }

    int refWidth    = images0[0].info.width;
    int refHeight   = images0[0].info.height;
    int refChannels = images0[0].info.numberOfChannels;

    file0.Close();

    if ( refWidth <= 0 || refHeight <= 0 )
        throw pcl::Error( "FrameLoader: invalid dimensions in first frame" );

    console.WriteLn( pcl::String().Format(
        "  Reference: %d x %d, %d channel(s)",
        refWidth, refHeight, refChannels ) );

    // 3. Allocate SubCube (nEnabled, height, width)
    //    We store only the first channel (luminance for color images).
    SubCube cube( enabled.size(), size_t( refHeight ), size_t( refWidth ) );

    // 4. Load each enabled frame
    for ( size_t i = 0; i < enabled.size(); ++i )
    {
        const pcl::String& path = enabled[i]->path;

        console.WriteLn( pcl::String().Format(
            "  [%d/%d] %s",
            int( i + 1 ), int( enabled.size() ),
            pcl::IsoString( pcl::File::ExtractNameAndExtension( path ) ).c_str() ) );

        // Open file
        pcl::String ext = pcl::File::ExtractExtension( path ).Lowercase();
        pcl::FileFormat format( ext, true/*read*/, false/*write*/ );
        pcl::FileFormatInstance file( format );
        pcl::ImageDescriptionArray images;

        if ( !file.Open( images, path ) )
            throw pcl::Error( "FrameLoader: failed to open: " + path );

        if ( images.IsEmpty() )
        {
            file.Close();
            throw pcl::Error( "FrameLoader: no image data in: " + path );
        }

        // Validate dimensions match reference
        int w = images[0].info.width;
        int h = images[0].info.height;
        if ( w != refWidth || h != refHeight )
        {
            file.Close();
            throw pcl::Error( pcl::String().Format(
                "FrameLoader: dimension mismatch in frame %d — expected %dx%d, got %dx%d: ",
                int( i + 1 ), refWidth, refHeight, w, h ) + path );
        }

        // Read FITS keywords if available
        pcl::FITSKeywordArray keywords;
        if ( format.CanStoreKeywords() )
            file.ReadFITSKeywords( keywords );

        // Read the image
        pcl::Image img;
        if ( !file.ReadImage( img ) )
        {
            file.Close();
            throw pcl::Error( "FrameLoader: failed to read image data: " + path );
        }

        file.Close();

        // Copy first channel (luminance) into the SubCube.
        // PCL Image stores data channel-by-channel: PixelData(0) is a contiguous
        // array of height*width floats in row-major order (row after row).
        const pcl::Image::sample* src = img.PixelData( 0 );
        size_t numPx = size_t( refWidth ) * size_t( refHeight );
        cube.setSub( i, src, numPx );

        // Extract and store metadata from FITS keywords
        cube.setMetadata( i, ExtractMetadata( keywords ) );
    }

    console.WriteLn( pcl::String().Format(
        "<end><cbr>FrameLoader: loaded %d frames into SubCube (%d x %d x %d)",
        int( enabled.size() ), int( enabled.size() ), refHeight, refWidth ) );

    return cube;
}

// ----------------------------------------------------------------------------

LoadedFrames FrameLoader::LoadRaw( const std::vector<FramePath>& frames )
{
    // 1. Filter to enabled frames only
    std::vector<const FramePath*> enabled;
    for ( const auto& f : frames )
    {
        if ( f.enabled )
            enabled.push_back( &f );
    }

    if ( enabled.empty() )
        throw pcl::Error( "FrameLoader: no enabled frames to load" );

    pcl::Console console;
    console.WriteLn( pcl::String().Format(
        "<end><cbr>FrameLoader::LoadRaw: loading %d of %d frames",
        int( enabled.size() ), int( frames.size() ) ) );

    // 2. Open the first frame to get reference dimensions
    pcl::String ext0 = pcl::File::ExtractExtension( enabled[0]->path ).Lowercase();
    pcl::FileFormat format0( ext0, true/*read*/, false/*write*/ );
    pcl::FileFormatInstance file0( format0 );
    pcl::ImageDescriptionArray images0;

    if ( !file0.Open( images0, enabled[0]->path ) )
        throw pcl::Error( "FrameLoader: failed to open: " + enabled[0]->path );

    if ( images0.IsEmpty() )
    {
        file0.Close();
        throw pcl::Error( "FrameLoader: no image data in: " + enabled[0]->path );
    }

    int refWidth    = images0[0].info.width;
    int refHeight   = images0[0].info.height;
    int refChannels = images0[0].info.numberOfChannels;

    file0.Close();

    if ( refWidth <= 0 || refHeight <= 0 )
        throw pcl::Error( "FrameLoader: invalid dimensions in first frame" );

    console.WriteLn( pcl::String().Format(
        "  Reference: %d x %d, %d channel(s)", refWidth, refHeight, refChannels ) );

    // 3. Detect if CFA debayering is needed (single-channel + Bayer pattern keyword)
    //    Read FITS keywords from first frame to check for Bayer pattern.
    BayerPattern bayerPattern = BayerPattern::None;
    if ( refChannels == 1 )
    {
        pcl::String ext0b = pcl::File::ExtractExtension( enabled[0]->path ).Lowercase();
        pcl::FileFormat fmt0b( ext0b, true, false );
        pcl::FileFormatInstance f0b( fmt0b );
        pcl::ImageDescriptionArray imgs0b;
        if ( f0b.Open( imgs0b, enabled[0]->path ) )
        {
            pcl::FITSKeywordArray kw0;
            if ( fmt0b.CanStoreKeywords() )
                f0b.ReadFITSKeywords( kw0 );
            f0b.Close();
            bayerPattern = DetectBayerPattern( kw0 );
        }
    }

    bool needsDebayer = ( refChannels == 1 && bayerPattern != BayerPattern::None );
    int outChannels = needsDebayer ? 3 : refChannels;

    if ( needsDebayer )
    {
        const char* patName = bayerPattern == BayerPattern::RGGB ? "RGGB" :
                              bayerPattern == BayerPattern::GRBG ? "GRBG" :
                              bayerPattern == BayerPattern::GBRG ? "GBRG" : "BGGR";
        console.WriteLn( pcl::String().Format(
            "  CFA detected (%s) — will debayer to RGB", patName ) );
    }

    // 4. Prepare result
    LoadedFrames result;
    result.width       = refWidth;
    result.height      = refHeight;
    result.numChannels = outChannels;
    result.pixelData.resize( enabled.size() );
    result.metadata.resize( enabled.size() );

    // Parallel loading: pre-allocate per-frame error/warning slots
    const int LOAD_THREADS = 4;
    size_t N = enabled.size();
    std::vector<std::string> errors( N );
    std::vector<std::string> warnings( N );

    // 5. Load each enabled frame (parallel — file reads overlap)
    #pragma omp parallel for num_threads(LOAD_THREADS) schedule(dynamic)
    for ( size_t i = 0; i < N; ++i )
    {
        try
        {
            const pcl::String& path = enabled[i]->path;

            #pragma omp critical(console)
            {
                console.WriteLn( pcl::String().Format(
                    "  [%d/%d] %s",
                    int( i + 1 ), int( N ),
                    pcl::IsoString( pcl::File::ExtractNameAndExtension( path ) ).c_str() ) );
            }

            // Create format + instance as thread-local variables (persist for ReadImage)
            pcl::String ext = pcl::File::ExtractExtension( path ).Lowercase();
            pcl::FileFormat format( ext, true/*read*/, false/*write*/ );
            pcl::FileFormatInstance file( format );
            pcl::ImageDescriptionArray images;
            pcl::FITSKeywordArray keywords;
            int w = 0, h = 0;
            bool canStoreKW = format.CanStoreKeywords();

            // Serialize: Open + keyword read (CFITSIO global handle table safety)
            #pragma omp critical(cfitsio)
            {
                if ( !file.Open( images, path ) )
                    throw pcl::Error( "FrameLoader: failed to open: " + path );

                if ( images.IsEmpty() )
                {
                    file.Close();
                    throw pcl::Error( "FrameLoader: no image data in: " + path );
                }

                w = images[0].info.width;
                h = images[0].info.height;

                if ( canStoreKW )
                    file.ReadFITSKeywords( keywords );
            }

            // Validate dimensions match reference
            if ( w != refWidth || h != refHeight )
            {
                #pragma omp critical(cfitsio)
                { file.Close(); }
                throw pcl::Error( pcl::String().Format(
                    "FrameLoader: dimension mismatch in frame %d — expected %dx%d, got %dx%d: ",
                    int( i + 1 ), refWidth, refHeight, w, h ) + path );
            }

            // PARALLEL: Read the image (each thread has its own file handle)
            pcl::Image img;
            if ( !file.ReadImage( img ) )
            {
                #pragma omp critical(cfitsio)
                { file.Close(); }
                throw pcl::Error( "FrameLoader: failed to read image data: " + path );
            }

            // Serialize: Close (CFITSIO global handle table safety)
            #pragma omp critical(cfitsio)
            { file.Close(); }

            // === Everything below runs in PARALLEL (no shared state) ===

            img.Normalize();

            size_t numPx = size_t( refWidth ) * size_t( refHeight );

            if ( needsDebayer )
            {
                const pcl::Image::sample* cfa = img.PixelData( 0 );
                result.pixelData[i].resize( 3 );
                DebayerBilinear( cfa, refWidth, refHeight, bayerPattern,
                                 result.pixelData[i][0],
                                 result.pixelData[i][1],
                                 result.pixelData[i][2] );

                pcl::Image rgbImg;
                rgbImg.AllocateData( refWidth, refHeight, 3, pcl::ColorSpace::RGB );
                std::copy( result.pixelData[i][0].begin(), result.pixelData[i][0].end(),
                           rgbImg.PixelData( 0 ) );
                std::copy( result.pixelData[i][1].begin(), result.pixelData[i][1].end(),
                           rgbImg.PixelData( 1 ) );
                std::copy( result.pixelData[i][2].begin(), result.pixelData[i][2].end(),
                           rgbImg.PixelData( 2 ) );

                result.metadata[i] = ExtractMetadata( keywords );
                if ( result.metadata[i].fwhm == 0.0 && result.metadata[i].eccentricity == 0.0 )
                    ComputeFrameMetrics( rgbImg, result.metadata[i], &warnings[i] );
            }
            else
            {
                result.pixelData[i].resize( outChannels );
                for ( int c = 0; c < outChannels; ++c )
                {
                    const pcl::Image::sample* src = img.PixelData( c );
                    result.pixelData[i][c].assign( src, src + numPx );
                }

                result.metadata[i] = ExtractMetadata( keywords );
                if ( result.metadata[i].fwhm == 0.0 && result.metadata[i].eccentricity == 0.0 )
                    ComputeFrameMetrics( img, result.metadata[i], &warnings[i] );
            }
        }
        catch ( const pcl::Error& e )
        {
            try { errors[i] = pcl::IsoString( e.Message() ).c_str(); }
            catch ( ... ) { errors[i] = "FrameLoader: unknown error in frame " + std::to_string( i + 1 ); }
        }
        catch ( const std::exception& e )
        {
            errors[i] = e.what();
        }
        catch ( ... )
        {
            errors[i] = "FrameLoader: unknown error in frame " + std::to_string( i + 1 );
        }
    }

    // 6. Emit any warnings from parallel region
    for ( size_t i = 0; i < N; ++i )
    {
        if ( !warnings[i].empty() )
            console.WarningLn( pcl::String( warnings[i].c_str() ) );
    }

    // 7. Check for errors — throw the first one
    for ( size_t i = 0; i < N; ++i )
    {
        if ( !errors[i].empty() )
            throw pcl::Error( pcl::String( errors[i].c_str() ) );
    }

    console.WriteLn( pcl::String().Format(
        "<end><cbr>FrameLoader::LoadRaw: loaded %d frames (%d x %d, %d ch%s)",
        int( enabled.size() ), refWidth, refHeight, outChannels,
        needsDebayer ? ", debayered" : "" ) );

    return result;
}

// ----------------------------------------------------------------------------

SubMetadata FrameLoader::ExtractMetadata( const pcl::FITSKeywordArray& keywords )
{
    SubMetadata meta;

    meta.fwhm          = GetKeywordValue( keywords, "FWHM",         0.0 );
    meta.hfr           = GetKeywordValue( keywords, "HFR",          0.0 );
    meta.gain          = GetKeywordValue( keywords, "GAIN",         0.0 );

    // Eccentricity — different software uses different names
    meta.eccentricity  = GetKeywordValue( keywords, "ECCENTRI",     0.0 );
    if ( meta.eccentricity == 0.0 )
        meta.eccentricity = GetKeywordValue( keywords, "ECCENTRICITY", 0.0 );

    // Sky background
    meta.skyBackground = GetKeywordValue( keywords, "SKYBACK",      0.0 );
    if ( meta.skyBackground == 0.0 )
        meta.skyBackground = GetKeywordValue( keywords, "MSKY",     0.0 );

    // Altitude
    meta.altitude      = GetKeywordValue( keywords, "OBJCTALT",     0.0 );
    if ( meta.altitude == 0.0 )
        meta.altitude  = GetKeywordValue( keywords, "ALTITUDE",     0.0 );

    // Exposure time
    meta.exposure      = GetKeywordValue( keywords, "EXPTIME",      0.0 );
    if ( meta.exposure == 0.0 )
        meta.exposure  = GetKeywordValue( keywords, "EXPOSURE",     0.0 );

    // CCD temperature
    meta.ccdTemp       = GetKeywordValue( keywords, "CCD-TEMP",     0.0 );
    if ( meta.ccdTemp == 0.0 )
        meta.ccdTemp   = GetKeywordValue( keywords, "SET-TEMP",     0.0 );

    // String keywords
    pcl::String obj    = GetKeywordString( keywords, "OBJECT", pcl::String() );
    meta.object        = std::string( pcl::IsoString( obj ).c_str() );

    pcl::String filt   = GetKeywordString( keywords, "FILTER", pcl::String() );
    meta.filter        = std::string( pcl::IsoString( filt ).c_str() );

    return meta;
}

// ----------------------------------------------------------------------------

double FrameLoader::GetKeywordValue( const pcl::FITSKeywordArray& keywords,
                                     const pcl::IsoString& name,
                                     double defaultValue )
{
    for ( const pcl::FITSHeaderKeyword& kw : keywords )
    {
        if ( kw.name.Trimmed().CompareIC( name ) == 0 )
        {
            // FITS keyword values may be quoted strings containing numbers
            pcl::IsoString valStr = kw.value.Trimmed();
            valStr.Unquote();
            valStr.Trim();
            if ( !valStr.IsEmpty() )
            {
                try
                {
                    return valStr.ToDouble();
                }
                catch ( const std::bad_alloc& )
                {
                    throw;  // Memory errors must propagate
                }
                catch ( const std::exception& )
                {
                    // Malformed numeric value in FITS keyword — use default
                    return defaultValue;
                }
            }
        }
    }
    return defaultValue;
}

// ----------------------------------------------------------------------------

pcl::String FrameLoader::GetKeywordString( const pcl::FITSKeywordArray& keywords,
                                           const pcl::IsoString& name,
                                           const pcl::String& defaultValue )
{
    for ( const pcl::FITSHeaderKeyword& kw : keywords )
    {
        if ( kw.name.Trimmed().CompareIC( name ) == 0 )
        {
            pcl::IsoString valStr = kw.value.Trimmed();
            valStr.Unquote();
            valStr.Trim();
            if ( !valStr.IsEmpty() )
                return pcl::String( valStr );
        }
    }
    return defaultValue;
}

// ----------------------------------------------------------------------------

BayerPattern FrameLoader::DetectBayerPattern( const pcl::FITSKeywordArray& keywords )
{
    // Check common FITS keywords for Bayer/CFA pattern info.
    // BAYERPAT is standard; COLORTYP is used by some capture software.
    for ( const pcl::FITSHeaderKeyword& kw : keywords )
    {
        pcl::IsoString name = kw.name.Trimmed().Uppercase();
        if ( name == "BAYERPAT" || name == "COLORTYP" )
        {
            pcl::IsoString val = kw.value.Trimmed();
            val.Unquote();
            val.Trim();
            val = val.Uppercase();

            if ( val == "RGGB" ) return BayerPattern::RGGB;
            if ( val == "GRBG" ) return BayerPattern::GRBG;
            if ( val == "GBRG" ) return BayerPattern::GBRG;
            if ( val == "BGGR" ) return BayerPattern::BGGR;
        }
    }

    // Fallback: check XBAYROFF/YBAYROFF (Bayer offset keywords)
    // If present, the image is CFA even without an explicit pattern keyword.
    // Default to RGGB (most common OSC pattern).
    for ( const pcl::FITSHeaderKeyword& kw : keywords )
    {
        pcl::IsoString name = kw.name.Trimmed().Uppercase();
        if ( name == "XBAYROFF" || name == "YBAYROFF" || name == "CFATYPE" )
        {
            pcl::Console().WarningLn( "CFA pattern not explicit -- assuming RGGB. "
                "If colors look wrong, check BAYERPAT keyword." );
            return BayerPattern::RGGB;
        }
    }

    return BayerPattern::None;
}

// ----------------------------------------------------------------------------

void FrameLoader::DebayerBilinear( const float* cfa, int W, int H,
                                    BayerPattern pattern,
                                    std::vector<float>& outR,
                                    std::vector<float>& outG,
                                    std::vector<float>& outB )
{
    size_t numPx = size_t( W ) * size_t( H );
    outR.resize( numPx );
    outG.resize( numPx );
    outB.resize( numPx );

    // Map pattern to per-pixel color indices.
    // For a 2x2 Bayer tile, determine which color each position has:
    //   0 = Red, 1 = Green, 2 = Blue
    // The tile repeats across the image. Position (y,x) maps to tile (y%2, x%2).
    int tileColor[2][2]; // [row%2][col%2] → 0=R, 1=G, 2=B

    switch ( pattern )
    {
    case BayerPattern::RGGB:
        tileColor[0][0] = 0; tileColor[0][1] = 1;
        tileColor[1][0] = 1; tileColor[1][1] = 2;
        break;
    case BayerPattern::GRBG:
        tileColor[0][0] = 1; tileColor[0][1] = 0;
        tileColor[1][0] = 2; tileColor[1][1] = 1;
        break;
    case BayerPattern::GBRG:
        tileColor[0][0] = 1; tileColor[0][1] = 2;
        tileColor[1][0] = 0; tileColor[1][1] = 1;
        break;
    case BayerPattern::BGGR:
        tileColor[0][0] = 2; tileColor[0][1] = 1;
        tileColor[1][0] = 1; tileColor[1][1] = 0;
        break;
    default:
        // Should not happen — fill with CFA as luminance
        for ( size_t i = 0; i < numPx; ++i )
            outR[i] = outG[i] = outB[i] = cfa[i];
        return;
    }

    // Bilinear interpolation: for each pixel, the "native" color is read directly
    // from the CFA; the other two colors are interpolated from neighbors.
    // Clamp to image bounds.
    auto px = [&]( int y, int x ) -> float {
        y = std::max( 0, std::min( H - 1, y ) );
        x = std::max( 0, std::min( W - 1, x ) );
        return cfa[y * W + x];
    };

    for ( int y = 0; y < H; ++y )
    {
        for ( int x = 0; x < W; ++x )
        {
            int color = tileColor[y & 1][x & 1];
            size_t idx = y * W + x;

            float r, g, b;

            if ( color == 0 )  // Red pixel
            {
                r = cfa[idx];
                g = 0.25f * ( px(y-1,x) + px(y+1,x) + px(y,x-1) + px(y,x+1) );
                b = 0.25f * ( px(y-1,x-1) + px(y-1,x+1) + px(y+1,x-1) + px(y+1,x+1) );
            }
            else if ( color == 2 )  // Blue pixel
            {
                b = cfa[idx];
                g = 0.25f * ( px(y-1,x) + px(y+1,x) + px(y,x-1) + px(y,x+1) );
                r = 0.25f * ( px(y-1,x-1) + px(y-1,x+1) + px(y+1,x-1) + px(y+1,x+1) );
            }
            else  // Green pixel
            {
                g = cfa[idx];

                // Green pixels have two arrangements: on a red row or blue row
                // Determine neighbors based on which color is in the same row
                int rowParity = y & 1;
                int colParity = x & 1;

                // Check if horizontal neighbors are red or blue
                int hColor = tileColor[rowParity][1 - colParity];

                if ( hColor == 0 )  // Horizontal neighbor is red → red is on this row
                {
                    r = 0.5f * ( px(y, x-1) + px(y, x+1) );
                    b = 0.5f * ( px(y-1, x) + px(y+1, x) );
                }
                else  // Horizontal neighbor is blue → blue is on this row
                {
                    b = 0.5f * ( px(y, x-1) + px(y, x+1) );
                    r = 0.5f * ( px(y-1, x) + px(y+1, x) );
                }
            }

            outR[idx] = r;
            outG[idx] = g;
            outB[idx] = b;
        }
    }
}

// ----------------------------------------------------------------------------

void FrameLoader::ComputeFrameMetrics( const pcl::Image& img, SubMetadata& meta,
                                        std::string* warningOut )
{
    try
    {
        // Wrap image for PCL star detection (stack-local copy, non-owning alias)
        pcl::Image imgCopy( img );
        pcl::ImageVariant iv( &imgCopy );

        // Detect stars
        pcl::StarDetector detector;
        detector.SetSensitivity( 0.1 );
        detector.SetMaxDistortion( 0.6 );
        detector.SetMinSNR( 5.0 );
        auto stars = detector.DetectStars( iv );

        if ( stars.IsEmpty() )
            return;

        // Fit PSFs on the brightest stars (capped at 50 for speed)
        int maxFit = std::min( 50, int( stars.Length() ) );
        double sumFWHM = 0, sumEcc = 0, sumBkg = 0;
        int count = 0;

        for ( int i = 0; i < maxFit; ++i )
        {
            pcl::PSFFit fit( iv, stars[i].pos, pcl::DRect( stars[i].srect ),
                             pcl::PSFunction::Gaussian, false /*elliptical*/ );

            if ( fit.psf.status == pcl::PSFFitStatus::FittedOk )
            {
                sumFWHM += ( fit.psf.FWHMx() + fit.psf.FWHMy() ) * 0.5;

                double sx = std::max( fit.psf.sx, fit.psf.sy );
                double sy = std::min( fit.psf.sx, fit.psf.sy );
                sumEcc += ( sx > 0 ) ? std::sqrt( 1.0 - ( sy * sy ) / ( sx * sx ) ) : 0.0;

                sumBkg += fit.psf.B;
                ++count;
            }
        }

        if ( count > 0 )
        {
            meta.fwhm          = sumFWHM / count;
            meta.eccentricity  = sumEcc / count;
            meta.skyBackground = sumBkg / count;
            meta.hfr           = meta.fwhm * 0.5;  // HFR ≈ FWHM/2 for Gaussian PSF
        }
    }
    catch ( const pcl::Error& e )
    {
        if ( warningOut )
            *warningOut = "FrameLoader: PSF metrics failed: "
                + std::string( pcl::IsoString( e.Message() ).c_str() )
                + " -- frame will use default quality score";
    }
    catch ( const std::exception& e )
    {
        if ( warningOut )
            *warningOut = std::string( "FrameLoader: PSF metrics failed: " )
                + e.what() + " -- frame will use default quality score";
    }
}

// ----------------------------------------------------------------------------

} // namespace nukex
