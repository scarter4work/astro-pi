### Task 15: `tools/import_qe_research.py` — research JSON → engine-keyed shipping JSON

**Why the April version cannot ship.** The research file keys 87 filters by product name (`Astrodon-Ha-3nm-50mm`) with a `passes` array; cameras carry `bayer_pattern` (null for mono), 48 of 55 inherit QE from a `sensors` block that holds `Gr`/`Gb`/`mono_pk`. The loader (`qe_database.cpp`) reads `bayer`, `lines[{name,wavelength_nm,fwhm_nm}]`, and photosites `R/G/B/Gr/Gb/*`; the engine looks filters up by the classifier's canonical names. The April script passed `filters` through untouched — the engine would find no `HaO3`.

**Files:**
- Create: `tools/import_qe_research.py`
- Create: `tools/test_import_qe_research.py`

**Interfaces:**
- Consumes: `research/qe_database_research.json` (`_meta`, `sensors`, `cameras`, `filters`).
- Produces: `share/qe_database.json` with `schema_version: 1`, `cameras` (keys as in research, lowercase model ids, plus `generic_sony_imx_osc`), `filters` (canonical keys `HaO3, S2O3, L-eXtreme, L-eNhance, L-Ultimate, ALP-T, Ha, OIII, SII` plus every product name).

- [ ] **Step 1: Write the tests** — `tools/test_import_qe_research.py`

