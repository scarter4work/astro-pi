"""bench.py: the run plan, Ruling 2 / P28's status, the display stretch and the side-by-side layout.
Synthetic only (no NAS)."""
import os

import numpy as np
import pytest

import bench
import benchset
from metric import Box


def _row(label, end=1.0, end_int=1.0, judged=True, note=None):
    return {"label": label, "end": end, "end_interior": end_int if judged else None, "end_note": note,
            "interior_judged": judged}


def _rows(full, inner, judged=True, review=(), alt=None, ref_end=1.0, ref_note=None):
    """ours (with END ratios full / inner, and alt ratios) and one reference."""
    alt_full, alt_inner = alt if alt is not None else (full, inner)
    ours = _row("ours", end=full, end_int=inner, judged=judged)
    ours.update({"end_ratio": full, "end_interior_ratio": inner if judged else None,
                 "end_alt_ratio": alt_full, "end_interior_alt_ratio": alt_inner if judged else None,
                 "end_frames": 100.0, "end_alt_frames": 60.0, "review": list(review)})
    return [ours, _row("AS_P20", end=ref_end, judged=judged, note=ref_note)]


def test_plan_covers_every_capture_once_all_rggb():
    runs = bench.plan()
    ids = [r.id for r in runs]
    assert len(ids) == len(set(ids)) == len(benchset.BENCHMARK)
    assert [r.capture for r in runs] == [c.id for c in benchset.BENCHMARK]
    by = {r.id: r for r in runs}
    for c in benchset.BENCHMARK:
        if c.expect == "stack":
            assert by[c.id].bayer == "RGGB" and "camera spec" in c.bayer_source
    assert by["2026-01-30-saturn-empty"].bayer == "auto" and by["2026-01-30-saturn-empty"].expect == "NoTarget"
    assert by["2023-02-11-venus-clipped"].expect == "Clipped"


@pytest.mark.parametrize("full, inner, judged, want", [
    (1.00, 1.00, True, "review"),       # passes the bar: review, never pass (C1)
    (0.97, 0.97, True, "review"),       # the bar is inclusive
    (0.969, 1.10, True, "fail"),
    (1.10, 0.969, True, "fail"),
    (1.10, 0.50, False, "review"),      # an interior not judged does not count
])
def test_status_applies_the_bar_to_both_end_scores(full, inner, judged, want):
    st, why = bench.status_of(_rows(full, inner, judged))
    assert st == want
    if st == "review":
        assert why[-1] == bench.C1_NOTE


def test_no_capture_passes_on_its_numbers_c1():
    st, why = bench.status_of(_rows(2.0, 2.0))
    assert st == "review" and why == [bench.C1_NOTE]


def test_review_notes_are_kept_and_a_fail_stays_a_fail():
    st, why = bench.status_of(_rows(1.2, None, False, ["limb-only: human review decides"]))
    assert st == "review" and why == ["limb-only: human review decides", bench.C1_NOTE]
    assert bench.status_of(_rows(0.9, 0.9, True, ["limb-only"]))[0] == "fail"


def test_a_verdict_that_flips_with_our_frame_count_goes_to_review():
    st, why = bench.status_of(_rows(1.02, 1.05, True, alt=(0.95, 1.0)))
    assert st == "review" and "flips" in why[0] and "pass at the box's noise-power k 100" in why[0]
    assert "fail at the mean kept fraction's 60" in why[0]
    assert bench.status_of(_rows(0.9, 0.9, True, alt=(0.8, 0.8)))[0] == "fail"     # fails both: fail


def test_split_halves_outside_the_tolerance_go_to_review():
    sh = {"status": "outside", "judged": {"high": 2.86, "k": [500, 505], "ratio": [20.4] + [1.0] * 8},
          "mean_k": None}
    st, why = bench.status_of(_rows(1.9, 1.7), split_half=sh)
    assert st == "review" and why[0].startswith("split halves outside") and "x2.86" in why[0]
    ok = dict(sh, status="agrees", judged=dict(sh["judged"], high=0.9))
    assert bench.status_of(_rows(1.9, 1.7), split_half=ok)[1] == [bench.C1_NOTE]


