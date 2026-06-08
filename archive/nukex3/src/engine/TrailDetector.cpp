// src/engine/TrailDetector.cpp
#include "engine/TrailDetector.h"
#include "engine/SubCube.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>
#include <vector>
#include <utility>

namespace nukex {

TrailDetector::TrailDetector( const TrailDetectorConfig& config )
    : m_config( config )
{
}

// ---------------------------------------------------------------------------
// findSeeds — local median+MAD spatial outlier scan
// ---------------------------------------------------------------------------
std::vector<uint8_t> TrailDetector::findSeeds( const float* frameData,
                                                const uint8_t* alignMask,
                                                int width, int height ) const
{
    const int N = width * height;
    std::vector<uint8_t> seeds( N, 0 );
    const int halfWin = m_config.seedWindowSize / 2;

    std::vector<float> neighbors;
    neighbors.reserve( m_config.seedWindowSize * m_config.seedWindowSize );

    std::vector<float> absDevs;
    absDevs.reserve( m_config.seedWindowSize * m_config.seedWindowSize );

    for ( int y = 0; y < height; ++y )
    {
        for ( int x = 0; x < width; ++x )
        {
            const int idx = y * width + x;

            // Skip alignment-masked pixels
            if ( alignMask && alignMask[idx] )
                continue;

            // Collect valid neighbors in window
            neighbors.clear();
            for ( int wy = y - halfWin; wy <= y + halfWin; ++wy )
            {
                if ( wy < 0 || wy >= height ) continue;
                for ( int wx = x - halfWin; wx <= x + halfWin; ++wx )
                {
                    if ( wx < 0 || wx >= width ) continue;
                    int nIdx = wy * width + wx;
                    if ( alignMask && alignMask[nIdx] )
                        continue;
                    neighbors.push_back( frameData[nIdx] );
                }
            }

            // Skip if fewer than 5 valid neighbors
            if ( static_cast<int>( neighbors.size() ) < 5 )
                continue;

            // Compute local median
            size_t mid = neighbors.size() / 2;
            std::nth_element( neighbors.begin(), neighbors.begin() + mid, neighbors.end() );
            float localMedian = neighbors[mid];

            // Compute MAD (median absolute deviation)
            absDevs.resize( neighbors.size() );
            for ( size_t i = 0; i < neighbors.size(); ++i )
                absDevs[i] = std::fabs( neighbors[i] - localMedian );
            size_t madMid = absDevs.size() / 2;
            std::nth_element( absDevs.begin(), absDevs.begin() + madMid, absDevs.end() );
            float localMAD = absDevs[madMid] * 1.4826f;

            // Flag as seed if pixel exceeds threshold
            float val = frameData[idx];
            if ( localMAD > 0 && val > localMedian + m_config.seedSigma * localMAD )
                seeds[idx] = 1;
        }
    }

    return seeds;
}

// ---------------------------------------------------------------------------
// clusterSeeds — morphological dilation + union-find CCL + eigenvalue linearity filter
// ---------------------------------------------------------------------------
std::vector<TrailDetector::Cluster> TrailDetector::clusterSeeds(
    const std::vector<uint8_t>& seeds, int width, int height ) const
{
    const int N = width * height;
    const int gap = m_config.gapTolerance;

    // Step 1: Dilate seed mask by gapTolerance pixels (box dilation)
    std::vector<uint8_t> dilated( N, 0 );
    for ( int y = 0; y < height; ++y )
    {
        for ( int x = 0; x < width; ++x )
        {
            if ( !seeds[y * width + x] )
                continue;
            // Dilate: set all pixels in box around this seed
            for ( int dy = -gap; dy <= gap; ++dy )
            {
                int ny = y + dy;
                if ( ny < 0 || ny >= height ) continue;
                for ( int dx = -gap; dx <= gap; ++dx )
                {
                    int nx = x + dx;
                    if ( nx < 0 || nx >= width ) continue;
                    dilated[ny * width + nx] = 1;
                }
            }
        }
    }

    // Step 2: Connected-component labeling on dilated mask using union-find (8-connectivity)
    std::vector<int> parent( N, -1 );  // -1 = not part of any component

    // Union-find with path compression
    auto findRoot = [&]( int a ) -> int {
        while ( parent[a] != a )
        {
            parent[a] = parent[parent[a]];  // path compression
            a = parent[a];
        }
        return a;
    };

    auto unite = [&]( int a, int b ) {
        int ra = findRoot( a );
        int rb = findRoot( b );
        if ( ra != rb )
            parent[rb] = ra;
    };

    // First pass: scan and assign labels
    for ( int y = 0; y < height; ++y )
    {
        for ( int x = 0; x < width; ++x )
        {
            int idx = y * width + x;
            if ( !dilated[idx] )
                continue;

            // Initialize self as root
            parent[idx] = idx;

            // Check 4 neighbors that were already visited: left, upper-left, up, upper-right
            // (8-connectivity backward scan)
            int neighbors[4][2] = { {y, x-1}, {y-1, x-1}, {y-1, x}, {y-1, x+1} };
            for ( int n = 0; n < 4; ++n )
            {
                int ny = neighbors[n][0];
                int nx = neighbors[n][1];
                if ( ny < 0 || ny >= height || nx < 0 || nx >= width )
                    continue;
                int nIdx = ny * width + nx;
                if ( parent[nIdx] < 0 )
                    continue;  // not in a component
                unite( idx, nIdx );
            }
        }
    }

    // Step 3: Group original (undilated) seed pixels by their component label
    std::unordered_map<int, std::vector<int>> componentSeeds;
    for ( int i = 0; i < N; ++i )
    {
        if ( seeds[i] && parent[i] >= 0 )
        {
            int root = findRoot( i );
            componentSeeds[root].push_back( i );
        }
    }

    // Step 4: For each component, compute PCA linearity and filter.
    // Helper lambda: try to build a Cluster from a set of pixel indices.
    // Returns true and fills 'out' if the cluster passes linearity+extent filters.
    auto tryBuildCluster = [&]( const std::vector<int>& pxIndices, Cluster& out ) -> bool
    {
        if ( static_cast<int>( pxIndices.size() ) < 3 )
            return false;

        // Compute centroid
        double cx = 0, cy = 0;
        for ( int idx : pxIndices )
        {
            cx += idx % width;
            cy += idx / width;
        }
        cx /= pxIndices.size();
        cy /= pxIndices.size();

        // Compute 2x2 covariance matrix
        double cxx = 0, cxy = 0, cyy = 0;
        for ( int idx : pxIndices )
        {
            double dx = ( idx % width ) - cx;
            double dy = ( idx / width ) - cy;
            cxx += dx * dx;
            cxy += dx * dy;
            cyy += dy * dy;
        }
        double n = static_cast<double>( pxIndices.size() );
        cxx /= n;
        cxy /= n;
        cyy /= n;

        // Eigenvalues via quadratic formula
        double trace = cxx + cyy;
        double det = cxx * cyy - cxy * cxy;
        double disc = trace * trace - 4.0 * det;
        if ( disc < 0 ) disc = 0;
        double sqrtDisc = std::sqrt( disc );
        double lambda1 = ( trace + sqrtDisc ) / 2.0;
        double lambda2 = ( trace - sqrtDisc ) / 2.0;

        if ( lambda1 <= 0 )
            return false;

        double linearity = 1.0 - lambda2 / lambda1;

        // Principal direction from eigenvector of lambda_max
        double dirX, dirY;
        if ( std::fabs( cxy ) > 1e-12 )
        {
            dirX = lambda1 - cyy;
            dirY = cxy;
        }
        else
        {
            if ( cxx >= cyy ) { dirX = 1.0; dirY = 0.0; }
            else              { dirX = 0.0; dirY = 1.0; }
        }
        double dirLen = std::sqrt( dirX * dirX + dirY * dirY );
        if ( dirLen > 0 ) { dirX /= dirLen; dirY /= dirLen; }

        // Extent along principal axis
        double minProj = 1e30, maxProj = -1e30;
        for ( int idx : pxIndices )
        {
            double dx = ( idx % width ) - cx;
            double dy = ( idx / width ) - cy;
            double proj = dx * dirX + dy * dirY;
            minProj = std::min( minProj, proj );
            maxProj = std::max( maxProj, proj );
        }
        double extent = maxProj - minProj;

        if ( linearity >= m_config.linearityMin &&
             extent >= static_cast<double>( m_config.minClusterLen ) )
        {
            out.pixelIndices = pxIndices;
            out.cx = cx;  out.cy = cy;
            out.dirX = dirX;  out.dirY = dirY;
            out.linearity = linearity;
            out.extent = extent;
            return true;
        }
        return false;
    };

    // Recursive splitting: if a cluster fails linearity but is large enough,
    // split along the minor axis (perpendicular to principal) and retry each half.
    // This handles crossing trails merged into one component.
    std::vector<Cluster> result;

    std::function<void( const std::vector<int>&, int )> evaluateCluster;
    evaluateCluster = [&]( const std::vector<int>& pxIndices, int depth )
    {
        if ( static_cast<int>( pxIndices.size() ) < 3 )
            return;

        Cluster c;
        if ( tryBuildCluster( pxIndices, c ) )
        {
            result.push_back( std::move( c ) );
            return;
        }

        // If too few seeds or too deep, give up
        if ( depth >= 4 || static_cast<int>( pxIndices.size() ) < 6 )
            return;

        // Compute centroid and minor axis for splitting
        double cx = 0, cy = 0;
        for ( int idx : pxIndices )
        {
            cx += idx % width;
            cy += idx / width;
        }
        cx /= pxIndices.size();
        cy /= pxIndices.size();

        // Compute covariance to get minor axis
        double cxx = 0, cxy = 0, cyy = 0;
        for ( int idx : pxIndices )
        {
            double dx = ( idx % width ) - cx;
            double dy = ( idx / width ) - cy;
            cxx += dx * dx;
            cxy += dx * dy;
            cyy += dy * dy;
        }
        double n = static_cast<double>( pxIndices.size() );
        cxx /= n;  cxy /= n;  cyy /= n;

        double trace = cxx + cyy;
        double det = cxx * cyy - cxy * cxy;
        double disc = trace * trace - 4.0 * det;
        if ( disc < 0 ) disc = 0;
        double sqrtDisc = std::sqrt( disc );
        double lambda2 = ( trace - sqrtDisc ) / 2.0;  // minor eigenvalue

        // Minor axis eigenvector (perpendicular to principal)
        double minDirX, minDirY;
        if ( std::fabs( cxy ) > 1e-12 )
        {
            minDirX = lambda2 - cyy;
            minDirY = cxy;
        }
        else
        {
            if ( cxx >= cyy ) { minDirX = 0.0; minDirY = 1.0; }
            else              { minDirX = 1.0; minDirY = 0.0; }
        }
        double len = std::sqrt( minDirX * minDirX + minDirY * minDirY );
        if ( len > 0 ) { minDirX /= len; minDirY /= len; }

        // Split seeds by sign of projection onto minor axis
        std::vector<int> groupA, groupB;
        for ( int idx : pxIndices )
        {
            double dx = ( idx % width ) - cx;
            double dy = ( idx / width ) - cy;
            double proj = dx * minDirX + dy * minDirY;
            if ( proj >= 0 )
                groupA.push_back( idx );
            else
                groupB.push_back( idx );
        }

        evaluateCluster( groupA, depth + 1 );
        evaluateCluster( groupB, depth + 1 );
    };

    for ( auto& [root, pixelIndices] : componentSeeds )
        evaluateCluster( pixelIndices, 0 );

    return result;
}

// ---------------------------------------------------------------------------
// walkAndConfirm — line walk + cross-line neighbor check + dilation
// ---------------------------------------------------------------------------
std::vector<uint8_t> TrailDetector::walkAndConfirm(
    const float* frameData, const uint8_t* alignMask,
    const std::vector<Cluster>& clusters,
    int width, int height,
    std::vector<TrailLine>& linesOut ) const
{
    const int N = width * height;
    std::vector<uint8_t> mask( N, 0 );

    for ( const auto& cluster : clusters )
    {
        double cx = cluster.cx;
        double cy = cluster.cy;
        double dx = cluster.dirX;
        double dy = cluster.dirY;

        // Perpendicular direction
        double px = -dy;
        double py = dx;

        // Find parametric t range where line is within frame bounds
        // Line: (cx + t*dx, cy + t*dy)
        double tMin = -1e9, tMax = 1e9;

        // Constrain so cx + t*dx in [0, width-1]
        if ( std::fabs( dx ) > 1e-12 )
        {
            double t1 = ( 0 - cx ) / dx;
            double t2 = ( width - 1 - cx ) / dx;
            if ( t1 > t2 ) std::swap( t1, t2 );
            tMin = std::max( tMin, t1 );
            tMax = std::min( tMax, t2 );
        }
        else
        {
            // dx ~ 0, line is nearly vertical
            if ( cx < 0 || cx >= width )
                continue;
        }

        // Constrain so cy + t*dy in [0, height-1]
        if ( std::fabs( dy ) > 1e-12 )
        {
            double t1 = ( 0 - cy ) / dy;
            double t2 = ( height - 1 - cy ) / dy;
            if ( t1 > t2 ) std::swap( t1, t2 );
            tMin = std::max( tMin, t1 );
            tMax = std::min( tMax, t2 );
        }
        else
        {
            if ( cy < 0 || cy >= height )
                continue;
        }

        if ( tMin > tMax )
            continue;

        // Walk at 1-pixel steps
        std::vector<int> confirmedPixels;
        int steps = static_cast<int>( std::ceil( tMax - tMin ) );
        confirmedPixels.reserve( steps + 1 );
        std::vector<uint8_t> visited( width * height, 0 );

        for ( int step = 0; step <= steps; ++step )
        {
            double t = tMin + step;
            if ( t > tMax ) t = tMax;

            int lx = static_cast<int>( std::round( cx + t * dx ) );
            int ly = static_cast<int>( std::round( cy + t * dy ) );

            if ( lx < 0 || lx >= width || ly < 0 || ly >= height )
                continue;

            int lineIdx = ly * width + lx;

            // Skip alignment-masked pixels
            if ( alignMask && alignMask[lineIdx] )
                continue;

            // Sample cross-line neighbors at +/-1..+/-crossLineOffset perpendicular
            std::vector<float> crossNeighbors;
            crossNeighbors.reserve( 2 * m_config.crossLineOffset );

            for ( int offset = 1; offset <= m_config.crossLineOffset; ++offset )
            {
                for ( int sign = -1; sign <= 1; sign += 2 )
                {
                    int nx = static_cast<int>( std::round( lx + sign * offset * px ) );
                    int ny = static_cast<int>( std::round( ly + sign * offset * py ) );
                    if ( nx < 0 || nx >= width || ny < 0 || ny >= height )
                        continue;
                    int nIdx = ny * width + nx;
                    if ( alignMask && alignMask[nIdx] )
                        continue;
                    crossNeighbors.push_back( frameData[nIdx] );
                }
            }

            // Skip if fewer than 2 valid neighbors
            if ( static_cast<int>( crossNeighbors.size() ) < 2 )
                continue;

            // Compute neighbor median + MAD
            size_t mid = crossNeighbors.size() / 2;
            std::nth_element( crossNeighbors.begin(),
                              crossNeighbors.begin() + mid,
                              crossNeighbors.end() );
            float neighborMedian = crossNeighbors[mid];

            std::vector<float> absDevs( crossNeighbors.size() );
            for ( size_t i = 0; i < crossNeighbors.size(); ++i )
                absDevs[i] = std::fabs( crossNeighbors[i] - neighborMedian );
            size_t madMid = absDevs.size() / 2;
            std::nth_element( absDevs.begin(), absDevs.begin() + madMid, absDevs.end() );
            float neighborMAD = absDevs[madMid] * 1.4826f;

            // Confirm trail pixel (deduplicate — adjacent t values can map to same pixel)
            float val = frameData[lineIdx];
            if ( !visited[lineIdx] && neighborMAD > 1e-10f && val > neighborMedian + m_config.confirmSigma * neighborMAD )
            {
                confirmedPixels.push_back( lineIdx );
                visited[lineIdx] = 1;
            }
        }

        // Reject if fewer than 5 confirmed pixels
        if ( static_cast<int>( confirmedPixels.size() ) < 5 )
            continue;

        // Record this trail line
        TrailLine line;
        line.cx = cx;
        line.cy = cy;
        line.dx = dx;
        line.dy = dy;
        line.confirmedCount = static_cast<int>( confirmedPixels.size() );
        linesOut.push_back( line );

        // Dilate confirmed pixels perpendicular to line by dilateRadius
        int dilateR = static_cast<int>( std::ceil( m_config.dilateRadius ) );
        for ( int cIdx : confirmedPixels )
        {
            int basex = cIdx % width;
            int basey = cIdx / width;

            // Mark the center pixel
            mask[cIdx] = 1;

            // Dilate perpendicular to line direction
            for ( int d = -dilateR; d <= dilateR; ++d )
            {
                if ( d == 0 ) continue;
                int nx = static_cast<int>( std::round( basex + d * px ) );
                int ny = static_cast<int>( std::round( basey + d * py ) );
                if ( nx >= 0 && nx < width && ny >= 0 && ny < height )
                    mask[ny * width + nx] = 1;
            }
        }
    }

    return mask;
}

// ---------------------------------------------------------------------------
// detectFrame — orchestrate the pipeline for a single frame
// ---------------------------------------------------------------------------
FrameTrailResult TrailDetector::detectFrame( const float* frameData,
                                              const uint8_t* alignMask,
                                              int width, int height ) const
{
    FrameTrailResult result;

    auto seeds = findSeeds( frameData, alignMask, width, height );
    auto clusters = clusterSeeds( seeds, width, height );

    if ( clusters.empty() )
    {
        result.linesDetected = 0;
        result.maskedPixels = 0;
        return result;
    }

    auto trailMask = walkAndConfirm( frameData, alignMask, clusters, width, height, result.lines );

    int maskedCount = 0;
    for ( auto v : trailMask )
        maskedCount += v;

    result.linesDetected = static_cast<int>( result.lines.size() );
    result.maskedPixels = maskedCount;

    return result;
}

// ---------------------------------------------------------------------------
// detectAndMask — operate on SubCube with OpenMP
// ---------------------------------------------------------------------------
int TrailDetector::detectAndMask( SubCube& cube, LogCallback log ) const
{
    const int W = static_cast<int>( cube.width() );
    const int H = static_cast<int>( cube.height() );
    const int N = static_cast<int>( cube.numSubs() );
    const int totalPixels = W * H;

    if ( !cube.hasMasks() )
        cube.allocateMasks();

    // Per-frame detection in parallel, collect results
    struct FrameResult {
        int maskedCount;
        std::vector<uint8_t> trailMask;
        std::vector<TrailLine> lines;
    };
    std::vector<FrameResult> results( N );

    #pragma omp parallel for schedule(dynamic)
    for ( int z = 0; z < N; ++z )
    {
        // Extract frame z as row-major float array
        std::vector<float> frame( totalPixels );
        for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
                frame[y * W + x] = cube.pixel( z, y, x );

        // Extract alignment mask for frame z
        std::vector<uint8_t> alignMask( totalPixels );
        for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
                alignMask[y * W + x] = cube.mask( z, y, x );

        auto seeds = findSeeds( frame.data(), alignMask.data(), W, H );
        auto clusters = clusterSeeds( seeds, W, H );

        if ( clusters.empty() )
        {
            results[z].maskedCount = 0;
            continue;
        }

        results[z].trailMask = walkAndConfirm( frame.data(), alignMask.data(),
                                                clusters, W, H, results[z].lines );

        int count = 0;
        for ( auto v : results[z].trailMask )
            count += v;
        results[z].maskedCount = count;
    }

    // Apply masks serial (logging not thread-safe)
    int totalMasked = 0;
    for ( int z = 0; z < N; ++z )
    {
        if ( results[z].maskedCount == 0 )
            continue;

        for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
                if ( results[z].trailMask[y * W + x] )
                    cube.setMask( z, y, x, 1 );

        int frameMasked = results[z].maskedCount;
        totalMasked += frameMasked;

        if ( log )
        {
            log( "    Frame " + std::to_string( z ) + ": " +
                 std::to_string( results[z].lines.size() ) + " trail(s), " +
                 std::to_string( frameMasked ) + " pixels masked" );

            double maskFrac = static_cast<double>( frameMasked ) / totalPixels;
            if ( maskFrac > 0.3 )
                log( "    WARNING: frame " + std::to_string( z ) +
                     " has " + std::to_string( static_cast<int>( maskFrac * 100 ) ) +
                     "% pixels masked — consider excluding this frame" );
        }
    }

    return totalMasked;
}

} // namespace nukex
