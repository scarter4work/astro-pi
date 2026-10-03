#!/usr/bin/env python3
"""Transform research/qe_database_research.json -> share/qe_database.json.

The research file is keyed the way a researcher thinks: product filter names
with `passes`, cameras that inherit QE from a `sensors` block, Gr/Gb/mono_pk
photosites. The engine is keyed the way FilterClassifier + QEDatabase think:
canonical filter names ("HaO3", "L-eXtreme", ...) with emission `lines`, one
resolved QE block per camera with R/G/B (OSC) or mono_pk (mono) photosites.
This script is the only bridge between the two. It is deterministic and
refuses to write a database the engine cannot consume.

Two research-data gaps are handled explicitly rather than silently: a mono
sensor's peak QE is sometimes keyed "mono" instead of "mono_pk" (vocabulary
drift, same quantity, aliased through); an OSC camera whose sensor was only
ever researched as a flat panchromatic curve, with no Bayer-channel split at
any wavelength, is excluded from the shipped file with a note rather than
invented or shipped broken (spec 6.3's generic_sony_imx_osc fallback covers
it at runtime, with a Process Console warning). A camera with a Bayer split
at SOME wavelengths but not others is different — inconsistent data, not a
missing measurement — and still fails loud.

Every QE block a camera consumes is validated before anything is derived
from it, and a violation is a TransformError (exit 1, nothing written), never
a silent drop or coercion: wavelength keys must be plain integers, photosite
keys must be ones this script knows, values must be numbers in [0, 1], the
mono/mono_pk alias may not appear twice, and the green photosites must be
unambiguous (see `green_qe`). Confidence must be a string the C++ loader's
parse_confidence() recognises.

Usage: import_qe_research.py <research.json> <shipped.json>
"""
import argparse
import json
import re
import statistics
import sys

SCHEMA_VERSION = 1
GENERIC_OSC_KEY = "generic_sony_imx_osc"

# Emission lines the engine solves for. Hb is intentionally absent from the
# canonical Q-solve set: at 486 nm it lands on the same B/G photosites as
# OIII (501 nm), so a Q matrix with both columns is rank-deficient on an
# RGB sensor and ChannelDecomposer would throw SingularQError.
LINES = {"Ha": 656.3, "OIII": 500.7, "SII": 672.4, "Hb": 486.1}
Q_SOLVE_LINES = ("Ha", "OIII", "SII")
LINE_TOL_NM = 4.0

# Canonical dual-NB keys emitted by FilterClassifier::known_table(), by the
# set of Q-solve lines their passes cover.
# A quad-band filter such as Optolong's L-Quad Enhance passes Hb as well, but
# Hb is not a Q-solve line, so what it contributes is the three-line set. That
# set is exactly determined on an RGB sensor -- three unknowns from three
# photosites -- and had no canonical name until 2026-09-04, which meant the
# classifier could not reach it and the batch stopped at start even though the
# QE data shipped.
CANONICAL_DUAL = {
    frozenset(("Ha", "OIII")):        "HaO3",
    frozenset(("SII", "OIII")):       "S2O3",
    frozenset(("Ha", "OIII", "SII")): "HaO3S2",
}

# Product entries that FilterClassifier maps to their own canonical name.
PRODUCT_CANONICAL = {
    "Optolong-LeXtreme-7nm":    "L-eXtreme",
    "Optolong-LeNhance":        "L-eNhance",
    "Optolong-LUltimate-3nm":   "L-Ultimate",
    "Antlia-ALP-T-Ha-OIII-5nm": "ALP-T",
}
REQUIRED_CANONICAL = tuple(CANONICAL_DUAL.values()) + tuple(PRODUCT_CANONICAL.values()) + Q_SOLVE_LINES

TYPE_MAP = {
    "narrowband-single": "NARROWBAND",
    "dual-narrowband": "DUAL_NB", "tri-narrowband": "DUAL_NB",
    "quad-narrowband": "DUAL_NB", "narrowband-multi": "DUAL_NB",
    "luminance": "BROADBAND", "broadband-RGB": "BROADBAND", "broadband-LPR": "BROADBAND",
}
BAYER_OK = {"RGGB", "BGGR", "GRBG", "GBRG"}
REQUIRED_CAMERA_FIELDS = ("sensor", "type", "confidence")

# Exactly the strings parse_confidence() in src/lib/calibration/src/
# qe_database.cpp recognises. Anything else -- "Medium", "hi", null -- the
# loader silently turns into QEConfidence::UNKNOWN, so it must fail here.
ALLOWED_CONFIDENCE = ("high", "medium", "low")

