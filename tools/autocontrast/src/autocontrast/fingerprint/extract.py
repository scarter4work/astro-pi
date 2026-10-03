"""Top-level fingerprint extraction and the serializable record (§4.1).

``extract()`` turns an in-memory sRGB image (plus the metadata the loader
supplies) into a :class:`FingerprintData` — the stats block of the §4.1 record.
It contains no image data (§4). WCS and provenance are attached by the ingest
layer (Phase 1); this module owns only the measured statistics.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .color import rgb_to_lab
from .energy import EnergySpectrum, energy_spectrum_from_planes
from .metrics import (
    background_channel_balance,
    chroma_histogram,
    floor_to_peak_ratio,
    saturation_quantiles,
    structure_to_gradient_ratio_from_transform,
    tonal_quantiles,
)
from .starlet import starlet_transform

CHROMA_BINS = 16
CHROMA_EXTENT = 100.0


@dataclass
class FingerprintData:
    """The measured statistics block of a fingerprint record (§4.1).

    Arrays are numpy in memory; :meth:`to_dict` renders them to plain lists so the
    whole record is JSON-serializable with no numpy types leaking through.
    """

    energy: EnergySpectrum
    tonal_quantiles: np.ndarray
    saturation_quantiles: np.ndarray
    chroma_hist: np.ndarray
    background: dict
    pixel_scale_arcsec: float
    psf_fwhm_arcsec: float
    palette_class: str

    def to_dict(self) -> dict:
        return {
            "energy_spectrum": {
                "centers_arcsec": self.energy.centers_arcsec.tolist(),
                "energy": self.energy.energy.tolist(),
            },
            "tonal_quantiles": self.tonal_quantiles.tolist(),
            "saturation_quantiles": self.saturation_quantiles.tolist(),
            "chroma_hist": self.chroma_hist.tolist(),
            "background": {
                "floor_to_peak_ratio": self.background["floor_to_peak_ratio"],
                "structure_to_gradient_ratio": self.background["structure_to_gradient_ratio"],
                "channel_balance": list(self.background["channel_balance"]),
            },
            "pixel_scale_arcsec": self.pixel_scale_arcsec,
            "psf_fwhm_arcsec": self.psf_fwhm_arcsec,
            "palette_class": self.palette_class,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "FingerprintData":
        bg = d["background"]
        return cls(
            energy=EnergySpectrum(
                centers_arcsec=np.asarray(d["energy_spectrum"]["centers_arcsec"]),
                energy=np.asarray(d["energy_spectrum"]["energy"]),
            ),
            tonal_quantiles=np.asarray(d["tonal_quantiles"]),
            saturation_quantiles=np.asarray(d["saturation_quantiles"]),
            chroma_hist=np.asarray(d["chroma_hist"]),
            background={
                "floor_to_peak_ratio": bg["floor_to_peak_ratio"],
                "structure_to_gradient_ratio": bg["structure_to_gradient_ratio"],
                "channel_balance": np.asarray(bg["channel_balance"]),
            },
            pixel_scale_arcsec=d["pixel_scale_arcsec"],
            psf_fwhm_arcsec=d["psf_fwhm_arcsec"],
            palette_class=d["palette_class"],
        )


def extract(
    rgb: np.ndarray,
    *,
    pixel_scale_arcsec: float,
    n_scales: int,
    psf_fwhm_arcsec: float,
    palette_class: str,
) -> FingerprintData:
    """Fingerprint an sRGB image in [0, 1].

    Luminance-based components (energy, tonal, structure) use CIELAB L*; color
    components use a*/b* and C*; channel balance uses the raw RGB background.
    """
    if rgb.ndim != 3 or rgb.shape[-1] != 3:
        raise ValueError(f"extract expects an (H, W, 3) sRGB image, got {rgb.shape}")

    lab = rgb_to_lab(rgb)
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    chroma = np.hypot(a, b)

    # ONE starlet decomposition, used twice. The energy spectrum (§4.2) and the
    # structure/gradient ratio are both functions of the starlet transform of L*
    # at `n_scales` -- the same array, the same depth, the same deterministic
    # transform. Computing it once per component cost 0.574s of a 1.505s
    # extraction at the 1600px search proxy, purely to rebuild an array we had
    # just thrown away, and `extract` is called once per candidate in a loop that
    # runs up to 27 of them per iteration.
    #
    # This is a pure sharing of an intermediate: the two components consume the
    # planes exactly as before and nothing about the metric, the scale
    # convention (plane i -> 2**i * pixel_scale_arcsec) or the arithmetic
    # changes. Bit-exactness is PROVEN, not assumed --
    # `tests/test_fingerprint_cost.py` recomputes the pre-optimization
    # composition from the still-public single-image entry points and asserts
    # exact equality on real cached renders. It has to be exact: every threshold
    # in this project (the 0.075/0.15 magnitude buckets, epsilon_improve,
    # epsilon, the §10 exit criterion) is calibrated against fingerprint values,
    # and a drift here would invalidate all of them without failing anything.
    planes, residual = starlet_transform(L, n_scales=n_scales)

    return FingerprintData(
        energy=energy_spectrum_from_planes(planes, pixel_scale_arcsec),
        tonal_quantiles=tonal_quantiles(L),
        saturation_quantiles=saturation_quantiles(chroma),
        chroma_hist=chroma_histogram(a, b, bins=CHROMA_BINS, extent=CHROMA_EXTENT),
        background={
            "floor_to_peak_ratio": floor_to_peak_ratio(L),
            "structure_to_gradient_ratio": structure_to_gradient_ratio_from_transform(
                planes, residual
            ),
            "channel_balance": background_channel_balance(rgb),
        },
        pixel_scale_arcsec=pixel_scale_arcsec,
        psf_fwhm_arcsec=psf_fwhm_arcsec,
        palette_class=palette_class,
    )
