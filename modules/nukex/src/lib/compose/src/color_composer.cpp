#include "nukex/compose/color_composer.hpp"

#include <algorithm>
#include <cmath>

namespace nukex {

namespace {

// sRGB transfer functions (IEC 61966-2-1)
double srgb_to_linear(double v) {
    return (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double v) {
    return (v <= 0.0031308) ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// D65 white point in XYZ
constexpr double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;

// CIE Lab f / f^-1
double f_lab(double t) {
    return (t > 0.008856) ? std::cbrt(t) : (7.787 * t + 16.0 / 116.0);
}
double f_lab_inv(double t) {
    double t3 = t * t * t;
    return (t3 > 0.008856) ? t3 : (t - 16.0 / 116.0) / 7.787;
}

void srgb_to_xyz(double r, double g, double b, double& X, double& Y, double& Z) {
    double rl = srgb_to_linear(r), gl = srgb_to_linear(g), bl = srgb_to_linear(b);
    X = 0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl;
    Y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
    Z = 0.0193339 * rl + 0.1191920 * gl + 0.9503041 * bl;
}
void xyz_to_srgb(double X, double Y, double Z, double& r, double& g, double& b) {
    double rl =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double gl = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double bl =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    // Don't clamp negatives here — let clip_to_gamut see them so the
    // gamut_clipped_ counter records under-saturation as well as over.
    r = linear_to_srgb(rl);
    g = linear_to_srgb(gl);
    b = linear_to_srgb(bl);
}

} // namespace

LabColor ColorComposer::rgb_to_lab(double r, double g, double b) {
    double X, Y, Z;
    srgb_to_xyz(r, g, b, X, Y, Z);
    double fx = f_lab(X / Xn), fy = f_lab(Y / Yn), fz = f_lab(Z / Zn);
    return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

sRGBPixel ColorComposer::lab_to_srgb(const LabColor& lab) {
    double fy = (lab.L + 16.0) / 116.0;
    double fx = fy + lab.a / 500.0;
    double fz = fy - lab.b / 200.0;
    double X = Xn * f_lab_inv(fx);
    double Y = Yn * f_lab_inv(fy);
    double Z = Zn * f_lab_inv(fz);
    sRGBPixel out;
    xyz_to_srgb(X, Y, Z, out.r, out.g, out.b);
    return out;
}

double ColorComposer::signal_weight(double v) {
    return std::max(0.0, v); // simple linear weight; clamp negatives
}

bool ColorComposer::out_of_gamut(const sRGBPixel& p) {
    const double eps = 1e-9;
    return p.r < -eps || p.g < -eps || p.b < -eps
        || p.r > 1.0 + eps || p.g > 1.0 + eps || p.b > 1.0 + eps;
}

bool ColorComposer::clip_to_gamut(double& r, double& g, double& b) {
    bool clipped = false;

    // Negative energy is not a hue, it is an absence: floor it first.
    if (r < 0.0) { r = 0.0; clipped = true; }
    if (g < 0.0) { g = 0.0; clipped = true; }
    if (b < 0.0) { b = 0.0; clipped = true; }

    // Lupton et al. 2004, PASP 116, 133: rescale all three channels by
    // their common maximum, so the intensity is clipped at unity while the
    // colour stays correct. Clamping each channel independently changes the
    // hue of every over-bright pixel, which on a narrowband stack is most
    // of the frame.
    const double m = std::max(r, std::max(g, b));
    if (m > 1.0) {
        const double inv = 1.0 / m;
        r *= inv; g *= inv; b *= inv;
        clipped = true;
    }
    return clipped;
}

double ColorComposer::luminance_of(const DerivedSlots& s) {
    // Native L, else rec709 from RGB, else the emission total.
    //
    // The third case is not a fallback, it is the narrowband case. A dual-NB
    // frame has no broadband slots at all -- Phase B fills Ha and OIII, and L,
    // R, G and B are all zero -- so without it every composed pixel has L* = 0
    // and the image is black. It did not LOOK black before only because the
    // chrominance was being clamped into the visible range channel by channel,
    // which is to say the picture was made entirely of a rounding artefact.
    //
    // Total line flux is the right quantity: it is what Rector, Levay,
    // Frattare et al. 2004 (AJ) add together when they stack colorized
    // narrowband layers, and it leaves hue -- the RATIO of those lines -- to
    // be carried separately, exactly as Lupton et al. 2004 require.
    const double emission_total = std::max(0.0, s.Ha)
                                + std::max(0.0, s.OIII)
                                + std::max(0.0, s.SII);
    const double rec709 = 0.299 * s.R + 0.587 * s.G + 0.114 * s.B;
    return (s.L > 0.0)   ? s.L
         : (rec709 > 0.0) ? rec709
                          : std::min(1.0, emission_total);
}

double ColorComposer::lab_L_from_luminance(double v) {
    // srgb_to_linear(v) -> Y -> L* via f_lab matches the natural L* of an
    // equal-RGB sRGB triplet exactly, which is what makes the grey axis an
    // identity through compose_pixel.
    const double linear_y = srgb_to_linear(v);
    return 116.0 * f_lab(linear_y) - 16.0;
}

LabColor ColorComposer::compose_lab(const DerivedSlots& s, double gate_total) {
    // 1) Luminance.
    const double L_broad = luminance_of(s);

    // 2) Continuum subtract (opt-in)
    double ha = s.Ha, oiii = s.OIII, sii = s.SII;
    if (mode_ == Mode::CONTINUUM_SUBTRACT) {
        ha   -= continuum_.k_Ha   * s.R;
        oiii -= continuum_.k_OIII * (s.G + s.B) * 0.5;
        sii  -= continuum_.k_SII  * s.R;
    }

    // 3) Natural Lab: chrominance from RGB when broadband present, then
    // override luminance with L_broad converted to CIE L* via the sRGB
    // transfer function. This preserves LRGBSHO design intent (dedicated
    // L drives luminance, RGB drives chrominance) AND yields identity
    // roundtrip on the gray axis.
    bool have_broadband = (s.R + s.G + s.B) > 0.0;
    LabColor lab_natural = have_broadband
        ? rgb_to_lab(s.R, s.G, s.B)
        : LabColor{ 0.0, 0.0, 0.0 };
    lab_natural.L = lab_L_from_luminance(L_broad);

    // 4) Emission contribution to chrominance, from line FLUXES: each plane's
    // sky level comes off first (see set_line_backgrounds). A pedestal is not
    // emission, and on a faint stack it is most of the number.
    double w_ha   = signal_weight(ha   - sky_ha_);
    double w_oiii = signal_weight(oiii - sky_oiii_);
    double w_sii  = signal_weight(sii  - sky_sii_);
    double total_w = w_ha + w_oiii + w_sii;

    LabColor pal_ha   = Palette::for_line(EmissionLineId::Ha);
    LabColor pal_oiii = Palette::for_line(EmissionLineId::OIII);
    LabColor pal_sii  = Palette::for_line(EmissionLineId::SII);

    double emission_a = 0.0, emission_b = 0.0, chroma_scale = 0.0;
    if (total_w > 0.0) {
        // Hue is the RATIO of the emission lines, never their absolute
        // strength. Lupton et al. 2004 (PASP 116, 133) state the rule this
        // once violated: under any non-linear mapping an object's colour
        // must not depend on its brightness. Brightness is carried by L
        // alone, which is how STScI colorizes a narrowband layer -- fixed
        // hue, fixed saturation, value carries the signal (Rector, Levay,
        // Frattare et al. 2004, AJ).
        const double inv_w = 1.0 / total_w;
        emission_a = (w_ha * pal_ha.a + w_oiii * pal_oiii.a + w_sii * pal_sii.a) * inv_w;
        emission_b = (w_ha * pal_ha.b + w_oiii * pal_oiii.b + w_sii * pal_sii.b) * inv_w;

        // Normalisation alone would hand a pixel at total_w = 1e-9 the full
        // palette vector. Ramp the chrominance down towards the measured
        // sky level so faint data desaturates on purpose rather than by
        // accident, and so blank sky is not painted a bold colour.
        if (gate_full_scale_ > gate_background_) {
            const double span = gate_full_scale_ - gate_background_;
            const double judged = (gate_total >= 0.0) ? gate_total : total_w;
            chroma_scale = std::min(1.0,
                std::max(0.0, (judged - gate_background_) / span));
        } else {
            chroma_scale = 1.0;
        }
        emission_a *= chroma_scale;
        emission_b *= chroma_scale;
    }
    last_emission_a_ = emission_a;
    last_emission_b_ = emission_b;
    last_chroma_scale_ = chroma_scale;

    return LabColor{
        lab_natural.L,
        lab_natural.a + emission_a,
        lab_natural.b + emission_b
    };
}

sRGBPixel ColorComposer::map_to_srgb(const LabColor& lab_final) {
    // 5) Convert to sRGB, reducing chroma until the colour fits.
    //
    // Clamping channels is what destroys hue, and it does so in BOTH
    // directions. Lupton et al. 2004 give the fix for a channel above 1
    // (rescale all three by their maximum), but the emission palette is
    // saturated enough -- a* of +50 to +60 -- that at low luminance the
    // conversion lands NEGATIVE in green, and flooring that to zero flattens
    // the hue just as badly.
    //
    // So gamut mapping happens in Lab, not sRGB: hold L and the hue angle,
    // and walk chroma toward the neutral axis until every channel is in
    // range. Scaling a and b together preserves the hue angle exactly, which
    // is the property the whole line-ratio argument depends on. And it
    // happens at the L* the VIEWER sees: for the stretched output that is the
    // stretched luminance, never the linear one (see compose_lab).
    sRGBPixel out = lab_to_srgb(lab_final);
    if (out_of_gamut(out)) {
        ++gamut_clipped_;

        double lo = 0.0, hi = 1.0;   // lo is always in gamut: chroma 0 is grey
        for (int i = 0; i < 16; ++i) {
            const double mid = 0.5 * (lo + hi);
            // Scale the FULL chrominance, natural plus emission, so the hue
            // angle of the composite is what is held.
            const LabColor scaled{ lab_final.L, lab_final.a * mid, lab_final.b * mid };
            if (out_of_gamut(lab_to_srgb(scaled))) hi = mid; else lo = mid;
        }
        const LabColor fitted{ lab_final.L, lab_final.a * lo, lab_final.b * lo };
        out = lab_to_srgb(fitted);
        last_gamut_chroma_scale_ = lo;

        // L itself can exceed the cube even at zero chroma (a pixel brighter
        // than white). Fall back to the Lupton rescale for that, which keeps
        // the colour and clips only the intensity.
        clip_to_gamut(out.r, out.g, out.b);
    } else {
        last_gamut_chroma_scale_ = 1.0;
    }
    return out;
}

sRGBPixel ColorComposer::compose_pixel(const DerivedSlots& s) {
    return map_to_srgb(compose_lab(s));
}

} // namespace nukex
