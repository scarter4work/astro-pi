"""The Executor seam.

The loop never touches pixels. It emits an Action; an Executor realizes it.
Production uses real PixInsight processes; tests use a numpy approximation. Loop
logic is identical under both, which is what lets the SS7 guardrails and SS6.3
convergence be covered by fast offline tests (SS3.1 requires the sidecar run
standalone with no PI present).
"""

from __future__ import annotations

from typing import Protocol

import numpy as np

from .actions import Action


class Executor(Protocol):
    def apply(
        self, rgb: np.ndarray, action: Action, *, pixel_scale_arcsec: float
    ) -> np.ndarray:
        """Return a new image with ``action`` applied. Must not mutate ``rgb``."""
        ...