# The loader parses wavelength keys with an unguarded std::stoi(): "656.3"
# silently truncates to 656, " 656" and "0656" parse too but are distinct
# JSON keys that can collide, and "Ha" throws. Only a canonical positive
# integer string is accepted.
WAVELENGTH_KEY = re.compile(r"[1-9][0-9]*")

# Every photosite key the research file may use. OSC cameras consume R, B and
# the greens; mono cameras consume mono_pk (or its drift alias "mono"). A
# "both-variants" sensor legitimately carries both vocabularies, and each
# camera type ignores the other's keys -- but a key outside this set is a typo
# or an unknown quantity, and dropping it would ship the wrong number.
OSC_SITES = ("R", "G", "Gr", "Gb", "B")
MONO_SITES = ("mono_pk", "mono")
KNOWN_SITES = frozenset(OSC_SITES + MONO_SITES)


class TransformError(ValueError):
    pass


def nearest_line(center_nm):
    name, wl = min(LINES.items(), key=lambda kv: abs(kv[1] - center_nm))
    return name if abs(wl - center_nm) <= LINE_TOL_NM else None


def passes_to_lines(passes):
    """Research `passes` -> engine `lines`, keeping only passes on a known emission line."""
    out = []
    for p in passes:
        name = nearest_line(float(p["center_nm"]))
        if name is None:
            continue
        out.append({"name": name, "wavelength_nm": LINES[name], "fwhm_nm": float(p["fwhm_nm"])})
    return out


def q_solve_lines(lines):
    return [l for l in lines if l["name"] in Q_SOLVE_LINES]


def ordered_for_q_solve(lines):
    """Canonical dual-NB line order the classifier documents: Ha/SII before
    OIII, i.e. descending wavelength_nm. Applied to every dual-NB canonical
    entry, whether it's derived (HaO3, S2O3) or product-mapped (L-eXtreme,
    L-eNhance, L-Ultimate, ALP-T) -- source `passes` order is not guaranteed."""
    return sorted(lines, key=lambda l: -l["wavelength_nm"])


def median_lines(entries):
    """Same line set across several products -> one entry with median FWHM per line."""
    by_name = {}
    order = []
    for lines in entries:
        for l in lines:
            if l["name"] not in by_name:
                order.append(l["name"])
                by_name[l["name"]] = []
            by_name[l["name"]].append(l["fwhm_nm"])
    return [{"name": n, "wavelength_nm": LINES[n], "fwhm_nm": round(statistics.median(by_name[n]), 2)}
            for n in order]


def transform_filters(filters):
    out = {}
    dual_sources = {}    # canonical key -> [lines, ...]
    single_sources = {}  # line name -> [lines, ...]

    for name, f in filters.items():
        ftype = TYPE_MAP.get(f.get("type"))
        if ftype is None:
            raise TransformError(f"Filter '{name}': unknown type {f.get('type')!r}")
        lines = passes_to_lines(f.get("passes", []))
        out[name] = {"type": ftype, "lines": lines}

        q_lines = q_solve_lines(lines)
        key = CANONICAL_DUAL.get(frozenset(l["name"] for l in q_lines))
        # Two-line canonicals are built from dual-band products only: a quad
        # filter's Ha pass is a different piece of glass from a dual's, and
        # averaging their FWHMs together would describe neither. A three-line
        # canonical is the opposite case -- only a tri- or quad-band product
        # can cover all three Q-solve lines, so those products are its only
        # possible source, and excluding them left the set with no canonical
        # name at all. That is what stopped an L-Quad Enhance batch at start
        # while its measured QE sat in the database, unreachable.
        if key and (f.get("type") == "dual-narrowband" or len(q_lines) == 3):
            dual_sources.setdefault(key, []).append(q_lines)
        if ftype == "NARROWBAND" and len(q_lines) == 1:
            single_sources.setdefault(q_lines[0]["name"], []).append(q_lines)

        if name in PRODUCT_CANONICAL:
            out[PRODUCT_CANONICAL[name]] = {"type": "DUAL_NB", "lines": ordered_for_q_solve(q_lines)}

    for key, sources in dual_sources.items():
        # Ha before OIII, SII before OIII: the order the classifier documents.
        out[key] = {"type": "DUAL_NB", "lines": ordered_for_q_solve(median_lines(sources))}
    for line_name, sources in single_sources.items():
        out[line_name] = {"type": "NARROWBAND", "lines": median_lines(sources)}

    missing = [k for k in REQUIRED_CANONICAL if k not in out]
    if missing:
        raise TransformError(f"Research data yields no source for canonical filter(s): {missing}")
    return out


def resolved_qe(name, cam, sensors):
    if cam.get("qe_inherits_from_sensor"):
        s = sensors.get(cam.get("sensor"))
        if s is None:
            raise TransformError(f"Camera '{name}' inherits from sensor {cam.get('sensor')!r} which is not in `sensors`")
        return s.get("qe", {})
    return cam.get("qe", {})