```python
import json
import pathlib
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tools" / "import_qe_research.py"
RESEARCH = REPO / "research" / "qe_database_research.json"


def run(src, dst):
    return subprocess.run(["python3", str(SCRIPT), str(src), str(dst)],
                          capture_output=True, text=True, check=False)


def sensor(qe, **extra):
    return {"manufacturer": "Sony Semiconductor", "type": "both-variants", "qe": qe, **extra}


def osc_cam(sensor_name, **extra):
    return {"manufacturer": "ZWO", "sensor": sensor_name, "type": "OSC", "bayer_pattern": "RGGB",
            "qe_inherits_from_sensor": True, "confidence": "high", "source_urls": [], "notes": "", **extra}


BASE = {
    "_meta": {"researcher": "test"},
    "sensors": {
        "IMX585": sensor({"501": {"R": 0.03, "Gr": 0.85, "Gb": 0.87, "B": 0.5, "mono_pk": 0.91},
                          "656": {"R": 0.73, "Gr": 0.32, "Gb": 0.30, "B": 0.03, "mono_pk": 0.81}}),
    },
    "cameras": {
        "asi585mc": osc_cam("IMX585"),
        "asi585mm": {"manufacturer": "ZWO", "sensor": "IMX585", "type": "mono", "bayer_pattern": None,
                     "qe_inherits_from_sensor": True, "confidence": "medium", "source_urls": [], "notes": ""},
    },
    "filters": {
        "Optolong-LeXtreme-7nm": {"type": "dual-narrowband",
                                  "passes": [{"center_nm": 656.3, "fwhm_nm": 7.0}, {"center_nm": 500.7, "fwhm_nm": 7.0}]},
        "SVBony-SV220-3nm": {"type": "dual-narrowband",
                             "passes": [{"center_nm": 656.3, "fwhm_nm": 3.0}, {"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Antlia-ALP-T-SII-OIII-3nm": {"type": "dual-narrowband",
                                      "passes": [{"center_nm": 672.4, "fwhm_nm": 3.0}, {"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Optolong-LeNhance": {"type": "tri-narrowband",
                              "passes": [{"center_nm": 656.3, "fwhm_nm": 24}, {"center_nm": 500.7, "fwhm_nm": 10}, {"center_nm": 486.1, "fwhm_nm": 10}]},
        "Optolong-LUltimate-3nm": {"type": "dual-narrowband",
                                   "passes": [{"center_nm": 656.3, "fwhm_nm": 3.0}, {"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Antlia-ALP-T-Ha-OIII-5nm": {"type": "dual-narrowband",
                                     "passes": [{"center_nm": 656.3, "fwhm_nm": 5.0}, {"center_nm": 500.7, "fwhm_nm": 5.0}]},
        "Astrodon-Ha-3nm-50mm": {"type": "narrowband-single", "passes": [{"center_nm": 656.3, "fwhm_nm": 3.0}]},
        "Astrodon-OIII-3nm-50mm": {"type": "narrowband-single", "passes": [{"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Astrodon-SII-3nm-50mm": {"type": "narrowband-single", "passes": [{"center_nm": 672.4, "fwhm_nm": 3.0}]},
        "Optolong-Lpro": {"type": "broadband-LPR", "passes": [{"center_nm": 540, "fwhm_nm": 300}]},
    },
}


def write(tmp_path, doc):
    src = tmp_path / "research.json"
    src.write_text(json.dumps(doc))
    return src, tmp_path / "shipped.json"


def test_drops_meta_and_research_only_fields(tmp_path):
    src, dst = write(tmp_path, BASE)
    r = run(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert "_meta" not in out
    assert out["schema_version"] == 1
    cam = out["cameras"]["asi585mc"]
    for k in ("qe_inherits_from_sensor", "bayer_pattern", "source_urls", "notes"):
        assert k not in cam


def test_osc_camera_inherits_sensor_qe_with_G_mean_and_bayer_key(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    cam = json.loads(dst.read_text())["cameras"]["asi585mc"]
    assert cam["bayer"] == "RGGB"
    assert cam["qe"]["656"] == {"R": 0.73, "G": 0.31, "B": 0.03}


def test_mono_camera_ships_mono_pk_only_and_no_bayer(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    cam = json.loads(dst.read_text())["cameras"]["asi585mm"]
    assert "bayer" not in cam
    assert cam["qe"]["656"] == {"mono_pk": 0.81}


def test_canonical_dual_nb_entries_use_median_fwhm(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    f = json.loads(dst.read_text())["filters"]
    hao3 = f["HaO3"]
    assert hao3["type"] == "DUAL_NB"
    assert [(l["name"], l["wavelength_nm"]) for l in hao3["lines"]] == [("Ha", 656.3), ("OIII", 500.7)]
    assert hao3["lines"][0]["fwhm_nm"] == 4.0          # median of 7.0, 3.0, 3.0, 5.0 (dual-narrowband products only)
    s2o3 = f["S2O3"]
    assert [l["name"] for l in s2o3["lines"]] == ["SII", "OIII"]
    assert s2o3["lines"][0]["fwhm_nm"] == 3.0


def test_classifier_product_canonicals_present_and_hb_dropped(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    f = json.loads(dst.read_text())["filters"]
    assert [l["name"] for l in f["L-eXtreme"]["lines"]] == ["Ha", "OIII"]
    assert [l["name"] for l in f["L-eNhance"]["lines"]] == ["Ha", "OIII"]   # Hb dropped: same photosites as OIII
    assert "Optolong-LeNhance" in f                                          # product entry kept (informational)


def test_single_line_canonicals_and_broadband_products(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    f = json.loads(dst.read_text())["filters"]
    assert f["Ha"]["lines"] == [{"name": "Ha", "wavelength_nm": 656.3, "fwhm_nm": 3.0}]
    assert f["Optolong-Lpro"] == {"type": "BROADBAND", "lines": []}


def test_generic_sony_osc_camera_is_mean_of_sony_osc_cameras(tmp_path):
    doc = json.loads(json.dumps(BASE))
    doc["sensors"]["IMX571"] = sensor({"501": {"R": 0.07, "Gr": 0.89, "Gb": 0.89, "B": 0.6},
                                       "656": {"R": 0.47, "Gr": 0.06, "Gb": 0.04, "B": 0.05}})
    doc["cameras"]["asi2600mc"] = osc_cam("IMX571")
    src, dst = write(tmp_path, doc)
    assert run(src, dst).returncode == 0
    g = json.loads(dst.read_text())["cameras"]["generic_sony_imx_osc"]
    assert g["type"] == "OSC" and g["bayer"] == "RGGB" and g["confidence"] == "low"
    assert g["qe"]["656"]["R"] == round((0.73 + 0.47) / 2, 4)
    assert g["qe"]["656"]["G"] == round((0.31 + 0.05) / 2, 4)


def test_missing_required_field_fails_loud(tmp_path):
    doc = json.loads(json.dumps(BASE))
    del doc["cameras"]["asi585mc"]["confidence"]
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 1
    assert "asi585mc" in r.stderr and "confidence" in r.stderr
    assert not dst.exists()


def test_missing_canonical_filter_fails_loud(tmp_path):
    doc = json.loads(json.dumps(BASE))
    del doc["filters"]["Antlia-ALP-T-SII-OIII-3nm"]     # no S2O3 source left
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 1
    assert "S2O3" in r.stderr


def test_real_research_file_round_trip(tmp_path):
    dst = tmp_path / "shipped.json"
    r = run(RESEARCH, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    for key in ("HaO3", "S2O3", "L-eXtreme", "L-eNhance", "L-Ultimate", "ALP-T", "Ha", "OIII", "SII"):
        assert key in out["filters"], key
    assert "generic_sony_imx_osc" in out["cameras"]
    assert len(out["cameras"]) >= 56
    assert out["cameras"]["asi2400mc"]["bayer"] == "RGGB"
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest tools/test_import_qe_research.py -q 2>&1 | tail -3`
Expected: 10 failed (`No such file` for the script).

- [ ] **Step 3: Implement `tools/import_qe_research.py`**