def test_a_missing_end_is_unmeasured_never_a_verdict():
    st, why = bench.status_of(_rows(1.5, 1.5, ref_end=None, ref_note="END not computed: no frame count"))
    assert st == "unmeasured" and "NO VERDICT" in why[0] and "AS_P20: END not computed: no frame count" in why[0]


def test_stretch_removes_each_channels_pedestal_and_keeps_the_colour_ratio():
    rng = np.random.default_rng(1)
    img = np.zeros((64, 64, 3))
    img[16:48, 16:48] = [0.6, 0.4, 0.2]                # a warm target
    img += np.array([0.05, 0.0, 0.10])                 # sky pedestals that differ per channel
    img += rng.normal(0, 1e-4, img.shape)
    levels = bench.display_levels(img, Box(0, 64, 0, 64))
    out = bench.stretch(img, levels).astype(float)
    # The sky is near black in every channel (noise above the 1st percentile, through gamma 2.2:
    # a few DN); a pedestal left in would show blue at ~135 DN.
    assert out[4, 4].max() <= 16
    lin = (out[32, 32] / 255) ** (1 / bench.DISPLAY_GAMMA)
    assert lin[0] / lin[2] == pytest.approx(3.0, rel=0.05)   # 0.6 : 0.2 survives


def test_two_images_differing_by_gain_and_offset_display_identically():
    rng = np.random.default_rng(2)
    a = rng.random((40, 40)) + 1.0
    b = 7.0 * a + 300.0
    box = Box(0, 40, 0, 40)
    pa, pb = bench.stretch(a, bench.display_levels(a, box)), bench.stretch(b, bench.display_levels(b, box))
    assert np.abs(pa.astype(int) - pb.astype(int)).max() <= 1


def test_detail_tiles_find_the_textured_patches_and_keep_them_apart():
    rng = np.random.default_rng(3)
    lum = np.full((600, 1200), 0.5)
    lum[50:250, 50:250] += rng.normal(0, 0.2, (200, 200))
    lum[300:500, 900:1100] += rng.normal(0, 0.1, (200, 200))
    tiles = bench.detail_tiles(lum, Box(0, 600, 0, 1200), tile=128, sep=300)
    assert len(tiles) == 2
    (a, b) = [((t.y0 + t.y1) / 2, (t.x0 + t.x1) / 2) for t in tiles]
    assert 50 <= a[0] <= 250 and 50 <= a[1] <= 250       # the stronger texture first
    assert 300 <= b[0] <= 500 and 900 <= b[1] <= 1100
    assert all(t.y1 - t.y0 == 128 and t.x1 - t.x0 == 128 for t in tiles)


def test_compose_lays_out_columns_with_headers(tmp_path):
    from PIL import Image
    p = [np.zeros((50, 80, 3), np.uint8), np.full((50, 80, 3), 128, np.uint8)]
    out = os.path.join(tmp_path, "sbs.png")
    bench.compose([p, p], [["ours", "DAN 1"], ["AS_P20", "DAN 0.9"]], ["row 1", "row 2"], "title", out)
    w, h = Image.open(out).size
    assert w == bench.PAD + 2 * (200 + bench.PAD)        # panels narrower than 200 px get 200
    assert h > 2 * 50