def validate_qe_block(label, qe_block):
    """Reject any QE block that would otherwise be dropped, coerced or
    truncated on its way to the engine. Checks every key the block carries,
    including the ones this camera's type ignores: a percentage typed into a
    mono_pk column is corrupt research data whichever camera reads it."""
    if not isinstance(qe_block, dict):
        raise TransformError(f"{label}: qe must be an object keyed by wavelength, got {type(qe_block).__name__}")
    for wl, sites in qe_block.items():
        if not WAVELENGTH_KEY.fullmatch(wl):
            raise TransformError(f"{label}: wavelength key {wl!r} is not a plain integer nm "
                                 f"(e.g. '656'); the loader would truncate or reject it")
        if not isinstance(sites, dict):
            raise TransformError(f"{label} wavelength '{wl}': photosites must be an object, "
                                 f"got {type(sites).__name__}")
        unknown = sorted(set(sites) - KNOWN_SITES)
        if unknown:
            raise TransformError(f"{label} wavelength '{wl}': unknown photosite key(s) {unknown}; "
                                 f"known: {sorted(KNOWN_SITES)}")
        for site, v in sites.items():
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                raise TransformError(f"{label} wavelength '{wl}': photosite '{site}' = {v!r} is not a number")
            if not 0.0 <= v <= 1.0:
                raise TransformError(f"{label} wavelength '{wl}': photosite '{site}' = {v!r} is not a "
                                     f"QE fraction in [0, 1] (a percentage?)")
        if all(k in sites for k in MONO_SITES):
            raise TransformError(f"{label} wavelength '{wl}': both 'mono' and its alias 'mono_pk' are "
                                 f"present; ambiguous which one ships")


def green_qe(label, wl, sites):
    """The single G value the loader consumes, or None if the wavelength has
    no green at all (the caller decides whether that is a gap or an error).

    Deterministic rule, no implicit averaging of mismatched data:
      - "G" alone        -> shipped as-is (already the per-pixel green).
      - "Gr" and "Gb"    -> their mean. A Bayer cell has two green photosites,
                            one on the red row and one on the blue row, and the
                            loader's G is the one green channel both feed, so
                            the mean is the physically correct collapse. This
                            is the shape of all the real research data.
      - "G" with Gr/Gb   -> error: two independent answers for one quantity,
                            and silently averaging all three would weight the
                            split readings 2:1 over the merged one.
      - Gr without Gb, or vice versa -> error: half a split is not a
                            measurement of the mean; the other half is unknown,
                            not equal.
    """
    split = [k for k in ("Gr", "Gb") if k in sites]
    if "G" in sites:
        if split:
            raise TransformError(f"{label} wavelength '{wl}': merged 'G' and split {split} are both "
                                 f"present; ambiguous which is authoritative")
        return float(sites["G"])
    if len(split) == 1:
        raise TransformError(f"{label} wavelength '{wl}': only {split[0]!r} of the Gr/Gb pair is "
                             f"present; cannot form G from half a Bayer green split")
    if not split:
        return None
    return round(statistics.mean((float(sites["Gr"]), float(sites["Gb"]))), 4)


def photosites(name, cam_type, qe_block, label=None):
    """Collapse research photosites to what the loader consumes.

    OSC: a wavelength with no R/G/B is not immediately fatal — some sensors
    were only ever researched as a flat panchromatic curve (`ICX694` and
    friends), never split by Bayer channel. If NONE of the camera's
    wavelengths have a Bayer split, the caller excludes the camera outright
    (spec 6.3: generic_sony_imx_osc covers it at runtime). But if SOME
    wavelengths have a split and others don't, that is inconsistent research
    data, not a missing-measurement camera, and must still fail loud.
    """
    label = label or f"Camera '{name}'"
    validate_qe_block(label, qe_block)
    out = {}
    incomplete = []
    for wl, sites in qe_block.items():
        if cam_type == "mono":
            # "mono" is research vocabulary drift, not a different quantity:
            # some sensors record the same peak/measured mono QE under the
            # bare key instead of "mono_pk". Ship it under the one key name
            # the loader knows. validate_qe_block() has already refused a
            # wavelength that carries both, so this lookup cannot choose.
            pk = sites.get("mono_pk", sites.get("mono"))
            if pk is not None:
                out[str(wl)] = {"mono_pk": float(pk)}
            continue
        g = green_qe(label, wl, sites)
        if "R" not in sites or "B" not in sites or g is None:
            incomplete.append(wl)
            continue
        out[str(wl)] = {"R": float(sites["R"]), "G": g, "B": float(sites["B"])}
    if out and incomplete:
        raise TransformError(f"Camera '{name}': wavelength(s) {sorted(incomplete)} lack R/G/B "
                              f"photosites while other wavelengths have them (inconsistent Bayer data)")
    return out


