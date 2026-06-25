"""On-disk cache of the prepare stage, so the UI runs detect+Gaia+match once and
re-renders per gain change. Stores the detected/distance table as an npz and the
QA summary as JSON under a per-target cache directory.
"""
from __future__ import annotations

import json
import os

import numpy as np
from astropy.table import Table

_COLUMNS = ("x", "y", "flux", "r_med_geo", "r_lo_geo", "r_hi_geo")
_NPZ = "prep.npz"
_QA = "qa.json"


def save_prep(cache_dir: str, table: Table, qa: dict) -> None:
    os.makedirs(cache_dir, exist_ok=True)
    cols = {c: np.asarray(table[c], dtype=float) for c in _COLUMNS}
    np.savez(os.path.join(cache_dir, _NPZ), **cols)
    with open(os.path.join(cache_dir, _QA), "w") as fh:
        json.dump(qa, fh, indent=2)


def load_prep(cache_dir: str) -> tuple[Table, dict]:
    data = np.load(os.path.join(cache_dir, _NPZ))
    table = Table()
    for c in _COLUMNS:
        table[c] = data[c]
    with open(os.path.join(cache_dir, _QA)) as fh:
        qa = json.load(fh)
    return table, qa
