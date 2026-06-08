#ifndef NUKEX_COMPOSE_PALETTE_HPP
#define NUKEX_COMPOSE_PALETTE_HPP

namespace nukex {

enum class EmissionLineId { Ha, OIII, SII };

struct LabColor {
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

class Palette {
public:
    static LabColor for_line(EmissionLineId line);
};

} // namespace nukex

#endif
