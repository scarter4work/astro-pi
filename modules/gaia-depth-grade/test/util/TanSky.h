// TanSky: astropy's linear TAN WCS (wcslib lin + tan + sph) as a gdg::SkyTransform, for tests
// and the in-PI probes (test tooling, PCL-free, never shipped).
//
// It follows wcslib 8 (the library astropy.wcs wraps): linp2x/linx2p (CDELT diagonal: x/cdelt;
// CD or rotated: the inverse matrix), tanx2s/tans2x with r0 = 180/pi, and sphx2s/sphs2x with
// euler angles (CRVAL1, 90 - CRVAL2, LONPOLE). Pixels are astropy origin-0 (wcslib pixel - 1),
// which is the core frame when the header describes a FITS (bottom-up) raster.
//
// Only the zenithal TAN case PixInsight and the goldens use: PV1_1 = 0, PV1_2 = 90 (phi0, theta0
// at the native pole). LATPOLE is irrelevant for a zenithal projection.
#pragma once

#include "core/Sky.h"

#include <cmath>
#include <map>
#include <stdexcept>
#include <string>

namespace gdg::test {

class TanSky : public gdg::SkyTransform {
public:
    /// From CRPIX (1-based, FITS), CRVAL (deg), the pixel-to-intermediate matrix (CD, or
    /// diag(CDELT) * PC) and LONPOLE (wcslib default 180 for a TAN reference below the pole).
    TanSky(double crpix1, double crpix2, double crval1, double crval2, double m11, double m12, double m21,
           double m22, double lonpole = 180.0)
        : crpix1_(crpix1), crpix2_(crpix2), crval1_(crval1), m11_(m11), m12_(m12), m21_(m21), m22_(m22) {
        diagonal_ = m12 == 0.0 && m21 == 0.0;
        const double det = m11 * m22 - m12 * m21;
        if (det == 0.0) throw std::invalid_argument("TanSky: singular pixel-to-world matrix");
        i11_ = m22 / det;
        i12_ = -m12 / det;
        i21_ = -m21 / det;
        i22_ = m11 / det;
        eul0_ = crval1;
        eul1_ = 90.0 - crval2;
        eul2_ = lonpole;
        eul3_ = cosd(eul1_);
        eul4_ = sind(eul1_);
    }

    /// From FITS keywords (name -> numeric value): CRPIX1/2, CRVAL1/2, and CD1_1..CD2_2 or
    /// CDELT1/2 (+ optional PC). Requires CTYPE TAN to have been checked by the caller.
    static TanSky from_keywords(const std::map<std::string, double>& k) {
        auto get = [&](const char* n) {
            auto it = k.find(n);
            if (it == k.end()) throw std::invalid_argument(std::string("TanSky: keyword missing: ") + n);
            return it->second;
        };
        auto opt = [&](const char* n, double d) {
            auto it = k.find(n);
            return it == k.end() ? d : it->second;
        };
        if (opt("PV1_1", 0.0) != 0.0 || opt("PV1_2", 90.0) != 90.0)
            throw std::invalid_argument("TanSky: only phi0 = 0, theta0 = 90 is supported");
        double m11, m12, m21, m22;
        if (k.count("CD1_1")) {
            m11 = get("CD1_1");
            m12 = opt("CD1_2", 0.0);
            m21 = opt("CD2_1", 0.0);
            m22 = get("CD2_2");
        } else {
            const double c1 = get("CDELT1"), c2 = get("CDELT2");
            m11 = c1 * opt("PC1_1", 1.0);
            m12 = c1 * opt("PC1_2", 0.0);
            m21 = c2 * opt("PC2_1", 0.0);
            m22 = c2 * opt("PC2_2", 1.0);
        }
        const double crval2 = get("CRVAL2");
        // wcslib: LONPOLE defaults to 180 when delta0 < theta0 (= 90 for TAN), else 0.
        const double lonpole = opt("LONPOLE", crval2 < 90.0 ? 180.0 : 0.0);
        return TanSky(get("CRPIX1"), get("CRPIX2"), get("CRVAL1"), crval2, m11, m12, m21, m22, lonpole);
    }

    bool pixel_to_sky(double x, double y, double& ra, double& dec) const override {
        // linp2x (astropy origin 0 -> wcslib 1-based pixel)
        const double d1 = (x + 1.0) - crpix1_, d2 = (y + 1.0) - crpix2_;
        double X, Y;
        if (diagonal_) {
            X = m11_ * d1;
            Y = m22_ * d2;
        } else {
            X = m11_ * d1 + m12_ * d2;
            Y = m21_ * d1 + m22_ * d2;
        }
        // tanx2s
        const double r = std::sqrt(X * X + Y * Y);
        const double phi = r == 0.0 ? 0.0 : atan2d(X, -Y);
        const double theta = atan2d(kR2D, r);
        // sphx2s
        const double dphi = phi - eul2_;
        const double sinthe = sind(theta), costhe = cosd(theta);
        const double costhe3 = costhe * eul3_, costhe4 = costhe * eul4_;
        const double sinthe3 = sinthe * eul3_, sinthe4 = sinthe * eul4_;
        const double sinphi = sind(dphi), cosphi = cosd(dphi);
        double xx = sinthe4 - costhe3 * cosphi;
        if (std::fabs(xx) < 1.0e-5) xx = -cosd(theta + eul1_) + costhe3 * (1.0 - cosphi);
        const double yy = -costhe * sinphi;
        double dlng;
        if (xx != 0.0 || yy != 0.0)
            dlng = atan2d(yy, xx);
        else
            dlng = eul1_ < 90.0 ? dphi + 180.0 : -dphi;
        double lng = eul0_ + dlng;
        if (eul0_ >= 0.0) {
            if (lng < 0.0) lng += 360.0;
        } else {
            if (lng > 0.0) lng -= 360.0;
        }
        if (lng > 360.0)
            lng -= 360.0;
        else if (lng < -360.0)
            lng += 360.0;
        const double z = sinthe3 + costhe4 * cosphi;
        const double lat = std::fabs(z) > 0.99 ? std::copysign(acosd(std::sqrt(xx * xx + yy * yy)), z) : asind(z);
        // astropy's SkyCoord longitude wraps to [0, 360).
        if (lng < 0.0) lng += 360.0;
        if (lng >= 360.0) lng -= 360.0;
        ra = lng;
        dec = lat;
        return std::isfinite(ra) && std::isfinite(dec);
    }

