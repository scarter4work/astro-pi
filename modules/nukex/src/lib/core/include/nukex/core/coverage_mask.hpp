#ifndef NUKEX_CORE_COVERAGE_MASK_HPP
#define NUKEX_CORE_COVERAGE_MASK_HPP

#include <cstdint>
#include <vector>

namespace nukex {

/// Which output pixels of a warped frame actually received source data.
///
/// `warp` leaves 0 outside the source's coverage, and 0 is a legal pixel
/// value, so nothing downstream can tell a measurement of darkness from an
/// absence of data. The stacker therefore averaged absent samples in as
/// though they were black. Measured on a 53-frame M3 stack: 268,802
/// exactly-zero pixels, a rim at 48.6% of interior brightness, and
/// contamination reaching 48 px deep -- the session's dither and drift
/// excursion.
///
/// The mask is per CHANNEL, not per frame, because channel registration can
/// push one colour plane off the source while the others stay on it.
///
/// Storage is one bit per pixel per channel: a 6072x4042 three-channel frame
/// costs 9.2 MB here against 294 MB for a byte-per-pixel mask, and this
/// allocation sits beside a voxel cube already measured in gigabytes.
class CoverageMask {
public:
    CoverageMask() = default;

    CoverageMask(int width, int height, int n_channels)
        : w_(width), h_(height), nc_(n_channels),
          bits_((static_cast<std::size_t>(width) * height * n_channels + 7) / 8, 0) {}

    bool covered(int c, int y, int x) const {
        const std::size_t i = index(c, y, x);
        return (bits_[i >> 3] >> (i & 7)) & 1u;
    }

    void set_covered(int c, int y, int x, bool value) {
        const std::size_t i = index(c, y, x);
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (i & 7));
        if (value) bits_[i >> 3] |= bit;
        else       bits_[i >> 3] &= static_cast<std::uint8_t>(~bit);
    }

    /// True when no mask was supplied. Callers treat this as "everything is
    /// covered", which is what preserves behaviour for paths that do not
    /// warp at all (the reference frame reaches the accumulator unwarped).
    bool empty() const { return nc_ == 0; }

    int width() const    { return w_; }
    int height() const   { return h_; }
    int channels() const { return nc_; }

    std::int64_t covered_count(int c) const {
        std::int64_t n = 0;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (covered(c, y, x)) ++n;
        return n;
    }

private:
    std::size_t index(int c, int y, int x) const {
        return (static_cast<std::size_t>(c) * h_ + y) * w_ + x;
    }

    int w_ = 0, h_ = 0, nc_ = 0;
    std::vector<std::uint8_t> bits_;
};

} // namespace nukex

#endif
