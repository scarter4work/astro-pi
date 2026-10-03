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
        # The only source of the three-line HaO3S2 canonical the importer
        # requires (REQUIRED_CANONICAL); without it every transform refuses.
        "Optolong-LQuadEnhance": {"type": "quad-narrowband",
                                  "passes": [{"center_nm": 656.3, "fwhm_nm": 12.0}, {"center_nm": 500.7, "fwhm_nm": 12.0},
                                             {"center_nm": 672.4, "fwhm_nm": 12.0}, {"center_nm": 486.1, "fwhm_nm": 12.0}]},
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


def test_product_canonical_line_order_enforced_regardless_of_source_order(tmp_path):
    doc = json.loads(json.dumps(BASE))
    doc["filters"]["Optolong-LeXtreme-7nm"] = {"type": "dual-narrowband",
        "passes": [{"center_nm": 500.7, "fwhm_nm": 7.0}, {"center_nm": 656.3, "fwhm_nm": 7.0}]}  # OIII listed first
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 0, r.stderr
    f = json.loads(dst.read_text())["filters"]
    assert [l["name"] for l in f["L-eXtreme"]["lines"]] == ["Ha", "OIII"]
    assert [l["name"] for l in f["HaO3"]["lines"]] == ["Ha", "OIII"]


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


def test_osc_camera_without_bayer_qe_is_excluded_with_note(tmp_path):
    doc = json.loads(json.dumps(BASE))
    # ICX694-shaped gap: a "both-variants" sensor researched only as a flat
    # panchromatic curve, never split by Bayer channel at any wavelength.
    doc["sensors"]["ICX694"] = sensor({"501": {"mono": 0.67}, "656": {"mono": 0.65}})
    doc["cameras"]["atik-460ex-color"] = osc_cam("ICX694")
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert "atik-460ex-color" not in out["cameras"]
    assert "atik-460ex-color" in r.stderr
    assert "Bayer-split" in r.stderr
    assert "asi585mc" in out["cameras"]
    assert "asi585mm" in out["cameras"]


def test_mono_alias_key_ships_as_mono_pk(tmp_path):
    doc = json.loads(json.dumps(BASE))
    doc["sensors"]["IMX174"] = sensor({"656": {"mono": 0.55}})
    doc["cameras"]["asi174mm"] = {"manufacturer": "ZWO", "sensor": "IMX174", "type": "mono", "bayer_pattern": None,
                                   "qe_inherits_from_sensor": True, "confidence": "medium", "source_urls": [], "notes": ""}
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 0, r.stderr
    cam = json.loads(dst.read_text())["cameras"]["asi174mm"]
    assert cam["qe"]["656"] == {"mono_pk": 0.55}


def test_real_research_file_round_trip(tmp_path):
    dst = tmp_path / "shipped.json"
    r = run(RESEARCH, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    for key in ("HaO3", "S2O3", "L-eXtreme", "L-eNhance", "L-Ultimate", "ALP-T", "Ha", "OIII", "SII"):
        assert key in out["filters"], key
    assert "generic_sony_imx_osc" in out["cameras"]
    assert len(out["cameras"]) >= 55
    assert out["cameras"]["asi2400mc"]["bayer"] == "RGGB"
    for name, cam in out["cameras"].items():
        if cam["type"] == "OSC":
            wavelengths_with_rgb = [wl for wl, sites in cam["qe"].items()
                                     if all(k in sites for k in ("R", "G", "B"))]
            assert len(wavelengths_with_rgb) >= 2, name
