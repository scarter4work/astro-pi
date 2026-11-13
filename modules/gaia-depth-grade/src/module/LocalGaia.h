// Offline distance source: PixInsight's local Gaia DR3 database, read in-process with
// pcl::GaiaDatabaseFile (plan Task 12; replaces the PJSR queryLocalGaiaDR3 + TSV + sidecar path).
#ifndef __GDG_LocalGaia_h
#define __GDG_LocalGaia_h

#include "core/Catalog.h"
#include "core/Log.h"

#include <pcl/StringList.h>

#include <optional>
#include <vector>

namespace pcl
{

/// One GaiaDatabaseFile search row, as the Gaia process reports it.
struct LocalGaiaStar
{
   double ra, dec;                    // degrees
   double parx, magG, magBP, magRP;   // the database's float values, widened exactly
   uint32 flags;
};

class LocalGaiaSource : public gdg::CatalogSource
{
public:

   /// `xpsdPaths`: the Gaia DR3 (or DR3SP) database files to search. `magLimit`: the G cut
   /// (search magnitudeHigh, then the strict G < limit of local_gaia.py); none = no cut.
   LocalGaiaSource( const StringList& xpsdPaths, std::optional<double> magLimit, double snrMin, const gdg::Log& log );

   /// Cone search over every file (the PJSR _searchGaia parameters: magnitudeLow -5,
   /// magnitudeHigh = the limit, no source limit, no flags), then local_gaia_distances.
   /// bp_rp = magBP - magRP, NaN when the NoGBPMag or NoGRPMag flag is set.
   gdg::Catalog distances_for( const gdg::Footprint& footprint ) override;

   /// The raw search rows (in file order, then the database's own order).
   std::vector<LocalGaiaStar> Search( const gdg::Footprint& footprint ) const;

   static double BPRP( const LocalGaiaStar& s );

private:

   StringList            m_paths;
   std::optional<double> m_magLimit;
   double                m_snrMin;
   gdg::Log              m_log;
};

/// queryLocalGaiaDR3's error, verbatim: no local Gaia DR3 database is configured.
extern const char* const kNoLocalGaiaMessage;

/// The Gaia database files to use: the module setting GaiaDatabaseFiles (one path per line) when
/// set; otherwise the DR3, then DR3SP, files configured for PixInsight's Gaia process in
/// ~/.PixInsight/core-NNN-pxi.settings (the first settings file, in name order, that has any;
/// port of discoverGaiaDbPaths). Throws pcl::Error(kNoLocalGaiaMessage) when none.
StringList DiscoverGaiaDatabases();

} // namespace pcl

#endif // __GDG_LocalGaia_h
