"""The reference record: a fingerprint plus its sky position, palette, and
provenance (design §4.1). Contains no image data.
"""

from __future__ import annotations

from dataclasses import dataclass

from autocontrast.fingerprint.extract import FingerprintData

# §5.3 provenance taxonomy. tool_output is rejected at ingest (autophagy guard);
# user_render is stored but excluded from consensus by default; professional_render
# is consensus-eligible.
SOURCE_TYPES = frozenset({"professional_render", "user_render", "tool_output"})
CONSENSUS_ELIGIBLE = frozenset({"professional_render"})


@dataclass
class ReferenceRecord:
    """A stored reference: fingerprint statistics + queryable position/palette +
    provenance. ``fov_radius_arcmin`` is the angular radius of the footprint used
    for cone-search overlap (§5.1)."""

    id: str
    ra_deg: float
    dec_deg: float
    fov_radius_arcmin: float
    palette_class: str
    source_type: str
    fingerprint: FingerprintData
    provenance: dict

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "ra_deg": self.ra_deg,
            "dec_deg": self.dec_deg,
            "fov_radius_arcmin": self.fov_radius_arcmin,
            "palette_class": self.palette_class,
            "source_type": self.source_type,
            "fingerprint": self.fingerprint.to_dict(),
            "provenance": self.provenance,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "ReferenceRecord":
        return cls(
            id=d["id"],
            ra_deg=d["ra_deg"],
            dec_deg=d["dec_deg"],
            fov_radius_arcmin=d["fov_radius_arcmin"],
            palette_class=d["palette_class"],
            source_type=d["source_type"],
            fingerprint=FingerprintData.from_dict(d["fingerprint"]),
            provenance=d["provenance"],
        )
