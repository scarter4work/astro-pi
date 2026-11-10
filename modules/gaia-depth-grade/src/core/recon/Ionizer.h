// Exciting-star (ionizer) identification: a port of recon/ionizer.py (PCL-free).
//
// The ionizer is the hot blue star that carves the HII cavity: its position anchors the cavity
// apex and its Gaia distance anchors absolute scale.
//
// Integration point (Phase D): the colour comes from the catalogue's Gaia BP-RP, carried by the
// cross-match into gdg::StarRow::bp_rp (NaN = no colour), and the distance from StarRow::r_med
// (Python's r_med_geo). ionizer_table(rows) is the bridge from the matched star table. Until the
// catalogue fills bp_rp, every pick falls back to "gaia-bright", exactly as Python does on an
// all-NaN or absent bp_rp column.
#pragma once

#include "Model.h"
#include "core/Detect.h"

#include <optional>
#include <vector>

namespace gdg::recon {

/// ionizer.py _FLUX_WEIGHT: score = bp_rp - 0.4 * log10(flux / flux_max), lower is better. A
/// star 10x fainter must be 0.4 mag bluer in BP-RP to win.
inline constexpr double IONIZER_FLUX_WEIGHT = 0.4;

/// The columns pick_ionizer reads from the Python star table. An absent optional is an absent
/// column (Python `"bp_rp" not in table.colnames`); every present column has x.size() entries.
struct IonizerTable {
    std::vector<double> x, y, flux;
    std::optional<std::vector<double>> bp_rp;
    std::optional<std::vector<double>> r_med_geo;
    size_t size() const { return x.size(); }
};

/// The matched star table as pick_ionizer's input: x, y, flux, bp_rp and r_med_geo (= r_med)
/// columns all present.
IonizerTable ionizer_table(const std::vector<StarRow>& rows);

/// ionizer_from_xy(x, y, distance_pc): source "override".
Ionizer ionizer_from_xy(double x, double y, std::optional<double> distance_pc);

/// pick_ionizer(table, (ny, nx), footprint). `table` nullptr is Python's None. A usable star has
/// finite x, y, flux, flux > 0 and 0 <= x < nx, 0 <= y < ny (the footprint cone is the image's
/// own sky area, so Python never reads its footprint argument; it is not ported). With a bp_rp
/// column and any usable star of finite colour: the lowest score wins (first on ties), source
/// "gaia-blue". Otherwise the brightest usable star (first on ties), "gaia-bright". No usable
/// star: (nx / 2, ny / 2), no distance, "field-center". distance_pc = finite r_med_geo, else
/// none. Throws std::invalid_argument when a column's length differs from x.
Ionizer pick_ionizer(const IonizerTable* table, int ny, int nx);

}  // namespace gdg::recon
