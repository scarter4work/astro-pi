import importlib.util
import json
import pathlib
import subprocess
import sys

import pytest

REPO = pathlib.Path(__file__).parent.parent
SCRIPT = REPO / "tools" / "import_qe_research.py"
RESEARCH_JSON = REPO / "research" / "qe_database_research.json"

# Import the script as a module too, so we can unit-test its helper
# functions directly (not just the CLI's exit code / stdout contract).
_spec = importlib.util.spec_from_file_location("import_qe_research", SCRIPT)
iqr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(iqr)


def run_import(input_path, output_path):
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(input_path), str(output_path)],
        capture_output=True, text=True, check=False,
    )
    return result


# --- CLI-level tests (subprocess, matches how the script is actually run) ---

def test_drops_meta_block(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "_meta": {"researcher": "test"},
        "sensors": {},
        "cameras": {
            "ASI_test": {
                "sensor": "Fake",
                "type": "OSC",
                "bayer": "RGGB",
                "qe": {"656": {"R": 0.5, "G": 0.1, "B": 0.05}},
                "confidence": "high"
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert "_meta" not in out
    assert "sensors" not in out
    assert "ASI_test" in out["cameras"]
    assert out["cameras"]["ASI_test"]["bayer"] == "RGGB"


def test_resolves_sensor_inheritance(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "_meta": {"researcher": "test"},
        "sensors": {
            "IMX571": {
                "qe": {
                    "656": {"R": 0.46, "Gr": 0.05, "Gb": 0.05, "B": 0.04, "mono_pk": 0.50}
                },
                "confidence": "high"
            }
        },
        "cameras": {
            "ASI2600MC": {
                "sensor": "IMX571",
                "type": "OSC",
                "bayer": "RGGB",
                "qe_inherits_from_sensor": True,
                "confidence": "medium"
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    cam = out["cameras"]["ASI2600MC"]
    assert "qe_inherits_from_sensor" not in cam
    assert "qe" in cam
    assert cam["qe"]["656"]["R"] == 0.46
    # Gr/Gb averaged into a single explicit "G" key (see photosite key
    # contract docstring: avoids the C++ loader's alphabetical-iteration
    # overwrite footgun for objects with both Gr and Gb keys).
    assert cam["qe"]["656"]["G"] == pytest.approx(0.05)
    assert "Gr" not in cam["qe"]["656"]
    assert "Gb" not in cam["qe"]["656"]
    # mono is a separate photosite (raw sensor QE) and must survive
    # under the canonical key even though the camera itself is OSC.
    assert cam["qe"]["656"]["mono_pk"] == 0.50


def test_validates_camera_required_fields(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "Bad": {"type": "OSC"}  # missing sensor, qe, confidence
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode != 0
    assert "Bad" in r.stderr


def test_writes_schema_version(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({"sensors": {}, "cameras": {}, "filters": {}}))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0
    out = json.loads(dst.read_text())
    assert out.get("schema_version") == 1


def test_bayer_pattern_renamed_to_bayer(tmp_path):
    """Real research cameras use 'bayer_pattern', not 'bayer' -- the
    shipping schema (and the C++ loader) expects 'bayer'."""
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "Cam": {
                "sensor": "S",
                "type": "OSC",
                "bayer_pattern": "RGGB",
                "confidence": "high",
                "qe": {"656": {"R": 0.5, "G": 0.4, "B": 0.3}},
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert out["cameras"]["Cam"]["bayer"] == "RGGB"
    assert "bayer_pattern" not in out["cameras"]["Cam"]


def test_mono_camera_no_bayer_key(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "MonoCam": {
                "sensor": "S",
                "type": "mono",
                "bayer_pattern": None,
                "confidence": "high",
                "qe": {"656": {"mono": 0.55}},
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    cam = out["cameras"]["MonoCam"]
    assert "bayer" not in cam
    # "mono" alias normalised to the canonical "mono_pk" token.
    assert cam["qe"]["656"]["mono_pk"] == 0.55


def test_rejects_unknown_photosite_key(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "Typo": {
                "sensor": "S",
                "type": "OSC",
                "confidence": "high",
                "qe": {"656": {"Rr": 0.5}},  # typo: not a recognised key
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode != 0
    assert "Typo" in r.stderr
    assert not dst.exists()


def test_rejects_ambiguous_mono_aliases(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "Ambiguous": {
                "sensor": "S",
                "type": "mono",
                "confidence": "high",
                "qe": {"656": {"mono": 0.5, "mono_pk": 0.6}},
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode != 0
    assert "Ambiguous" in r.stderr


def test_rejects_out_of_range_qe_value(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "OutOfRange": {
                "sensor": "S",
                "type": "OSC",
                "confidence": "high",
                "qe": {"656": {"R": 1.5}},
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode != 0
    assert "OutOfRange" in r.stderr


def test_filter_passes_converted_to_lines(tmp_path):
    """Real research filters use 'passes' with 'center_nm', not the
    shipping schema's 'lines' with 'wavelength_nm'."""
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {},
        "filters": {
            "Astrodon-Ha-3nm": {
                "type": "narrowband-single",
                "passes": [
                    {"center_nm": 656.3, "fwhm_nm": 3.0, "peak_transmission": 0.92}
                ],
                "confidence": "high",
            }
        }
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    filt = out["filters"]["Astrodon-Ha-3nm"]
    assert "passes" not in filt
    assert filt["lines"] == [
        {"name": "Halpha", "wavelength_nm": 656.3, "fwhm_nm": 3.0}
    ]


def test_filter_multi_pass_and_unknown_wavelength_falls_back_to_numeric_name(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {},
        "filters": {
            "Optolong-LeNhance": {
                "type": "tri-narrowband",
                "passes": [
                    {"center_nm": 656.3, "fwhm_nm": 24, "peak_transmission": 0.9},
                    {"center_nm": 500.7, "fwhm_nm": 10, "peak_transmission": 0.85},
                    {"center_nm": 486.1, "fwhm_nm": 10, "peak_transmission": 0.85},
                ],
            },
            "Generic-Broadband-Red": {
                "type": "broadband-RGB",
                "passes": [
                    {"center_nm": 625, "fwhm_nm": 90, "peak_transmission": 0.95}
                ],
            },
        }
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    tri = out["filters"]["Optolong-LeNhance"]["lines"]
    assert [l["name"] for l in tri] == ["Halpha", "OIII", "Hbeta"]
    broadband = out["filters"]["Generic-Broadband-Red"]["lines"]
    assert broadband == [{"name": "625nm", "wavelength_nm": 625, "fwhm_nm": 90}]


def test_filter_lines_passthrough_when_already_shipping_shape(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {},
        "filters": {
            "AlreadyShipping": {
                "type": "narrowband-single",
                "lines": [{"name": "Halpha", "wavelength_nm": 656.3, "fwhm_nm": 3.0}],
            }
        }
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert out["filters"]["AlreadyShipping"]["lines"] == [
        {"name": "Halpha", "wavelength_nm": 656.3, "fwhm_nm": 3.0}
    ]


def test_validates_filter_required_fields(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {},
        "filters": {
            "NoPasses": {"type": "narrowband-single"}
        }
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode != 0
    assert "NoPasses" in r.stderr


# --- Unit-level tests against the imported module (finer-grained) ---

def test_normalise_qe_block_averages_gr_gb():
    out = iqr.normalise_qe_block({"656": {"Gr": 0.10, "Gb": 0.20}}, "cam")
    assert out["656"]["G"] == pytest.approx(0.15)


def test_normalise_qe_block_single_green_split_key_used_directly():
    out = iqr.normalise_qe_block({"656": {"Gr": 0.10}}, "cam")
    assert out["656"]["G"] == pytest.approx(0.10)


def test_resolve_filter_missing_wavelength_field_raises():
    with pytest.raises(ValueError, match="BadPass"):
        iqr.resolve_filter("BadPass", {"type": "x", "passes": [{"fwhm_nm": 3.0}]})


# --- Real-data smoke test: proves the script survives the actual research
# file end-to-end (without committing its output -- that's Task 16). ---

@pytest.mark.skipif(not RESEARCH_JSON.exists(), reason="research JSON not present")
def test_real_research_file_transforms_cleanly(tmp_path):
    dst = tmp_path / "qe_database.json"
    r = run_import(RESEARCH_JSON, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())

    assert out["schema_version"] == 1
    assert set(out.keys()) >= {"schema_version", "cameras", "filters"}
    assert "_meta" not in out
    assert "sensors" not in out
    assert len(out["cameras"]) == 55
    assert len(out["filters"]) == 87

    allowed_site_keys = {"R", "G", "B", "mono_pk"}
    for name, cam in out["cameras"].items():
        for field in ("sensor", "type", "confidence", "qe"):
            assert field in cam, f"{name} missing {field}"
        assert cam["confidence"] in ("high", "medium", "low")
        for wl, sites in cam["qe"].items():
            assert wl.isdigit(), f"{name} has non-integer wavelength key {wl!r}"
            assert set(sites) <= allowed_site_keys, f"{name}@{wl} has {sites.keys()}"
            for v in sites.values():
                assert 0.0 <= v <= 1.0

    for name, filt in out["filters"].items():
        assert "type" in filt, f"{name} missing type"
        assert filt["lines"], f"{name} has no lines"
        for line in filt["lines"]:
            assert {"name", "wavelength_nm", "fwhm_nm"} <= set(line)
