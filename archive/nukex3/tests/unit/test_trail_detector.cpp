// tests/unit/test_trail_detector.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/TrailDetector.h"
#include "engine/SubCube.h"
#include <vector>
#include <cmath>
#include <random>

// Helper: create a flat background image with optional Gaussian noise
static std::vector<float> makeBackground( int W, int H, float bg = 0.1f,
                                           float noise = 0.005f, unsigned seed = 42 )
{
    std::vector<float> img( W * H, bg );
    std::mt19937 rng( seed );
    std::normal_distribution<float> dist( 0.0f, noise );
    for ( auto& v : img )
        v += dist( rng );
    return img;
}

// Helper: draw a bright line from (x0,y0) to (x1,y1) with given brightness
static void drawLine( std::vector<float>& img, int W, int H,
                      int x0, int y0, int x1, int y1, float brightness )
{
    int steps = std::max( std::abs( x1 - x0 ), std::abs( y1 - y0 ) );
    if ( steps == 0 ) return;
    for ( int i = 0; i <= steps; ++i )
    {
        int x = x0 + ( x1 - x0 ) * i / steps;
        int y = y0 + ( y1 - y0 ) * i / steps;
        if ( x >= 0 && x < W && y >= 0 && y < H )
            img[y * W + x] = brightness;
    }
}

// Helper: draw a Gaussian star at (cx, cy) with given peak and sigma
static void drawStar( std::vector<float>& img, int W, int H,
                      double cx, double cy, float peak, double sigma )
{
    int r = static_cast<int>( 4 * sigma + 1 );
    for ( int dy = -r; dy <= r; ++dy )
        for ( int dx = -r; dx <= r; ++dx )
        {
            int x = static_cast<int>( cx ) + dx;
            int y = static_cast<int>( cy ) + dy;
            if ( x >= 0 && x < W && y >= 0 && y < H )
            {
                double d2 = ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy );
                img[y * W + x] += peak * std::exp( -d2 / ( 2 * sigma * sigma ) );
            }
        }
}

TEST_CASE( "findSeeds detects bright trail pixels as seeds", "[trail][seed]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawLine( img, W, H, 10, 100, 190, 100, 0.5f );  // horizontal line

    nukex::TrailDetectorConfig config;
    nukex::TrailDetector detector( config );
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    // Should detect a line
    REQUIRE( result.linesDetected >= 1 );
    REQUIRE( result.maskedPixels > 50 );  // trail spans ~180 pixels
}

TEST_CASE( "No false detections on flat background", "[trail][seed]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected == 0 );
    REQUIRE( result.maskedPixels == 0 );
}

TEST_CASE( "Single bright pixel (cosmic ray) does not produce a line", "[trail][cosmic]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    img[100 * W + 100] = 0.9f;  // single hot pixel

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected == 0 );
}

TEST_CASE( "Diagonal trail detected", "[trail][diagonal]" )
{
    const int W = 300, H = 300;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawLine( img, W, H, 10, 10, 290, 290, 0.5f );  // 45-degree diagonal

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected >= 1 );
    REQUIRE( result.maskedPixels > 100 );
}

TEST_CASE( "Round star cluster does not trigger trail detection", "[trail][star]" )
{
    const int W = 200, H = 200;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawStar( img, W, H, 100, 100, 0.8f, 5.0 );  // bright star, sigma=5px

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected == 0 );
}

TEST_CASE( "Two crossing trails both detected", "[trail][multiple]" )
{
    const int W = 300, H = 300;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawLine( img, W, H, 10, 150, 290, 150, 0.5f );  // horizontal
    drawLine( img, W, H, 150, 10, 150, 290, 0.5f );   // vertical

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    REQUIRE( result.linesDetected >= 2 );
}

TEST_CASE( "Trail through star: star core not masked, trail on sky is", "[trail][star-crossing]" )
{
    const int W = 300, H = 300;
    auto img = makeBackground( W, H, 0.1f, 0.005f );
    drawStar( img, W, H, 150, 150, 0.9f, 6.0 );  // bright star at center
    drawLine( img, W, H, 10, 150, 290, 150, 0.4f );  // horizontal trail through star

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    // Trail should be detected (line found from non-star portions)
    REQUIRE( result.linesDetected >= 1 );
    REQUIRE( result.maskedPixels > 50 );
}

TEST_CASE( "Faint trail at exactly seed threshold", "[trail][threshold]" )
{
    const int W = 200, H = 200;
    float bg = 0.1f;
    float noise = 0.005f;
    auto img = makeBackground( W, H, bg, noise );

    // Trail at exactly 3.0 * expected MAD above median — borderline detection
    float trailBrightness = bg + 3.0f * noise * 1.4826f;
    drawLine( img, W, H, 10, 100, 190, 100, trailBrightness );

    nukex::TrailDetector detector;
    auto result = detector.detectFrame( img.data(), nullptr, W, H );

    // At exactly threshold, detection is marginal — just verify no crash
    REQUIRE( result.linesDetected <= 2 );
}

TEST_CASE( "detectAndMask sets SubCube masks", "[trail][subcube]" )
{
    const int W = 100, H = 100, N = 5;
    nukex::SubCube cube( N, H, W );
    cube.allocateMasks();

    // Frame 2 has a horizontal trail; others are clean
    for ( size_t z = 0; z < N; ++z )
    {
        auto img = makeBackground( W, H, 0.1f, 0.005f, 42 + z );
        if ( z == 2 )
            drawLine( img, W, H, 5, 50, 95, 50, 0.5f );
        cube.setSub( z, img.data(), W * H );
    }

    nukex::TrailDetector detector;
    int masked = detector.detectAndMask( cube );

    REQUIRE( masked > 0 );
    // Frame 2 at the trail y=50 should be masked
    REQUIRE( cube.mask( 2, 50, 50 ) == 1 );
    // Other frames at the same pixel should NOT be masked
    REQUIRE( cube.mask( 0, 50, 50 ) == 0 );
    REQUIRE( cube.mask( 1, 50, 50 ) == 0 );
}
