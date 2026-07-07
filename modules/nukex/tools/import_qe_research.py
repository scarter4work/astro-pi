#!/usr/bin/env python3
"""
Transform research/qe_database_research.json into the shipping
share/qe_database.json consumed by the C++ loader
(src/lib/calibration/src/qe_database.cpp).

Responsibilities:
  1. Read the research JSON.
  2. Drop the `_meta` block (research-only metadata).
  3. Resolve `qe_inherits_from_sensor: true` markers by copying the QE
     block from the corresponding entry in the research `sensors` map,
     then drop the marker (and the `sensors` map itself -- it is not
     part of the shipping schema).
  4. Normalise photosite keys inside each per-wavelength QE block so
     they are EXACTLY the tokens accepted by the C++ loader's
     `parse_photosite_key()` -- see PHOTOSITE KEY CONTRACT below.
  5. Normalise filter `passes` (research schema) into `lines` (shipping
     schema) -- see FILTER LINE CONTRACT below.
  6. Validate the result against the shipping schema.
  7. Write share/qe_database.json (schema_version 1, sorted, indented).

This is a one-shot script (run by the dev when refreshing the shipped
DB); it lives in tools/ so the transform is reproducible and testable,
not because it runs in CI.

PHOTOSITE KEY CONTRACT (source of truth: parse_photosite_key() in
src/lib/calibration/src/qe_database.cpp):

    "R"          -> Photosite::R
    "G"          -> Photosite::G
    "B"          -> Photosite::B
    "Gr" / "Gb"  -> Photosite::G   (both map to the SAME enum value)
    anything else -> Photosite::MONO_PEAK   (silent catch-all!)

Two consequences that drive the normalisation below:

  * nlohmann::json objects in this codebase use the default (sorted)
    `json`, not `ordered_json`. If a wavelength entry has both "Gr"
    and "Gb" keys, the loader's std::map<Photosite,double> insertion
    order is alphabetical ("Gb" before "Gr"), so "Gb" is silently
    overwritten by "Gr" and its value is lost. To avoid depending on
    that iteration-order footgun, this script pre-averages Gr/Gb into
    a single explicit "G" key (mean of the two) before shipping.

  * Because *any* unrecognised key silently becomes MONO_PEAK, the
    research data's two synonyms for the mono/no-CFA photosite --
    "mono" and "mono_pk" -- are NOT passed through as free-form
    strings. They are explicitly whitelisted and normalised to one
    canonical token, `mono_pk`. Any OTHER key we don't recognise is a
    hard error, not a silent MONO_PEAK fallback (a typo in the
    research JSON must not silently masquerade as valid QE data in
    the shipped DB).

  Accepted input photosite keys: R, B, G, Gr, Gb, mono, mono_pk.
  Emitted output photosite keys: R, B, G, mono_pk (only).

FILTER LINE CONTRACT (source of truth: the `filters` block in
src/lib/calibration/src/qe_database.cpp, and Task 3 fixtures):

    filters.<name> = { "type": str, "lines": [ {"name": str,
                        "wavelength_nm": float, "fwhm_nm": float} ] }

The research JSON instead stores each filter's transmission peaks as
`passes`: [{"center_nm": float, "fwhm_nm": float,
"peak_transmission": float, ["passband_nm": ...]}]. This script maps
`passes` -> `lines`, `center_nm` -> `wavelength_nm`, drops
`peak_transmission` / `passband_nm` (not part of the shipping
schema), and synthesises a human-readable `name` for each line: a
small lookup table names the well-known narrowband astro lines
(H-alpha, OIII, H-beta, SII) when the wavelength is within 0.3 nm of
the canonical value, and everything else (broadband RGB/LPR/
luminance passbands, and near-but-not-quite narrowband lines that
this script declines to guess the identity of) falls back to a
"<wavelength>nm" label. `name` is documentation only -- it is not
matched against anything elsewhere in the codebase.

If a filter entry already uses the shipping `lines` key directly
(e.g. in unit-test fixtures), it is passed through as-is.
"""

