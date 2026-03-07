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

#include <cstring>

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

    file0.Close();

    if ( refWidth <= 0 || refHeight <= 0 )
        throw pcl::Error( "FrameLoader: invalid dimensions in first frame" );

    console.WriteLn( pcl::String().Format(
        "  Reference: %d x %d", refWidth, refHeight ) );

    // 3. Prepare result
    LoadedFrames result;
    result.width  = refWidth;
    result.height = refHeight;
    result.pixelData.resize( enabled.size() );
    result.metadata.resize( enabled.size() );

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

        // Store raw pixel data (first channel / luminance)
        const pcl::Image::sample* src = img.PixelData( 0 );
        size_t numPx = size_t( refWidth ) * size_t( refHeight );
        result.pixelData[i].assign( src, src + numPx );

        // Extract and store metadata from FITS keywords
        result.metadata[i] = ExtractMetadata( keywords );
    }

    console.WriteLn( pcl::String().Format(
        "<end><cbr>FrameLoader::LoadRaw: loaded %d frames (%d x %d)",
        int( enabled.size() ), refWidth, refHeight ) );

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
                catch ( ... )
                {
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

} // namespace nukex
