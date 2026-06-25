import numpy as np

from gaia_depth_grade.cli import grade_array
from gaia_depth_grade.config import GradeConfig, Gains
from gaia_depth_grade.pipeline import prepare_grade, render_from_prep


def test_prepare_then_render_equals_grade(two_star_scene):
    header, img, src, near, far = two_star_scene
    cfg = GradeConfig(
        gains=Gains(brightness=0.6, size=0.3, contrast=0.2, saturation=0.2),
        p_low=0, p_high=100, min_match_rate=0.0,
    )

    graded_ref, qa_ref = grade_array(img, header, cfg, src)

    table, qa = prepare_grade(img, header, cfg, src)
    graded = render_from_prep(img, table, cfg)

    assert np.allclose(graded, graded_ref)
    assert qa == qa_ref


def test_prepare_table_has_distance_columns(two_star_scene):
    header, img, src, near, far = two_star_scene
    cfg = GradeConfig(p_low=0, p_high=100, min_match_rate=0.0)
    table, qa = prepare_grade(img, header, cfg, src)
    for col in ("x", "y", "flux", "r_med_geo", "r_lo_geo", "r_hi_geo"):
        assert col in table.colnames
    assert qa["n_matched"] == 2
