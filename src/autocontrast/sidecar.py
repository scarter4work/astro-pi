"""The sidecar file-protocol service (design §3.1).

PixInsight (PJSR) drives the Python sidecar without any network dependency: it
writes a request JSON, runs

    <venv>/bin/python -m autocontrast.sidecar request.json response.json

and reads the response JSON. This is the default, reliable transport (§3.1); it
avoids blocking the PI event loop and needs no NetworkTransfer POST support.

The sidecar stays headless-importable so the offline harness (§10, Phase 0) can
use the same code with no PixInsight present.

Response contract:
    {"ok": true,  "op": <op>, "result": {...}}
    {"ok": false, "op": <op>, "error": "<message>"}
Errors are always surfaced, never silently swallowed (§12).
"""

from __future__ import annotations

import json
import sys
import traceback
from pathlib import Path

from autocontrast import __version__
from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract
from autocontrast.io.loaders import load_raster


def _op_ping(_req: dict) -> dict:
    return {"pong": True, "version": __version__}


def _op_fingerprint(req: dict) -> dict:
    rgb = load_raster(req["image"], max_dim=req.get("max_dim", 1600))
    fp = extract(
        rgb,
        pixel_scale_arcsec=req["pixel_scale_arcsec"],
        n_scales=req.get("n_scales", 7),
        psf_fwhm_arcsec=req["psf_fwhm_arcsec"],
        palette_class=req["palette_class"],
    )
    return fp.to_dict()


def _op_distance(req: dict) -> dict:
    reference = FingerprintData.from_dict(req["reference"])
    target = FingerprintData.from_dict(req["target"])
    return {"distance": fingerprint_distance(reference, target)}


_OPS = {
    "ping": _op_ping,
    "fingerprint": _op_fingerprint,
    "distance": _op_distance,
}


def handle_request(request: dict) -> dict:
    """Dispatch a single request dict to a response dict. Never raises: failures
    are returned as ``{"ok": false, "error": ...}`` so the bridge always gets a
    well-formed response."""
    op = request.get("op")
    handler = _OPS.get(op)
    if handler is None:
        return {"ok": False, "op": op, "error": f"Unknown op {op!r}; expected {sorted(_OPS)}"}
    try:
        return {"ok": True, "op": op, "result": handler(request)}
    except Exception as exc:  # surface the real failure to PixInsight (§12)
        return {"ok": False, "op": op, "error": f"{type(exc).__name__}: {exc}",
                "traceback": traceback.format_exc()}


def main(argv: list[str] | None = None) -> int:
    """CLI entry: ``sidecar <request.json> <response.json>``. Returns 0 iff ok."""
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 2:
        sys.stderr.write("usage: python -m autocontrast.sidecar <request.json> <response.json>\n")
        return 2
    request_path, response_path = Path(argv[0]), Path(argv[1])
    request = json.loads(request_path.read_text())
    response = handle_request(request)
    response_path.write_text(json.dumps(response))
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
