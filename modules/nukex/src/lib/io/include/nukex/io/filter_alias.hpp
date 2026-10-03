#ifndef NUKEX_IO_FILTER_ALIAS_HPP
#define NUKEX_IO_FILTER_ALIAS_HPP

#include <initializer_list>
#include <map>
#include <set>
#include <string>

namespace nukex {

/// The emission lines NukeX can solve for on a colour sensor.
///
/// Named QSolveLine rather than EmissionLine because the QE database already
/// has a struct by that name holding a measured passband; this is the much
/// smaller idea of which of the three solvable lines a filter covers.
///
/// Hb is absent deliberately. At 486 nm it falls on the same blue and green
/// photosites as OIII at 501 nm, so a Q matrix carrying both columns is
/// rank-deficient and the decomposition has no unique answer. A quad-band
/// filter that passes Hb is described here by its other three lines.
enum class QSolveLine { Ha, OIII, SII };

/// The canonical filter whose calibrated entry covers exactly `lines`, or an
/// empty string when the database has none.
///
/// Empty is a real answer, not a failure to look hard enough: Ha and SII
/// without OIII has no calibrated entry, and inventing one would feed made-up
/// passbands into a Q-solve. Callers must present that as "NukeX cannot do
/// this combination" rather than accepting it and failing later.
std::string canonical_for_lines(const std::set<QSolveLine>& lines);

/// Convenience for call sites that build the set inline.
std::string canonical_for_lines(std::initializer_list<QSolveLine> lines);

/// Filter names a user has taught NukeX, keyed by normalised header name.
///
/// FITS `FILTER` values are whatever the capture software wrote, so the set of
/// spellings cannot be enumerated in advance. When NukeX does not recognise
/// one it asks which lines the filter passes and records the answer here, so
/// the next stack of the same data runs without asking again.
///
/// Consulted AFTER the built-in table, so an entry can add a name but never
/// shadow a shipped one.
class FilterAliasStore {
public:
    /// Load from `path`. A missing file is success with nothing in it -- that
    /// is simply the first run. Returns false only when a file exists and
    /// cannot be parsed, in which case the store is left empty so a
    /// hand-edited file cannot take a stack down with it.
    bool load(const std::string& path);

    /// Write the store to `path`, creating parent directories as needed.
    bool save(const std::string& path) const;

    /// Canonical filter name for `raw_filter_name`, or empty if not taught.
    /// Normalises its argument, so callers pass the raw header value.
    std::string lookup(const std::string& raw_filter_name) const;

    /// Teach one name. `raw_filter_name` is normalised; `canonical` must be a
    /// name the database carries, i.e. the result of canonical_for_lines().
    void set(const std::string& raw_filter_name, const std::string& canonical);

    bool empty() const { return aliases_.empty(); }
    std::size_t size() const { return aliases_.size(); }

    /// Lowercase alphanumerics only -- the same reduction FilterClassifier
    /// applies, so "L-QEF", "l qef" and "Lqef" are one key.
    static std::string normalize(const std::string& raw);

private:
    std::map<std::string, std::string> aliases_;
};

} // namespace nukex

#endif