    bool sky_to_pixel(double ra, double dec, double& x, double& y) const override {
        // sphs2x
        const double dlng = ra - eul0_;
        const double sinlat = sind(dec), coslat = cosd(dec);
        const double coslat3 = coslat * eul3_, coslat4 = coslat * eul4_;
        const double sinlat3 = sinlat * eul3_, sinlat4 = sinlat * eul4_;
        const double sinlng = sind(dlng), coslng = cosd(dlng);
        double xx = sinlat4 - coslat3 * coslng;
        if (std::fabs(xx) < 1.0e-5) xx = -cosd(dec + eul1_) + coslat3 * (1.0 - coslng);
        const double yy = -coslat * sinlng;
        double dphi;
        if (xx != 0.0 || yy != 0.0)
            dphi = atan2d(yy, xx);
        else
            dphi = eul1_ < 90.0 ? dlng - 180.0 : -dlng;
        double phi = std::fmod(eul2_ + dphi, 360.0);
        if (phi > 180.0)
            phi -= 360.0;
        else if (phi < -180.0)
            phi += 360.0;
        const double z = sinlat3 + coslat4 * coslng;
        const double theta = std::fabs(z) > 0.99 ? std::copysign(acosd(std::sqrt(xx * xx + yy * yy)), z) : asind(z);
        // tans2x (bounds checking on: the far hemisphere does not project)
        const double s = sind(theta);
        if (s <= 0.0) return false;
        const double r = kR2D * cosd(theta) / s;
        const double X = r * sind(phi), Y = -r * cosd(phi);
        // linx2p
        double p1, p2;
        if (diagonal_) {
            p1 = X / m11_ + crpix1_;
            p2 = Y / m22_ + crpix2_;
        } else {
            p1 = i11_ * X + i12_ * Y + crpix1_;
            p2 = i21_ * X + i22_ * Y + crpix2_;
        }
        x = p1 - 1.0;
        y = p2 - 1.0;
        return std::isfinite(x) && std::isfinite(y);
    }

private:
    static constexpr double kPI = 3.141592653589793238462643;
    static constexpr double kD2R = kPI / 180.0;
    static constexpr double kR2D = 180.0 / kPI;

    // wcslib wcstrig.c: exact values at multiples of 90 degrees.
    static double cosd(double a) {
        if (std::fmod(a, 90.0) == 0.0) {
            const int i = std::abs(static_cast<int>(std::floor(a / 90.0 + 0.5))) % 4;
            switch (i) {
            case 0: return 1.0;
            case 1: return 0.0;
            case 2: return -1.0;
            case 3: return 0.0;
            }
        }
        return std::cos(a * kD2R);
    }
    static double sind(double a) {
        if (std::fmod(a, 90.0) == 0.0) {
            int i = static_cast<int>(std::floor(a / 90.0 - 0.5)) % 4;
            if (i < 0) i += 4;
            switch (i) {
            case 0: return 1.0;
            case 1: return 0.0;
            case 2: return -1.0;
            case 3: return 0.0;
            }
        }
        return std::sin(a * kD2R);
    }
    static double asind(double v) {
        if (v <= -1.0) {
            if (v + 1.0 > -1e-10) return -90.0;
        } else if (v == 0.0) {
            return 0.0;
        } else if (v >= 1.0) {
            if (v - 1.0 < 1e-10) return 90.0;
        }
        return std::asin(v) * kR2D;
    }
    static double acosd(double v) {
        if (v >= 1.0) {
            if (v - 1.0 < 1e-10) return 0.0;
        } else if (v == 0.0) {
            return 90.0;
        } else if (v <= -1.0) {
            if (v + 1.0 > -1e-10) return 180.0;
        }
        return std::acos(v) * kR2D;
    }
    static double atan2d(double y, double x) {
        if (y == 0.0) {
            if (x >= 0.0) return 0.0;
            if (x < 0.0) return 180.0;
        } else if (x == 0.0) {
            if (y > 0.0) return 90.0;
            if (y < 0.0) return -90.0;
        }
        return std::atan2(y, x) * kR2D;
    }

    double crpix1_, crpix2_, crval1_;
    double m11_, m12_, m21_, m22_;
    double i11_, i12_, i21_, i22_;
    bool diagonal_;
    double eul0_, eul1_, eul2_, eul3_, eul4_;
};

}  // namespace gdg::test
