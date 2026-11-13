#include "Reconstruct.h"

#include "Channels.h"
#include "Extinction.h"
#include "Ionization.h"

#include <cmath>

namespace gdg::recon {

Image channel_mean(const Image& img) {
    if (img.channels == 1) return img;
    Image m = luminance(img);
    for (double& v : m.data) v = f32(v);
    return m;
}

ReconResult reconstruct(const ReconInputs& in, const ReconConfig& cfg, const Log& log) {
    std::map<std::string, Image> inputs = in.channels;
    if (in.mode == "duoband")
        for (const char* key : {"d1", "d2"}) {
            auto it = inputs.find(key);
            if (it != inputs.end()) it->second = channel_mean(it->second);
        }
    const ChannelSet ch = extract_channels(inputs, in.mode);
    const int ny = ch.ha.height, nx = ch.ha.width;
    if (in.hb) validate_shapes({&ch.ha, &*in.hb});

    IonizationMap ion = ionization_ratio(ch);

    Image lum(nx, ny);
    for (size_t i = 0; i < lum.data.size(); ++i) {
        double v = f32(ch.ha.data[i] + ch.oiii.data[i]);
        if (ch.sii) v = f32(v + ch.sii->data[i]);
        lum.data[i] = v;
    }

    Ionizer ionizer = in.ionizer_override ? *in.ionizer_override : Ionizer{};
    if (!in.ionizer_override) {
        const IonizerTable table = ionizer_table(in.stars);
        ionizer = pick_ionizer(&table, ny, nx);
    }

    Image dust = dust_from_structure(lum);
    if (in.hb) {
        const Image balmer = dust_from_balmer(ch.ha, *in.hb);
        for (size_t i = 0; i < dust.data.size(); ++i) dust.data[i] = std::max(dust.data[i], balmer.data[i]);
    }

    std::optional<double> pc_per_px;
    if (ionizer.distance_pc && in.deg_per_px_at) {
        const std::optional<double> deg = in.deg_per_px_at(ionizer.x, ionizer.y);
        if (deg) pc_per_px = *ionizer.distance_pc * *deg * 3.14159265358979323846 / 180.0;
    }

    CavityModel model = reconstruct_cavity(ch, ion, ionizer, lum, dust, pc_per_px, cfg);

    std::string degraded;
    if (!ch.is_linear) {
        degraded = RECON_DEGRADED_STRETCHED;
        log_warning(log, RECON_STRETCHED_WARNING);
    }
    return ReconResult{std::move(model), std::move(ion), std::move(ionizer), std::move(lum), std::move(degraded)};
}

}  // namespace gdg::recon
