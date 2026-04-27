# QE Curves Extracted From Manufacturer Plots

All values read directly from official QE curve images. Where curves are normalized
(relative response), peak QE from spec is applied.

## ASI585MC (Sony IMX585, OSC)
Source: https://i.zwoastro.com/wp-content/uploads/2025/03/9c83877bc4a07c999895f9bfe470b8fb-1.jpg
Curve is absolute QE in %. Three colored channels: R/G/B. Bayer pattern RGGB.
For the (Gr,Gb) photosites, both green pixels are read as the same green curve value.

| Wavelength | R   | G (Gr=Gb) | B   |
|-----------:|----:|----------:|----:|
| 486 nm     | 0.03| 0.65      | 0.70|
| 501 nm     | 0.03| 0.85      | 0.50|
| 656 nm     | 0.73| 0.32      | 0.03|
| 672 nm     | 0.71| 0.33      | 0.05|

Peak QE: 90% (Green at 525nm), 91% per ZWO product page.

## ASI585MM (Sony IMX585, mono)
Same sensor but no Bayer matrix. Mono QE = ~Bayer green-channel envelope but slightly higher
because Bayer absorbs additional ~5-10% in dye stack.
QHYCCD published values for the IMX585 mono variant in the miniCAM8M:
- Hβ (486 nm): ~85% (interpolated; not explicitly given)
- OIII (501 nm): 91.2% (explicit)
- Hα (656 nm): 80.9% (explicit)
- SII (672 nm): 78.7% (explicit, given as 671.6 nm)
Source: https://www.qhyccd.com/quantum-efficiency-performance-of-the-imx585-sensor-in-the-minicam8/

## QHY268M (Sony IMX571 mono)
Source: https://www.qhyccd.com/wp-content/uploads/20220505684.jpg
Curve is absolute response, normalized 0-1 (peak ≈0.92 = 92%).
- 486 nm: 0.89 → 0.89 absolute
- 501 nm: 0.90 → 0.90 absolute (matches "at least 87%" reported)
- 656 nm: 0.50 → 0.50 absolute
- 672 nm: 0.46 → 0.46 absolute
Peak QE: ~92%.

## ASI294MC (Sony IMX294)
Source: https://astronomy-imaging-camera.com/wp-content/uploads/294QE-1024x627.jpg
Curve is RELATIVE response (peak = 1.0). Peak absolute QE per ZWO/Sony: ~75%.
Curve only goes 400-700 nm so 672 nm is interpolated.

| Wavelength | R    | G    | B    |  R(abs) | G(abs) | B(abs) |
|-----------:|-----:|-----:|-----:|--------:|-------:|-------:|
| 486 nm     | 0.05 | 0.55 | 0.74 | 0.04    | 0.41   | 0.55   |
| 501 nm     | 0.06 | 0.85 | 0.51 | 0.045   | 0.64   | 0.38   |
| 656 nm     | 0.78 | 0.04 | 0.03 | 0.59    | 0.03   | 0.02   |
| 672 nm     | 0.74 | 0.05 | 0.04 | 0.56    | 0.04   | 0.03   |

(Note: ASI294 has 4-corner color filter pattern that's QGGR-like; treating as RGGB
is common but downstream tooling should be aware.)

Confidence: medium — relative chart with abs scaling factor, manufacturer-derived peak.
