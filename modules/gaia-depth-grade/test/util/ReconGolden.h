// Helpers shared by the recon parity tests (test tooling, PCL-free).
#pragma once

#include "Golden.h"
#include "core/recon/Model.h"

#include <catch_amalgamated.hpp>

#include <string>

namespace gdg::test {

/// A boolean (H, W) golden as a recon::Mask.
inline gdg::recon::Mask golden_mask(const std::string& name) {
    const NpyArray a = golden(name);
    if (a.shape.size() != 2) throw std::runtime_error(name + ": not a 2-D array");
    gdg::recon::Mask m(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]));
    for (size_t i = 0; i < a.data.size(); ++i) m.data[i] = a.data[i] != 0.0 ? 1 : 0;
    return m;
}

/// Rows [y0, y1) x columns [x0, x1) of a mono image (numpy a[y0:y1, x0:x1]).
inline gdg::Image crop(const gdg::Image& in, int y0, int y1, int x0, int x1) {
    gdg::Image out(x1 - x0, y1 - y0, in.channels);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            for (int c = 0; c < in.channels; ++c) out.at(x - x0, y - y0, c) = in.at(x, y, c);
    return out;
}

/// The C++ output, already float32-valued, against a float32 golden: reports the worst |d| and
/// the count of elements that are not bit-identical, and checks both against the stated limits.
inline void check_golden_image(const gdg::Image& got, const std::string& name, double tol, size_t max_unequal) {
    const gdg::Image want = golden_image(name);
    REQUIRE(got.width == want.width);
    REQUIRE(got.height == want.height);
    REQUIRE(got.channels == want.channels);
    const double d = max_abs_diff(got.data, want.data);
    const size_t n = count_unequal(got.data, want.data);
    WARN(name << ": max |d| " << d << ", " << n << " of " << want.data.size() << " not bit-identical");
    CHECK(d <= tol);
    CHECK(n <= max_unequal);
}

}  // namespace gdg::test
