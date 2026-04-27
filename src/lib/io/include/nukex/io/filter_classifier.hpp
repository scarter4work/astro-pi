#ifndef NUKEX_IO_FILTER_CLASSIFIER_HPP
#define NUKEX_IO_FILTER_CLASSIFIER_HPP

#include "nukex/io/filter.hpp"

#include <string>

namespace nukex {

struct FrameMetadata;

class FilterClassifier {
public:
    FilterClassifier() = default;

    Filter classify(const FrameMetadata& meta);

    const std::string& last_warning() const { return last_warning_; }

private:
    std::string last_warning_;

    static std::string normalize_name(const std::string& raw);
    static FilterClass lookup_known(const std::string& normalized,
                                    std::string& canonical_name_out,
                                    BandwidthSpec& bandwidth_out);
};

} // namespace nukex

#endif
