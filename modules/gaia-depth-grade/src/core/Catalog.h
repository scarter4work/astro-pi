// A distance catalogue: the rows a CatalogSource returns for a footprint (the Python
// distances._REQUIRED columns plus the optional bp_rp).
#pragma once

#include "Footprint.h"

#include <cmath>
#include <vector>

namespace gdg {

struct CatalogRow {
    double ra = 0.0, dec = 0.0;                  // degrees (ICRS)
    double r_med = NAN, r_lo = NAN, r_hi = NAN;  // distances in pc (r_med_geo, r_lo_geo, r_hi_geo)
    long long source_id = 0;
    double bp_rp = NAN;                          // Gaia BP-RP colour; NaN when unknown
};

using Catalog = std::vector<CatalogRow>;

class CatalogSource {
public:
    virtual ~CatalogSource() = default;
    virtual Catalog distances_for(const Footprint& footprint) = 0;
};

}  // namespace gdg
