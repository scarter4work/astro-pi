#ifndef NUKEX_COMPOSE_COLOR_COMPOSER_HPP
#define NUKEX_COMPOSE_COLOR_COMPOSER_HPP

#include "nukex/compose/palette.hpp"

#include <cstdint>

namespace nukex {

// Slot values are the stack's own LINEAR intensities, nominally in [0, 1];
// the derived emission slots are a Q-solve of the raw channels and are not
// bounded above. The composer reads a luminance through the sRGB transfer
// function as if it were display-encoded (that is what makes the grey axis
// an identity), which is why linear narrowband data lands at L* of a few
// units: dark, but not colourless. Negative values are treated as absence.
struct DerivedSlots {
    // Broadband channels (any may be zero if not in batch)
    double L = 0.0;
    double R = 0.0;
    double G = 0.0;
    double B = 0.0;
    // Emission-line channels (any may be zero if not in batch)
    double Ha   = 0.0;
    double OIII = 0.0;
    double SII  = 0.0;
};

struct sRGBPixel {
    double r = 0.0, g = 0.0, b = 0.0;
};

struct ContinuumK {
    double k_Ha   = 0.0;
    double k_OIII = 0.0;
    double k_SII  = 0.0;
};

class ColorComposer {
public:
    enum class Mode { LAB_LCH_DEFAULT, CONTINUUM_SUBTRACT };

    ColorComposer() = default;

    void set_mode(Mode m)                                { mode_ = m; }
    void set_continuum_coefficients(const ContinuumK& k) { continuum_ = k; }

    sRGBPixel compose_pixel(const DerivedSlots& s);

    /// The two halves of compose_pixel, separately.
    ///
    /// compose_lab is everything before the gamut: L* from the composer's own
    /// luminance (luminance_of), chroma from the natural RGB plus the gated
    /// emission palette. map_to_srgb is the gamut walk and the conversion.
    /// compose_pixel(s) == map_to_srgb(compose_lab(s)), bit for bit.
    ///
    /// They are separate because of where the gamut walk happens. Real
    /// narrowband data is dark: an emission total of 0.05 puts L* at 3.6,
    /// where sRGB holds almost no chroma, and the palette is walked to grey
    /// BEFORE any stretch runs -- measured on M16, saturation 0.099 in the
    /// raw channels to 0.011 in the stretched output. The stretch route
    /// therefore takes compose_lab, replaces L* with the L* of the STRETCHED
    /// luminance, and maps once, there. Hue and chroma stay exactly the
    /// line-ratio values Lupton et al. 2004 require; only lightness moves.
    /// `gate_total`, when >= 0, replaces the pixel's own sky-subtracted
    /// emission total in the chroma gate. The caller passes a spatially
    /// SMOOTHED total: extended faint emission is real when its neighbourhood
    /// is, and a lone noise excursion is not. Judged per pixel, the gate cut
    /// the faint outskirts of M16 to grey at a hard edge; judged over a 7x7
    /// neighbourhood the outskirts keep their colour and the sky stays grey.
    LabColor  compose_lab(const DerivedSlots& s, double gate_total = -1.0);
    sRGBPixel map_to_srgb(const LabColor& lab);

    /// The composer's luminance for a slot tuple, in the slots' own units:
    /// native L, else rec709 of RGB, else the emission total capped at 1.
    static double luminance_of(const DerivedSlots& s);

    /// The L* the composer assigns to a luminance value in [0, 1].
    static double lab_L_from_luminance(double v);

    // Chroma gate. Normalising the emission chrominance by the total
    // emission weight makes hue a pure line ratio (Lupton et al. 2004,
    // PASP 116, 133), but it also means an arbitrarily faint pixel
    // normalises to the full palette vector. This gate ramps chrominance
    // linearly to zero below `snr_floor`, so noise is not rendered as
    // saturated colour -- the deliberate version of the behaviour the
    // missing division used to provide by accident.
    //
    // Two parameters, because the derived slots carry a sky pedestal: a
    // ramp from zero would leave the background almost fully saturated.
    // `background` is the total emission weight of blank sky and
    // `full_scale` the weight at which chrominance reaches full strength.
    // Set both from the stack's own measured statistics, never from a
    // constant. full_scale <= background disables the gate.
    void set_chroma_gate(double background, double full_scale) {
        gate_background_ = background;
        gate_full_scale_ = full_scale;
    }
    double gate_background() const { return gate_background_; }
    double gate_full_scale() const { return gate_full_scale_; }

    /// Sky level of each emission plane, subtracted before the line weights
    /// are formed. The derived planes carry the sky pedestal of the channels
    /// they were solved from, and on a faint stack that pedestal dwarfs the
    /// signal: measured on a 12-frame M16, Ha sky 0.0247 with the nebula only
    /// +0.0010 above it, OIII 0.0229 and +0.0002. Weighted RAW, every nebula
    /// pixel is a 53% Ha "mix" and the palette blend lands between the two
    /// entries -- magenta with the old palette, brown with the new. Weighted
    /// sky-subtracted, the same nebula is 87% Ha, which is what it is. Hue is
    /// a ratio of line FLUXES, not of pedestals. Default 0: no subtraction.
    void set_line_backgrounds(double ha_sky, double oiii_sky, double sii_sky) {
        sky_ha_ = ha_sky; sky_oiii_ = oiii_sky; sky_sii_ = sii_sky;
    }
    double line_background_ha()   const { return sky_ha_; }
    double line_background_oiii() const { return sky_oiii_; }
    double line_background_sii()  const { return sky_sii_; }

    // Test seam: gamut handling is a pure function of its arguments.
    bool clip_to_gamut_for_test(double& r, double& g, double& b) {
        return clip_to_gamut(r, g, b);
    }

    // Stats
    std::int64_t gamut_clipped_count() const { return gamut_clipped_; }

    // Diagnostics for tests
    double last_pixel_emission_a() const { return last_emission_a_; }
    double last_pixel_emission_b() const { return last_emission_b_; }
    double last_pixel_chroma_scale() const { return last_chroma_scale_; }

    /// How far chroma had to be pulled toward the neutral axis to fit the
    /// sRGB gamut on the last pixel. 1.0 means it fitted as computed.
    double last_pixel_gamut_chroma_scale() const { return last_gamut_chroma_scale_; }

private:
    Mode         mode_      = Mode::LAB_LCH_DEFAULT;
    ContinuumK   continuum_ = {};
    std::int64_t gamut_clipped_ = 0;
    double       last_emission_a_ = 0.0;
    double       last_emission_b_ = 0.0;
    double       gate_background_ = 0.0;
    double       gate_full_scale_ = 0.0;
    double       sky_ha_ = 0.0, sky_oiii_ = 0.0, sky_sii_ = 0.0;
    double       last_chroma_scale_ = 0.0;
    double       last_gamut_chroma_scale_ = 1.0;

    static LabColor    rgb_to_lab(double r, double g, double b);
    static sRGBPixel   lab_to_srgb(const LabColor& lab);
    static double      signal_weight(double v);
    bool               clip_to_gamut(double& r, double& g, double& b);
    static bool        out_of_gamut(const sRGBPixel& p);
};

} // namespace nukex

#endif