import argparse
import json
import sys

REQUIRED_CAMERA_FIELDS = ("sensor", "type", "confidence")

# Photosite keys the research JSON may use, and how each is handled.
DIRECT_PHOTOSITE_KEYS = ("R", "B")          # passed through unchanged
GREEN_SPLIT_KEYS = ("Gr", "Gb")             # averaged into a single "G"
MONO_KEY_ALIASES = ("mono", "mono_pk")      # normalised to one canonical key
CANONICAL_MONO_KEY = "mono_pk"
KNOWN_PHOTOSITE_KEYS = frozenset(
    ("G",) + DIRECT_PHOTOSITE_KEYS + GREEN_SPLIT_KEYS + MONO_KEY_ALIASES
)

# Well-known narrowband astro emission lines, used only to synthesise a
# readable `name` for filter passbands. Tolerance is deliberately tight
# (0.3 nm) so we never mislabel a filter that happens to sit nearby but
# is not actually centred on the line (e.g. tri/quad-band NII-inclusive
# passes near 657-658 nm are left as numeric labels rather than guessed).
KNOWN_LINES_NM = {
    486.1: "Hbeta",
    500.7: "OIII",
    656.3: "Halpha",
    672.4: "SII",
}
LINE_NAME_TOLERANCE_NM = 0.3


def _line_name_for_wavelength(wavelength_nm):
    for known_wl, name in KNOWN_LINES_NM.items():
        if abs(wavelength_nm - known_wl) <= LINE_NAME_TOLERANCE_NM:
            return name
    return f"{wavelength_nm:g}nm"


def normalise_qe_block(qe, camera_name):
    """Rewrite a research `qe` block ({wavelength: {site: value}}) into
    the shipping form using only the photosite tokens the C++ loader's
    parse_photosite_key() explicitly recognises (R, G, B, mono_pk).

    Raises ValueError on unrecognised photosite keys, ambiguous mono
    aliasing (both "mono" and "mono_pk" present for one wavelength), or
    QE values outside the physically valid [0, 1] range.
    """
    out = {}
    for wl, sites in qe.items():
        unknown = set(sites) - KNOWN_PHOTOSITE_KEYS
        if unknown:
            raise ValueError(
                f"Camera '{camera_name}' wavelength {wl!r} has unrecognised "
                f"photosite key(s) {sorted(unknown)}; expected one of "
                f"{sorted(KNOWN_PHOTOSITE_KEYS)}"
            )

        s = {}

        for key in DIRECT_PHOTOSITE_KEYS:
            if key in sites:
                s[key] = sites[key]

        if "G" in sites:
            s["G"] = sites["G"]
        else:
            gr = sites.get("Gr")
            gb = sites.get("Gb")
            if gr is not None and gb is not None:
                s["G"] = (gr + gb) / 2.0
            elif gr is not None:
                s["G"] = gr
            elif gb is not None:
                s["G"] = gb

        mono_present = [k for k in MONO_KEY_ALIASES if k in sites]
        if len(mono_present) > 1:
            raise ValueError(
                f"Camera '{camera_name}' wavelength {wl!r} has both "
                f"{mono_present} for the mono photosite; ambiguous"
            )
        if mono_present:
            s[CANONICAL_MONO_KEY] = sites[mono_present[0]]

        for site_key, value in s.items():
            if not isinstance(value, (int, float)) or not (0.0 <= value <= 1.0):
                raise ValueError(
                    f"Camera '{camera_name}' wavelength {wl!r} site "
                    f"'{site_key}' has out-of-range QE value {value!r} "
                    f"(expected a fraction in [0, 1])"
                )

        if not s:
            raise ValueError(
                f"Camera '{camera_name}' wavelength {wl!r} has no usable "
                f"photosite values after normalisation"
            )

        out[wl] = s
    return out


