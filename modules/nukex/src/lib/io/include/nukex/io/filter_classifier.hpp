#ifndef NUKEX_IO_FILTER_CLASSIFIER_HPP
#define NUKEX_IO_FILTER_CLASSIFIER_HPP

#include "nukex/core/filter.hpp"
#include "nukex/io/filter_alias.hpp"

#include <string>

namespace nukex {

struct FrameMetadata;

class FilterClassifier {
public:
    FilterClassifier() = default;

    Filter classify(const FrameMetadata& meta);

    /// Names the user has taught NukeX, consulted only after the built-in
    /// table so an entry can add a spelling but never redefine a shipped one.
    /// FITS FILTER values are whatever the capture software wrote, so no
    /// shipped table can enumerate them.
    void set_aliases(FilterAliasStore aliases) { aliases_ = std::move(aliases); }

    const std::string& last_warning() const { return last_warning_; }

private:
    std::string      last_warning_;
    FilterAliasStore aliases_;

    static std::string normalize_name(const std::string& raw);
    static FilterClass lookup_known(const std::string& normalized,
                                    std::string& canonical_name_out,
                                    BandwidthSpec& bandwidth_out);
};

} // namespace nukex

#endif