def test_summary_counts_and_lists_diagnosis_rows():
    end_keys = {"end": None, "end_ratio": None, "end_interior": None, "end_interior_ratio": None,
                "end_alt_ratio": None, "end_interior_alt_ratio": None, "end_frames": None,
                "end_alt_frames": None, "end_noise_budget": None, "end_note": None}
    ours = dict(end_keys, label="ours", dan=0.1, dan_interior=None, ratio_to_best_reference=0.9,
                interior_ratio_to_best_reference=None, factor=1.0, band_snr=10.0, orientation=None, shift=None,
                scale=None, ncc=None, end=2.0, end_ratio=0.8, end_frames=9.0, end_alt_frames=7.0, end_alt_ratio=0.7)
    ref = dict(end_keys, label="AS_P20", dan=0.111, dan_interior=None, ratio_to_best_reference=1.0,
               interior_ratio_to_best_reference=None, factor=1.0, band_snr=10.0, orientation="identity",
               shift=[0.0, 0.0], scale=1.0, ncc=0.99, end=2.5, end_ratio=1.0, end_frames=2.0)
    rec_stack = {"run": "r1", "capture": "c", "bayer": "RGGB", "expect": "stack", "note": "", "status": "fail",
                 "reasons": ["END_full 0.8"], "wall_seconds": 2.0, "video_bytes": 2e9, "source": "NAS",
                 "probe": {"used": 9, "measured": 10, "seconds": 1.5, "peakRssMB": 100.0, "frames": 10,
                           "phaseSeconds": {"Stacking": 1.0}, "bytesRead": 3e9, "readMBps": 500.0},
                 "rows": [ours, ref], "photon_transfer": "v(m) = 0.1 m + 0 DN^2"}
    rec_diag = {"run": "d1", "capture": "d", "bayer": "auto", "expect": "NoTarget", "note": "", "status": "pass",
                "image_written": False, "wall_seconds": 0.5, "video_bytes": 1e6, "source": "NAS",
                "probe": {"diagnosis": {"kind": "NoTarget", "message": "nothing", "numbers": {"n": 1}},
                          "seconds": 0.4, "frames": 21, "phaseSeconds": {}, "bytesRead": 1e6, "readMBps": 50.0,
                          "peakRssMB": 50.0}}
    md = bench.summary_md([rec_stack, rec_diag])
    assert "0 pass, 0 review, 1 fail" in md and "RGGB" in md.split("\n")[2]
    assert "| r1 | RGGB | 9 / 10 | 9 (7) |" in md and "2.00 / 2.50 / -" in md and "0.800 / -" in md
    assert "not stacked" in md and "Known limit (C1)" in md
    assert "| d1 | NoTarget | NoTarget | no | **pass** |" in md


def test_detail_tiles_on_the_target_skip_the_limb():
    """Task 9's first lunar side-by-sides put both 1:1 crops on the limb, whose step outscores any
    crater; with the target mask the tiles lie wholly on the surface."""
    rng = np.random.default_rng(4)
    lum = np.full((600, 1200), 0.02)
    lum[:, 400:] = 0.5 + rng.normal(0, 0.02, (600, 800))     # surface right of a sharp limb at x 400
    lum[200:360, 800:960] += rng.normal(0, 0.08, (160, 160))  # a detailed patch on the surface
    mask = lum > 0.2
    free = bench.detail_tiles(lum, Box(0, 600, 0, 1200), tile=128, sep=300)
    assert any(t.x0 < 400 <= t.x1 for t in free)             # without the mask the limb wins a tile
    tiles = bench.detail_tiles(lum, Box(0, 600, 0, 1200), tile=128, sep=300, inside=mask)
    assert tiles and all(mask[t.y0:t.y1, t.x0:t.x1].all() for t in tiles)
    t = tiles[0]                                                  # the detailed patch first
    assert 800 - 64 <= (t.x0 + t.x1) / 2 <= 960 + 64 and 200 - 64 <= (t.y0 + t.y1) / 2 <= 360 + 64


def test_our_box_k_is_taken_only_from_a_plan_that_matches_runstack():
    probe = {"used": 100, "regionDetail": [{"kept": 80, "noiseOnly": False}, {"kept": 0, "noiseOnly": True}]}
    keff = {"stacked": 100, "regionKept": [80, 0], "keffBox": 91.5, "box": [0, 10, 0, 10]}
    assert benchset.ours_frames_box(probe, keff) == (91.5, "")
    k, why = benchset.ours_frames_box(probe, dict(keff, regionKept=[79, 0]))
    assert k is None and "differs from RunStack's" in why
    k, why = benchset.ours_frames_box(probe, None)
    assert k is None and "not computed" in why
    # the flip check: used x the unweighted mean kept fraction, a no-frame region counting 1
    assert benchset.ours_frames_mean(probe) == pytest.approx(100 * (0.8 + 1.0) / 2)
