#include "Ionizer.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace gdg::recon {

IonizerTable ionizer_table(const std::vector<StarRow>& rows) {
    IonizerTable t;
    t.bp_rp.emplace();
    t.r_med_geo.emplace();
    for (const StarRow& r : rows) {
        t.x.push_back(r.x);
        t.y.push_back(r.y);
        t.flux.push_back(r.flux);
        t.bp_rp->push_back(r.bp_rp);
        t.r_med_geo->push_back(r.r_med);
    }
    return t;
}

Ionizer ionizer_from_xy(double x, double y, std::optional<double> distance_pc) {
    return Ionizer{x, y, distance_pc, "override"};
}

namespace {

/// ionizer._distance: finite r_med_geo, else None (also when the column is absent).
std::optional<double> distance(const IonizerTable& t, size_t i) {
    if (!t.r_med_geo) return std::nullopt;
    const double d = (*t.r_med_geo)[i];
    return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
}

}  // namespace

Ionizer pick_ionizer(const IonizerTable* table, int ny, int nx) {
    const Ionizer centre{nx / 2.0, ny / 2.0, std::nullopt, "field-center"};
    if (!table || table->size() == 0) return centre;
    const IonizerTable& t = *table;
    const size_t n = t.size();
    if (t.y.size() != n || t.flux.size() != n || (t.bp_rp && t.bp_rp->size() != n) ||
        (t.r_med_geo && t.r_med_geo->size() != n))
        throw std::invalid_argument("pick_ionizer: table columns differ in length");

    std::vector<unsigned char> ok(n);
    bool any_ok = false;
    for (size_t i = 0; i < n; ++i) {
        const double x = t.x[i], y = t.y[i], f = t.flux[i];
        ok[i] = std::isfinite(x) && std::isfinite(y) && std::isfinite(f) && f > 0 && x >= 0 && x < nx && y >= 0 &&
                y < ny;
        any_ok = any_ok || ok[i];
    }
    if (!any_ok) return centre;

    if (t.bp_rp) {
        const std::vector<double>& col = *t.bp_rp;
        bool any_coloured = false;
        double fmax = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < n; ++i)
            if (ok[i] && std::isfinite(col[i])) {
                any_coloured = true;
                fmax = std::max(fmax, t.flux[i]);
            }
        if (any_coloured) {
            // np.argmin over score (inf for the rest): the first minimum.
            size_t best = 0;
            double best_score = std::numeric_limits<double>::infinity();
            bool have = false;
            for (size_t i = 0; i < n; ++i) {
                const double s = (ok[i] && std::isfinite(col[i]))
                                     ? col[i] - IONIZER_FLUX_WEIGHT * std::log10(t.flux[i] / fmax)
                                     : std::numeric_limits<double>::infinity();
                if (!have || s < best_score) {
                    best = i;
                    best_score = s;
                    have = true;
                }
            }
            return Ionizer{t.x[best], t.y[best], distance(t, best), "gaia-blue"};
        }
    }

    // np.argmax(np.where(ok, flux, -inf)): the first maximum.
    size_t best = 0;
    double best_flux = -std::numeric_limits<double>::infinity();
    bool have = false;
    for (size_t i = 0; i < n; ++i) {
        const double f = ok[i] ? t.flux[i] : -std::numeric_limits<double>::infinity();
        if (!have || f > best_flux) {
            best = i;
            best_flux = f;
            have = true;
        }
    }
    return Ionizer{t.x[best], t.y[best], distance(t, best), "gaia-bright"};
}

}  // namespace gdg::recon
