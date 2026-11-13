#include "Validate.h"

#include "core/Parallel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace gdg::recon {

namespace {

/// Shortest round-trip text of `v` (2.0 -> "2", -12.5 -> "-12.5", 1e300 -> "1e+300").
std::string num(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

}  // namespace

void require_finite_model(const CavityModel& model, const std::string& who) {
    const Image& z = model.z;
    const Image& d = model.dust;
    const Image& e = model.emissivity;
    const int nx = z.width, ny = z.height;
    struct Bad {
        int x = -1;
        const char* plane = "";
    };
    std::vector<Bad> bad(static_cast<size_t>(ny));
    gdg::parallel_for_dynamic(ny, std::max(1, 65536 / std::max(nx, 1)), [&](int y, int) {
        for (int x = 0; x < nx; ++x) {
            const char* plane = nullptr;
            if (!std::isfinite(z.at(x, y))) plane = "z";
            else if (!std::isfinite(d.at(x, y))) plane = "dust";
            else if (!(std::isfinite(e.at(x, y, 0)) && std::isfinite(e.at(x, y, 1)) && std::isfinite(e.at(x, y, 2))))
                plane = "emissivity";
            if (plane) {
                bad[static_cast<size_t>(y)] = Bad{x, plane};
                return;
            }
        }
    });
    for (int y = 0; y < ny; ++y) {
        const Bad& b = bad[static_cast<size_t>(y)];
        if (b.x >= 0)
            throw std::invalid_argument(who + ": the cavity model holds a non-finite " + b.plane + " value at (" +
                                        std::to_string(b.x) + ", " + std::to_string(y) + ")");
    }
}

void require_apex_in_grid(const CavityModel& model, const std::string& who) {
    const double ax = model.apex_xy.first, ay = model.apex_xy.second;
    if (!(std::isfinite(ax) && std::isfinite(ay))) throw std::invalid_argument(who + ": the apex must be finite");
    const double nx = model.z.width, ny = model.z.height;
    if (!(ax >= -nx && ax <= 2.0 * nx && ay >= -ny && ay <= 2.0 * ny))
        throw std::invalid_argument("apex (" + num(ax) + ", " + num(ay) + ") is outside the model grid (" +
                                    std::to_string(model.z.width) + "x" + std::to_string(model.z.height) +
                                    ") by more than its size");
}

}  // namespace gdg::recon
