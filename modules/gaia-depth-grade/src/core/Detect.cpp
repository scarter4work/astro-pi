#include "Detect.h"

#include "DaoFind.h"
#include "Numerics.h"
#include "SigmaClip.h"

#include <stdexcept>

namespace gdg {

Image luminance(const Image& img) {
    if (img.channels == 1) return img;
    Image out(img.width, img.height, 1);
    const size_t C = static_cast<size_t>(img.channels);
    for (size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = pairwise_sum(img.data.data() + i * C, C) / static_cast<double>(C);
    return out;
}

std::vector<StarRow> detect_stars(const Image& lum, double fwhm, double threshold_sigma) {
    if (lum.channels != 1) throw std::invalid_argument("detect_stars: luminance (mono) image required");
    const ClipStats st = sigma_clipped_stats(lum.data, 3.0);
    Image sub = lum;
    for (double& v : sub.data) v -= st.median;
    DaoParams p;
    p.fwhm = fwhm;
    p.threshold = threshold_sigma * st.std;
    std::vector<StarRow> out;
    for (const DaoStar& s : dao_find(sub, p)) out.push_back(StarRow{s.x, s.y, s.flux});
    return out;
}

}  // namespace gdg