def resolve_camera(name, cam, sensors):
    """Resolve qe_inherits_from_sensor by copying the sensor's qe block,
    then normalise photosite keys. Returns a dict containing only the
    shipping-schema camera fields (sensor, type, bayer, confidence, qe).
    """
    out = {}
    for field in ("sensor", "type", "confidence"):
        if field in cam:
            out[field] = cam[field]

    bayer = cam.get("bayer", cam.get("bayer_pattern"))
    if bayer:
        out["bayer"] = bayer

    if cam.get("qe_inherits_from_sensor"):
        sensor_name = cam.get("sensor")
        sensor = sensors.get(sensor_name)
        if sensor is None or "qe" not in sensor:
            raise ValueError(
                f"Camera '{name}' inherits from sensor '{sensor_name}' but "
                f"that sensor (with a 'qe' block) was not found in research"
            )
        qe = sensor["qe"]
    else:
        qe = cam.get("qe")

    if qe is not None:
        out["qe"] = normalise_qe_block(qe, name)

    return out


def validate_camera(name, cam):
    missing = [f for f in REQUIRED_CAMERA_FIELDS if f not in cam]
    if missing:
        raise ValueError(f"Camera '{name}' missing required fields: {missing}")
    if "qe" not in cam or not cam["qe"]:
        raise ValueError(
            f"Camera '{name}' missing 'qe' block (after inheritance resolution)"
        )


def resolve_filter(name, filt):
    """Return a dict containing only the shipping-schema filter fields
    (type, lines). Converts research `passes` into shipping `lines` if
    the filter doesn't already use `lines` directly.
    """
    out = {}
    if "type" in filt:
        out["type"] = filt["type"]

    if "lines" in filt:
        out["lines"] = filt["lines"]
    elif "passes" in filt:
        lines = []
        for p in filt["passes"]:
            wavelength_nm = p.get("wavelength_nm", p.get("center_nm"))
            fwhm_nm = p.get("fwhm_nm")
            if wavelength_nm is None or fwhm_nm is None:
                raise ValueError(
                    f"Filter '{name}' has a pass missing "
                    f"center_nm/wavelength_nm or fwhm_nm: {p!r}"
                )
            lines.append({
                "name": _line_name_for_wavelength(wavelength_nm),
                "wavelength_nm": wavelength_nm,
                "fwhm_nm": fwhm_nm,
            })
        out["lines"] = lines

    return out


def validate_filter(name, filt):
    if "type" not in filt:
        raise ValueError(f"Filter '{name}' missing required field: 'type'")
    lines = filt.get("lines")
    if not lines:
        raise ValueError(f"Filter '{name}' missing non-empty 'lines'")
    for i, line in enumerate(lines):
        missing = [f for f in ("name", "wavelength_nm", "fwhm_nm") if f not in line]
        if missing:
            raise ValueError(
                f"Filter '{name}' line[{i}] missing required fields: {missing}"
            )


def transform(research):
    sensors = research.get("sensors", {})

    out_cameras = {}
    for name, cam in research.get("cameras", {}).items():
        resolved = resolve_camera(name, cam, sensors)
        validate_camera(name, resolved)
        out_cameras[name] = resolved

    out_filters = {}
    for name, filt in research.get("filters", {}).items():
        resolved = resolve_filter(name, filt)
        validate_filter(name, resolved)
        out_filters[name] = resolved

    return {
        "schema_version": 1,
        "cameras": out_cameras,
        "filters": out_filters,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="path to research/qe_database_research.json")
    ap.add_argument("output", help="path to share/qe_database.json")
    args = ap.parse_args(argv)

    with open(args.input, "r") as f:
        research = json.load(f)

    try:
        shipped = transform(research)
    except ValueError as e:
        print(f"Validation error: {e}", file=sys.stderr)
        return 1

    with open(args.output, "w") as f:
        json.dump(shipped, f, indent=2, sort_keys=True)
        f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