```python
#!/usr/bin/env python3
"""Transform research/qe_database_research.json -> share/qe_database.json.

The research file is keyed the way a researcher thinks: product filter names
with `passes`, cameras that inherit QE from a `sensors` block, Gr/Gb/mono_pk
photosites. The engine is keyed the way FilterClassifier + QEDatabase think:
canonical filter names ("HaO3", "L-eXtreme", ...) with emission `lines`, one
resolved QE block per camera with R/G/B (OSC) or mono_pk (mono) photosites.
This script is the only bridge between the two. It is deterministic and
refuses to write a database the engine cannot consume.

Usage: import_qe_research.py <research.json> <shipped.json>
"""
import argparse
import json
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
CANONICAL_DUAL = {frozenset(("Ha", "OIII")): "HaO3", frozenset(("SII", "OIII")): "S2O3"}

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
        if f.get("type") == "dual-narrowband":   # tri/quad products carry extra passes; keep them out of the medians
            key = CANONICAL_DUAL.get(frozenset(l["name"] for l in q_lines))
            if key:
                dual_sources.setdefault(key, []).append(q_lines)
        elif ftype == "NARROWBAND" and len(q_lines) == 1:
            single_sources.setdefault(q_lines[0]["name"], []).append(q_lines)

        if name in PRODUCT_CANONICAL:
            out[PRODUCT_CANONICAL[name]] = {"type": "DUAL_NB", "lines": q_lines}

    for key, sources in dual_sources.items():
        # Ha before OIII, SII before OIII: the order the classifier documents.
        lines = median_lines(sources)
        lines.sort(key=lambda l: -l["wavelength_nm"])
        out[key] = {"type": "DUAL_NB", "lines": lines}
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


def photosites(name, cam_type, qe_block):
    """Collapse research photosites to what the loader consumes."""
    out = {}
    for wl, sites in qe_block.items():
        if cam_type == "mono":
            if "mono_pk" in sites:
                out[str(wl)] = {"mono_pk": float(sites["mono_pk"])}
            continue
        greens = [sites[k] for k in ("G", "Gr", "Gb") if k in sites]
        if "R" not in sites or "B" not in sites or not greens:
            raise TransformError(f"Camera '{name}': wavelength {wl} lacks R/G/B photosites: {sorted(sites)}")
        out[str(wl)] = {"R": float(sites["R"]),
                        "G": round(statistics.mean(float(g) for g in greens), 4),
                        "B": float(sites["B"])}
    return out


def transform_camera(name, cam, sensors):
    missing = [k for k in REQUIRED_CAMERA_FIELDS if k not in cam]
    if missing:
        raise TransformError(f"Camera '{name}' missing required field(s): {missing}")
    out = {"sensor": cam["sensor"], "type": cam["type"], "confidence": cam["confidence"]}
    if "manufacturer" in cam:
        out["manufacturer"] = cam["manufacturer"]
    if cam["type"] == "OSC":
        bayer = cam.get("bayer_pattern")
        if bayer not in BAYER_OK:
            raise TransformError(f"Camera '{name}': OSC camera needs bayer_pattern in {sorted(BAYER_OK)}, got {bayer!r}")
        out["bayer"] = bayer
    out["qe"] = photosites(name, cam["type"], resolved_qe(name, cam, sensors))
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
    for name, cam in research.get("cameras", {}).items():
        cameras[name] = transform_camera(name, cam, sensors)
        if cam.get("type") == "mono" and not cameras[name]["qe"]:
            mono_without_qe.append(name)
    cameras[GENERIC_OSC_KEY] = generic_sony_osc(cameras, sensors)
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_from": "research/qe_database_research.json via tools/import_qe_research.py",
        "cameras": cameras,
        "filters": transform_filters(research.get("filters", {})),
    }, mono_without_qe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()
    with open(args.input) as f:
        research = json.load(f)
    try:
        shipped, mono_without_qe = transform(research)
    except TransformError as e:
        print(f"import_qe_research: {e}", file=sys.stderr)
        return 1
    if mono_without_qe:
        print(f"note: {len(mono_without_qe)} mono camera(s) ship without a QE block "
              f"(sensor has no mono_pk; mono frames never enter the Q-solve): {mono_without_qe}",
              file=sys.stderr)
    with open(args.output, "w") as f:
        json.dump(shipped, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"wrote {args.output}: {len(shipped['cameras'])} cameras, {len(shipped['filters'])} filters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

`chmod +x tools/import_qe_research.py`.

- [ ] **Step 4: Run the tests**

Run: `python3 -m pytest tools/test_import_qe_research.py -q 2>&1 | tail -3`
Expected: `10 passed`.

- [ ] **Step 5: Commit**

```bash
git add tools/import_qe_research.py tools/test_import_qe_research.py
git commit -m "$(cat <<'EOF'
feat(tools): import_qe_research.py maps research JSON onto the engine's lookup keys

The engine looks filters up by FilterClassifier's canonical names (HaO3,
S2O3, L-eXtreme, L-eNhance, L-Ultimate, ALP-T) and cameras by INSTRUME-
derived keys; the research file keys 87 filters by product name with a
`passes` array. The transform derives the canonical entries from the
products (median FWHM per line, Hb dropped from the Q-solve set because
it shares photosites with OIII), keeps product entries for override
reference, resolves sensor inheritance, collapses Gr/Gb to G, renames
bayer_pattern -> bayer, ships mono_pk for mono cameras, and derives the
spec-6.3 generic_sony_imx_osc fallback camera as the mean of Sony OSC
cameras. Fails loud on any missing canonical key or camera field.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

