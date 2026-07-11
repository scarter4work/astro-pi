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
from .energy import EnergySpectrum, energy_spectrum
from .metrics import (
    background_channel_balance,
    chroma_histogram,
    floor_to_peak_ratio,
    saturation_quantiles,
    structure_to_gradient_ratio,
    tonal_quantiles,
)

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

    return FingerprintData(
        energy=energy_spectrum(L, pixel_scale_arcsec=pixel_scale_arcsec, n_scales=n_scales),
        tonal_quantiles=tonal_quantiles(L),
        saturation_quantiles=saturation_quantiles(chroma),
        chroma_hist=chroma_histogram(a, b, bins=CHROMA_BINS, extent=CHROMA_EXTENT),
        background={
            "floor_to_peak_ratio": floor_to_peak_ratio(L),
            "structure_to_gradient_ratio": structure_to_gradient_ratio(L, n_scales=n_scales),
            "channel_balance": background_channel_balance(rgb),
        },
        pixel_scale_arcsec=pixel_scale_arcsec,
        psf_fwhm_arcsec=psf_fwhm_arcsec,
        palette_class=palette_class,
    )
