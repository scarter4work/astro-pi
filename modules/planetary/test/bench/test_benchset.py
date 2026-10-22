"""The benchmark set's recorded facts hold on the NAS (read-only), and the entry point
run-stack-bench.sh will call works end to end."""
import os
import subprocess

import pytest

import bayer
import benchset
import refs
from conftest import NAS, nas_path

pytestmark = [pytest.mark.nas,
              pytest.mark.skipif(not os.path.isdir(NAS), reason=f"NAS not mounted at {NAS}")]


def test_set_covers_planets_lunar_and_diagnosis():
    kinds = [c.kind for c in benchset.BENCHMARK]
    assert kinds.count("planet") >= 3 and kinds.count("lunar") >= 3
    assert {c.expect for c in benchset.BENCHMARK if c.kind == "diagnosis"} == {"NoTarget", "Clipped"}
    assert all(c.references for c in benchset.BENCHMARK)


@pytest.mark.parametrize("c", [c for c in benchset.BENCHMARK if c.expect == "stack"], ids=lambda c: c.id)
def test_every_stacked_capture_is_debayered_with_a_sourced_pattern(c):
    assert c.bayer in ("RGGB", "GRBG", "GBRG", "BGGR") and c.bayer_source


@pytest.mark.ffmpeg
@pytest.mark.parametrize("c", benchset.BENCHMARK, ids=lambda c: c.id)
def test_recorded_facts_match_the_nas(c):
    video = nas_path(c.video)
    assert os.path.exists(video + ".txt") == c.sidecar, "Bayer sidecar presence"
    for p in c.references:
        assert os.path.isfile(nas_path(p)), p
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height,nb_frames", "-of", "csv=p=0", video],
                         check=True, capture_output=True, text=True).stdout.strip()
    w, h, n = map(int, out.split(","))
    assert (w, h) == c.size and n == c.frames


@pytest.mark.ffmpeg
@pytest.mark.parametrize("c", [c for c in benchset.BENCHMARK if c.bayer], ids=lambda c: c.id)
def test_recorded_pattern_matches_the_frames(c):
    """The green phase is measured on every capture; the red/blue assignment follows the warm-
    target rule unless the record says why not (multi-band Lqef captures)."""
    m = bayer.measure_video(nas_path(c.video), c.size)
    print(f"\n{c.id}: recorded {c.bayer} ({c.bayer_source}); measured {m}")
    greens = lambda p: tuple(i for i, ch in enumerate(p) if ch == "G")   # noqa: E731
    assert greens(m.pattern) == greens(c.bayer)
    if c.bayer_source.startswith("measured, warm-target rule"):
        assert m.pattern == c.bayer


@pytest.mark.parametrize("c", benchset.BENCHMARK, ids=lambda c: c.id)
def test_reference_factors_come_from_size_and_load(c):
    for p in c.references:
        ref = refs.load_reference(nas_path(p), frame_size=c.size)
        assert ref.factor in (1.0, 1.5, 3.0)


def test_compare_capture_end_to_end():
    """With AS_P20 standing in for our stack, 'ours' must measure exactly as AS_P20 does."""
    ours = nas_path("2_12_2023", "AS_P20", "2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png")
    rows = benchset.compare_capture("2023-02-11-jupiter", ours)
    assert [r.label for r in rows] == ["ours", "AS_P20", "AS_P25 (drizzled x3, resampled)"]
    assert not any(r.result.mosaic for r in rows)            # debayered: no mosaic mode
    assert rows[0].result.value == pytest.approx(rows[1].result.value, rel=1e-9)
    assert rows[1].registration.shift == pytest.approx((0.0, 0.0), abs=0.05)


def test_compare_capture_refuses_a_diagnosis_case():
    with pytest.raises(ValueError, match="diagnosis"):
        benchset.compare_capture("2026-01-30-saturn-empty", "/nonexistent.fits")
