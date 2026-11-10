#include "Ionization.h"

#include "core/Numerics.h"

#include <stdexcept>

namespace gdg::recon {

IonizationMap ionization_ratio(const ChannelSet& ch, double eps) {
    if (ch.ha.channels != 1 || ch.oiii.channels != 1)
        throw std::invalid_argument("ionization_ratio: ha and oiii must be (ny, nx) planes");
    const size_t n = ch.ha.data.size();
    Image oiii(ch.ha.width, ch.ha.height), total = oiii;
    for (size_t i = 0; i < n; ++i) {
        const double h = std::max(nan_to_num(ch.ha.data[i]), 0.0);
        const double o = std::max(nan_to_num(ch.oiii.data[i]), 0.0);
        oiii.data[i] = o;
        total.data[i] = o + h;  // numpy: oiii + ha
    }
    const Image num = gaussian_filter(oiii, IONIZATION_SMOOTH_SIGMA_PX);
    const Image den = gaussian_filter(total, IONIZATION_SMOOTH_SIGMA_PX);
    IonizationMap m{Image(ch.ha.width, ch.ha.height), ch.ratio_confidence};
    for (size_t i = 0; i < n; ++i) m.oiii_fraction.data[i] = f32(clip(num.data[i] / (den.data[i] + eps), 0.0, 1.0));
    return m;
}

}  // namespace gdg::recon
