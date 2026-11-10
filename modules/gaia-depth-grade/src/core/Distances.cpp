// Port of gaia_depth_grade/local_gaia.py (parallax_to_distance_table, LocalGaiaSource) and of the
// PJSR discoverGaiaDbPaths / _extractGaiaDbPaths (gaia_depth_grade_lib.jsh).
#include "Distances.h"

#include "Format.h"
#include "Numerics.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace gdg {

namespace {

// Median Gaia DR3 parallax uncertainty (mas) vs G, verbatim from local_gaia.py.
const std::vector<double> kGKnots{6.0, 13.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0, 21.0};
const std::vector<double> kSigmaKnots{0.02, 0.02, 0.025, 0.035, 0.05, 0.08, 0.15, 0.30, 0.65};

std::string xml_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            static const std::pair<const char*, char> ent[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool done = false;
            for (const auto& e : ent) {
                const size_t n = std::char_traits<char>::length(e.first);
                if (s.compare(i, n, e.first) == 0) {
                    out += e.second;
                    i += n;
                    done = true;
                    break;
                }
            }
            if (done) continue;
        }
        out += s[i++];
    }
    return out;
}

std::vector<std::string> extract(const std::string& txt, const std::string& key) {
    // _extractGaiaDbPaths: key + '\d+"\s+t="s">([^<]+\.xpsd)', anchored on k=" so that e.g.
    // EDR3DatabaseFilePathNN is not read as DR3DatabaseFilePathNN.
    const std::regex re("k=\"" + key + "\\d+\"\\s+t=\"s\">([^<]+\\.xpsd)<");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(txt.begin(), txt.end(), re); it != std::sregex_iterator(); ++it)
        out.push_back(xml_unescape((*it)[1].str()));
    return out;
}

}  // namespace

double sigma_parallax_from_g(double g) { return interp({g}, kGKnots, kSigmaKnots)[0]; }

std::vector<double> sigma_parallax_from_g(const std::vector<double>& g) { return interp(g, kGKnots, kSigmaKnots); }

Catalog parallax_to_distance_table(const std::vector<double>& ra, const std::vector<double>& dec,
                                   const std::vector<double>& parallax_mas, const std::vector<double>& g_mag,
                                   const std::vector<double>& bp_rp, double snr_min, const Log& log) {
    const size_t n = ra.size();
    if (dec.size() != n || parallax_mas.size() != n || g_mag.size() != n || (!bp_rp.empty() && bp_rp.size() != n))
        throw std::invalid_argument("parallax_to_distance_table: columns differ in length");
    const std::vector<double> sigma = sigma_parallax_from_g(g_mag);
    std::vector<char> keep(n);
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        const double plx = parallax_mas[i];
        const double snr = plx / sigma[i];
        keep[i] = std::isfinite(plx) && plx > 0 && std::isfinite(snr) && snr >= snr_min;
        kept += keep[i] ? 1 : 0;
    }
    const size_t n_drop = n - kept;
    if (n_drop)
        log_warning(log, strformat("local Gaia: dropped %d/%d sources failing the parallax S/N >= %.1f cut "
                                   "(non-positive or low-S/N parallax \xE2\x80\x94 distance unreliable). Kept %d.",
                                   static_cast<int>(n_drop), static_cast<int>(n), snr_min, static_cast<int>(kept)));
    Catalog t;
    t.reserve(kept);
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        const double plx = parallax_mas[i], s = sigma[i];
        CatalogRow r;
        r.ra = ra[i];
        r.dec = dec[i];
        r.r_med = 1000.0 / plx;
        // Distance bounds from the parallax +/- sigma interval; np.clip(plx - sigma, 1e-6, None).
        const double plx_hi = plx + s;
        const double plx_lo = std::max(plx - s, 1e-6);
        r.r_lo = 1000.0 / plx_hi;
        r.r_hi = 1000.0 / plx_lo;
        r.source_id = static_cast<long long>(t.size());
        r.bp_rp = bp_rp.empty() ? NAN : bp_rp[i];
        t.push_back(r);
    }
    return t;
}

Catalog local_gaia_distances(const std::vector<double>& ra, const std::vector<double>& dec,
                             const std::vector<double>& parallax_mas, const std::vector<double>& g_mag,
                             const std::vector<double>& bp_rp, std::optional<double> mag_limit, double snr_min,
                             const Log& log) {
    const size_t n = ra.size();
    if (dec.size() != n || parallax_mas.size() != n || g_mag.size() != n || (!bp_rp.empty() && bp_rp.size() != n))
        throw std::invalid_argument("local_gaia_distances: columns differ in length");
    std::vector<double> r2, d2, p2, g2, b2;
    if (mag_limit) {
        size_t excluded = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!(g_mag[i] < *mag_limit)) {  // NaN G is excluded, as numpy's g < limit is False
                ++excluded;
                continue;
            }
            r2.push_back(ra[i]);
            d2.push_back(dec[i]);
            p2.push_back(parallax_mas[i]);
            g2.push_back(g_mag[i]);
            b2.push_back(bp_rp.empty() ? NAN : bp_rp[i]);
        }
        if (excluded)
            log_warning(log, strformat("local Gaia: magnitude cut G<%.1f excludes %d source(s).", *mag_limit,
                                       static_cast<int>(excluded)));
    } else {
        r2 = ra;
        d2 = dec;
        p2 = parallax_mas;
        g2 = g_mag;
        b2 = bp_rp.empty() ? std::vector<double>(n, NAN) : bp_rp;
    }
    Catalog t = parallax_to_distance_table(r2, d2, p2, g2, b2, snr_min, log);
    if (t.empty())
        throw std::runtime_error("local Gaia produced zero usable sources after the parallax/magnitude cuts for this field");
    log_info(log, strformat("local Gaia: %d usable sources (parallax distances)", static_cast<int>(t.size())));
    return t;
}

std::vector<std::string> gaia_db_paths_from_settings(const std::string& settings_text) {
    // Prefer base DR3 (the full astrometric catalogue) over the DR3SP subset.
    std::vector<std::string> dr3 = extract(settings_text, "DR3DatabaseFilePath");
    if (!dr3.empty()) return dr3;
    return extract(settings_text, "DR3SPDatabaseFilePath");
}

}  // namespace gdg
