"""The recipe: an ordered, replayable audit log (SS12).

SS12 requires the output be reconstructible as a sequence of stock PixInsight
processes. The recipe IS that artifact -- it must be sufficient to reproduce the
result with AutoContrast absent from the machine. Auditability is the point.
"""

from __future__ import annotations

from dataclasses import dataclass

from .actions import Action

# SS6.2's action -> stock PixInsight process mapping.
PROCESS_FOR_KIND = {
    "local_contrast": "MultiscaleLinearTransform",
    "local_equalize": "LocalHistogramEqualization",
    "core_hdr": "HDRMultiscaleTransform",
    "tonal_reshape": "CurvesTransformation",
    "black_point": "HistogramTransformation",
    "chroma": "ColorSaturation",
    "background_neutralize": "BackgroundNeutralization",
    "star_split": "StarXTerminator",
}


@dataclass(frozen=True)
class Recipe:
    actions: tuple[Action, ...] = ()

    @classmethod
    def empty(cls) -> "Recipe":
        return cls(actions=())

    def extend(self, action: Action) -> "Recipe":
        return Recipe(actions=self.actions + (action,))

    @property
    def key(self) -> str:
        """Order-sensitive identity, used for beam distinctness."""
        return " | ".join(a.key for a in self.actions)

    @property
    def applied_kinds(self) -> frozenset[str]:
        return frozenset(a.kind for a in self.actions)

    def to_dict(self) -> dict:
        return {
            "actions": [
                {"kind": a.kind, "level": a.level,
                 "scale_arcsec": a.scale_arcsec, "params": a.params}
                for a in self.actions
            ]
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Recipe":
        return cls(actions=tuple(
            Action(kind=a["kind"], level=a["level"],
                   scale_arcsec=a["scale_arcsec"], params=a.get("params", {}))
            for a in d["actions"]
        ))

    def to_pixinsight_steps(self) -> list[dict]:
        """Render to stock PI process invocations -- the SS12 audit artifact."""
        steps = []
        for a in self.actions:
            if a.kind not in PROCESS_FOR_KIND:
                raise ValueError(
                    f"unmapped action kind '{a.kind}'; known kinds: "
                    f"{', '.join(sorted(PROCESS_FOR_KIND.keys()))}"
                )
            params = dict(a.params)
            params["strength"] = a.strength
            if a.scale_arcsec is not None:
                params["scale_arcsec"] = a.scale_arcsec
            steps.append({
                "process": PROCESS_FOR_KIND[a.kind],
                "action": a.key,
                "params": params,
            })
        return steps