def transform_camera(name, cam, sensors):
    """Returns the shipped camera dict, or None if an OSC camera's sensor has
    no Bayer-split QE at any wavelength (excluded by the caller, not an error:
    spec 6.3's generic_sony_imx_osc fallback covers it at runtime)."""
    missing = [k for k in REQUIRED_CAMERA_FIELDS if k not in cam]
    if missing:
        raise TransformError(f"Camera '{name}' missing required field(s): {missing}")
    if not isinstance(cam["confidence"], str) or cam["confidence"] not in ALLOWED_CONFIDENCE:
        raise TransformError(f"Camera '{name}': invalid confidence {cam['confidence']!r}; "
                             f"the loader accepts exactly {list(ALLOWED_CONFIDENCE)}")
    out = {"sensor": cam["sensor"], "type": cam["type"], "confidence": cam["confidence"]}
    if "manufacturer" in cam:
        out["manufacturer"] = cam["manufacturer"]
    if cam["type"] == "OSC":
        bayer = cam.get("bayer_pattern")
        if bayer not in BAYER_OK:
            raise TransformError(f"Camera '{name}': OSC camera needs bayer_pattern in {sorted(BAYER_OK)}, got {bayer!r}")
        out["bayer"] = bayer
    label = f"Camera '{name}'"
    if cam.get("qe_inherits_from_sensor"):
        label += f" (QE inherited from sensor '{cam['sensor']}')"
    out["qe"] = photosites(name, cam["type"], resolved_qe(name, cam, sensors), label)
    if cam["type"] == "OSC" and not out["qe"]:
        return None
    if cam["type"] == "OSC" and len(out["qe"]) < 2:
        raise TransformError(f"Camera '{name}': OSC camera needs QE at >= 2 wavelengths")
    return out


def generic_sony_osc(cameras, sensors):
    """Mean R/G/B per wavelength over Sony-sensor OSC cameras; the spec 6.3 unknown-INSTRUME fallback."""
    members = [c for c in cameras.values()
               if c["type"] == "OSC" and "sony" in sensors.get(c["sensor"], {}).get("manufacturer", "").lower()]
    if not members:
        raise TransformError("No Sony-sensor OSC cameras found; cannot derive generic_sony_imx_osc")
    wavelengths = set.intersection(*(set(c["qe"]) for c in members))
    qe = {}
    for wl in sorted(wavelengths, key=int):
        qe[wl] = {site: round(statistics.mean(c["qe"][wl][site] for c in members), 4) for site in ("R", "G", "B")}
    return {"sensor": "generic", "manufacturer": "generic", "type": "OSC", "bayer": "RGGB",
            "confidence": "low", "qe": qe}


def transform(research):
    sensors = research.get("sensors", {})
    cameras = {}
    mono_without_qe = []
    osc_without_bayer_qe = []
    for name, cam in research.get("cameras", {}).items():
        result = transform_camera(name, cam, sensors)
        if result is None:
            osc_without_bayer_qe.append(name)
            continue
        cameras[name] = result
        if cam.get("type") == "mono" and not cameras[name]["qe"]:
            mono_without_qe.append(name)
    cameras[GENERIC_OSC_KEY] = generic_sony_osc(cameras, sensors)
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_from": "research/qe_database_research.json via tools/import_qe_research.py",
        "cameras": cameras,
        "filters": transform_filters(research.get("filters", {})),
    }, mono_without_qe, osc_without_bayer_qe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()
    with open(args.input) as f:
        research = json.load(f)
    try:
        shipped, mono_without_qe, osc_without_bayer_qe = transform(research)
    except TransformError as e:
        print(f"import_qe_research: {e}", file=sys.stderr)
        return 1
    if mono_without_qe:
        print(f"note: {len(mono_without_qe)} mono camera(s) ship without a QE block "
              f"(sensor has no mono_pk; mono frames never enter the Q-solve): {mono_without_qe}",
              file=sys.stderr)
    if osc_without_bayer_qe:
        print(f"note: {len(osc_without_bayer_qe)} OSC camera(s) excluded — research has no "
              f"Bayer-split QE; generic_sony_imx_osc applies at runtime with a Process Console "
              f"warning: {osc_without_bayer_qe}", file=sys.stderr)
    with open(args.output, "w") as f:
        json.dump(shipped, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"wrote {args.output}: {len(shipped['cameras'])} cameras, {len(shipped['filters'])} filters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
