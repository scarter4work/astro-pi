#include "Channels.h"

#include "core/Numerics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace gdg::recon {

namespace {

void require_mono(const Image& a, const char* what) {
    if (a.channels != 1) throw std::invalid_argument(std::string(what) + ": expected a (ny, nx) plane");
}

/// np.asarray(a, dtype=np.float32) of a mono plane.
Image to_f32(const Image& a) {
    Image out = a;
    for (double& v : out.data) v = f32(v);
    return out;
}

/// Channel c of an (ny, nx, C) image as a mono plane (numpy a[..., c]).
Image channel(const Image& a, int c) {
    Image out(a.width, a.height);
    for (size_t i = 0; i < a.pixels(); ++i) out.data[i] = a.data[i * a.channels + c];
    return out;
}

}  // namespace

ChannelSet from_mono_sho(const Image& ha, const Image& oiii, const Image& sii) {
    require_mono(ha, "from_mono_sho ha");
    require_mono(oiii, "from_mono_sho oiii");
    require_mono(sii, "from_mono_sho sii");
    validate_shapes({&ha, &oiii, &sii});
    return ChannelSet(to_f32(ha), to_f32(oiii), to_f32(sii), CONF_SHO, true);
}

ChannelSet from_duoband(const Image& d1_hao3, const Image& d2_s2o3) {
    require_mono(d1_hao3, "from_duoband d1");
    require_mono(d2_s2o3, "from_duoband d2");
    validate_shapes({&d1_hao3, &d2_s2o3});
    Image ha(d1_hao3.width, d1_hao3.height), oiii = ha, sii = ha;
    for (size_t i = 0; i < ha.data.size(); ++i) {
        // float32 throughout: np.minimum, the subtraction and np.maximum(., 0) on float32 arrays.
        const float d1 = static_cast<float>(d1_hao3.data[i]);
        const float d2 = static_cast<float>(d2_s2o3.data[i]);
        const float o = std::isnan(d1) || std::isnan(d2) ? NAN : std::min(d1, d2);  // np.minimum propagates NaN
        const float h = d1 - o, s = d2 - o;
        oiii.data[i] = o;
        ha.data[i] = std::isnan(h) ? h : std::max(h, 0.0f);
        sii.data[i] = std::isnan(s) ? s : std::max(s, 0.0f);
    }
    return ChannelSet(std::move(ha), std::move(oiii), std::move(sii), CONF_DUOBAND, true);
}

ChannelSet from_osc_rgb(const Image& rgb) {
    if (rgb.channels != 3)
        throw std::invalid_argument("expected (ny, nx, 3) RGB, got (" + std::to_string(rgb.height) + ", " +
                                    std::to_string(rgb.width) + ", " + std::to_string(rgb.channels) + ")");
    Image ha(rgb.width, rgb.height), oiii = ha;
    for (size_t i = 0; i < rgb.pixels(); ++i) {
        ha.data[i] = f32(rgb.data[3 * i]);
        // (0.5 * (G + B)) in float32: the sum and the product both round to float32.
        const float s = static_cast<float>(rgb.data[3 * i + 1]) + static_cast<float>(rgb.data[3 * i + 2]);
        oiii.data[i] = 0.5f * s;
    }
    return ChannelSet(std::move(ha), std::move(oiii), std::nullopt, CONF_OSC, true);
}

bool looks_linear(const Image& arr) {
    // numpy propagates NaN through percentile and median; NaN < 0.10 is False.
    for (double v : arr.data)
        if (std::isnan(v)) return false;
    const std::vector<double> p = percentiles(arr.data, {0.1, 99.9});
    const double lo = p[0], hi = p[1];
    if (hi - lo <= 0) return true;  // flat: nothing indicates a stretch
    const double frac = (median(arr.data) - lo) / (hi - lo);
    return frac < LINEAR_MEDIAN_FRAC_MAX;
}

ChannelSet extract_channels(const std::map<std::string, Image>& inputs, const std::string& mode) {
    static const std::map<std::string, std::vector<std::string>> keys = {
        {"sho", {"ha", "oiii", "sii"}}, {"duoband", {"d1", "d2"}}, {"osc", {"rgb"}}};
    const auto it = keys.find(mode);
    if (it == keys.end())
        throw std::invalid_argument("unknown mode '" + mode + "'; expected one of ['duoband', 'osc', 'sho']");
    std::vector<std::string> missing;
    for (const std::string& k : it->second)
        if (!inputs.count(k)) missing.push_back(k);
    if (!missing.empty()) {
        std::string list;
        for (size_t i = 0; i < missing.size(); ++i) list += (i ? ", '" : "'") + missing[i] + "'";
        throw std::invalid_argument("mode '" + mode + "' missing inputs: [" + list + "]");
    }
    std::vector<const Image*> arrays;
    for (const std::string& k : it->second) arrays.push_back(&inputs.at(k));
    if (arrays.size() == 3)
        validate_shapes({arrays[0], arrays[1], arrays[2]});  // hard error on misregistration
    else if (arrays.size() == 2)
        validate_shapes({arrays[0], arrays[1]});

    ChannelSet cs = mode == "sho"       ? from_mono_sho(*arrays[0], *arrays[1], *arrays[2])
                    : mode == "duoband" ? from_duoband(*arrays[0], *arrays[1])
                                        : from_osc_rgb(*arrays[0]);

    bool linear = true;
    if (mode == "osc") {
        for (int c = 0; c < 3 && linear; ++c) linear = looks_linear(channel(*arrays[0], c));
    } else {
        for (const Image* a : arrays) {
            if (!linear) break;  // Python's all() short-circuits too
            linear = looks_linear(*a);
        }
    }
    if (linear) return cs;
    return ChannelSet(std::move(cs.ha), std::move(cs.oiii), std::move(cs.sii), 0.0, false);
}

}  // namespace gdg::recon
