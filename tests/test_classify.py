import pytest
from astrometa.classify import classify


@pytest.mark.parametrize("name,expected", [
    ("Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit", "light"),
    ("Dark_120.0s_Bin1_Lqef_20260101-000000_1deg_0001.fit", "dark"),
    ("Bias_1.0ms_Bin1_HaO3_20260101-000000_0001.fit", "bias"),
    ("B_master_flat.fit", "flat"),
    ("flat_001.fit", "flat"),
    ("pp_light_00260.fit", "derived"),
    ("r_pp_light_00012.fit", "derived"),
    ("ASIVideoStack_Output_01.fit", "derived"),
    ("AS_P20_moon.fit", "derived"),
    ("Autosave001.fit", "derived"),
    ("something_unrecognised.fit", "unknown"),
    ("light_0001.fit", "light"),
    ("dark_0001.fit", "dark"),
])
def test_classify_from_filename(name, expected):
    assert classify(name, {}) == expected


def test_imagetyp_header_overrides_filename():
    assert classify("mystery.fit", {"IMAGETYP": "Dark Frame"}) == "dark"
    assert classify("mystery.fit", {"IMAGETYP": "Light Frame"}) == "light"
    assert classify("mystery.fit", {"IMAGETYP": "FLAT"}) == "flat"
    assert classify("mystery.fit", {"IMAGETYP": "Bias Frame"}) == "bias"


def test_unrecognised_imagetyp_falls_back_to_filename():
    assert classify("Light_M 42_1.fit", {"IMAGETYP": "wibble"}) == "light"


def test_target_name_with_space_does_not_break_classification():
    assert classify("Light_NGC 7635_300.0s_Bin1_Lqef_x_0001.fit", {}) == "light"
