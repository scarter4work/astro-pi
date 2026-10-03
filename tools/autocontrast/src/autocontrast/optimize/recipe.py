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


def action_to_dict(a: Action) -> dict:
    """The ONE serialization of an Action.

    The §2.5 batched protocol has to hand an action to PixInsight and get a
    result back through a JSON file, and reconstruct the same Action to extend a
    recipe with. That is a second caller for what :meth:`Recipe.to_dict` was
    already doing inline; hoisted here so there is one shape, not two that must
    be kept in step by hand.
    """
    return {"kind": a.kind, "level": a.level, "scale_arcsec": a.scale_arcsec,
            "params": a.params}


def action_from_dict(d: dict) -> Action:
    return Action(kind=d["kind"], level=d["level"],
                  scale_arcsec=d["scale_arcsec"], params=d.get("params", {}))


def pixinsight_step(a: Action) -> dict:
    """Render one action to a stock PI process invocation (the SS12 artifact).

    The batched protocol's instructions carry ``process``/``params`` for
    PixInsight to execute; they must be the SAME rendering the audit log
    reports, or the recipe handed to the user would describe something other
    than what was run.
    """
    if a.kind not in PROCESS_FOR_KIND:
        raise ValueError(
            f"unmapped action kind '{a.kind}'; known kinds: "
            f"{', '.join(sorted(PROCESS_FOR_KIND.keys()))}"
        )
    params = dict(a.params)
    params["strength"] = a.strength
    if a.scale_arcsec is not None:
        params["scale_arcsec"] = a.scale_arcsec
    return {"process": PROCESS_FOR_KIND[a.kind], "action": a.key, "params": params}


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
        return {"actions": [action_to_dict(a) for a in self.actions]}

    @classmethod
    def from_dict(cls, d: dict) -> "Recipe":
        return cls(actions=tuple(action_from_dict(a) for a in d["actions"]))

    def to_pixinsight_steps(self) -> list[dict]:
        """Render to stock PI process invocations -- the SS12 audit artifact."""
        return [pixinsight_step(a) for a in self.actions]
