import textwrap
import pytest
from gaia_depth_grade.config import Gains, GradeConfig, load_config


def test_defaults_when_no_path():
    cfg = load_config(None)
    assert isinstance(cfg, GradeConfig)
    assert cfg.gains.brightness == 0.5
    assert cfg.p_low == 5.0 and cfg.p_high == 95.0


def test_toml_overrides(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text(textwrap.dedent("""
        p_low = 10.0
        [gains]
        brightness = 1.0
        saturation = 0.0
    """))
    cfg = load_config(str(p))
    assert cfg.p_low == 10.0
    assert cfg.gains.brightness == 1.0
    assert cfg.gains.saturation == 0.0
    assert cfg.gains.size == 0.4  # untouched default


def test_unknown_key_raises(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text("bogus_key = 1\n")
    with pytest.raises(ValueError):
        load_config(str(p))
