import json
import os
import stat
from pathlib import Path

import pytest

from astrometa import db, manifest


def _frame(conn, h, path, obj_name=None):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition, filter, camera)
        VALUES (?,?,?,10,1.0,'light','present','HaO3','ZWO ASI585MC Air')""",
        (h, path, Path(path).name))
    conn.commit()


def test_export_writes_manifest(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    out = manifest.export_dir(conn, leaf)
    assert out.name == ".astro-manifest.json"
    doc = json.loads(out.read_text())
    assert doc["frames"][0]["content_hash"] == "h1"
    assert doc["frames"][0]["filter"] == "HaO3"


def test_roundtrip_reconstructs_labels_after_db_loss(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "a.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    out = manifest.export_dir(conn, leaf)

    fresh = db.connect(tmp_path / "b.sqlite"); db.init_schema(fresh)
    assert manifest.import_file(fresh, out) == 1
    row = fresh.execute("SELECT content_hash, filter FROM frames").fetchone()
    assert row == ("h1", "HaO3")


def test_filename_with_space_survives_roundtrip(tmp_path):
    leaf = tmp_path / "d" / "NGC 7635"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "a.sqlite"); db.init_schema(conn)
    _frame(conn, "h9", str(leaf / "Light_NGC 7635_300.0s_0001.fit"))
    out = manifest.export_dir(conn, leaf)
    fresh = db.connect(tmp_path / "b.sqlite"); db.init_schema(fresh)
    manifest.import_file(fresh, out)
    assert fresh.execute("SELECT filename FROM frames").fetchone()[0] \
        == "Light_NGC 7635_300.0s_0001.fit"


def test_importing_twice_does_not_duplicate_rows(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "a.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    out = manifest.export_dir(conn, leaf)

    fresh = db.connect(tmp_path / "b.sqlite"); db.init_schema(fresh)
    manifest.import_file(fresh, out)
    manifest.import_file(fresh, out)
    count = fresh.execute("SELECT COUNT(*) FROM frames").fetchone()[0]
    assert count == 1


def test_exporting_twice_produces_the_same_manifest(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "a.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    manifest.export_dir(conn, leaf)
    doc1 = json.loads((leaf / manifest.MANIFEST_NAME).read_text())
    manifest.export_dir(conn, leaf)
    doc2 = json.loads((leaf / manifest.MANIFEST_NAME).read_text())
    # Compare only "frames", not the whole doc: generated_at is a
    # wall-clock timestamp that legitimately differs between the two
    # calls even though nothing about the underlying data changed --
    # asserting doc1 == doc2 would make this test flaky (or, worse,
    # tempt a future reader to "fix" the flake by freezing time rather
    # than by narrowing the comparison to what idempotency actually
    # promises: the same frames, not the same generated_at).
    assert doc1["frames"] == doc2["frames"]


# --- Correction 1: the LIKE prefix must be escaped --------------------

def test_like_escape_rejects_underscore_wildcard_sibling(tmp_path):
    """
    A naive f"{leaf_dir}/%" LIKE pattern treats '_' as a single-character
    wildcard, so a sibling path differing from the leaf ONLY where an
    underscore sits would incorrectly match. This archive's real root
    (/mnt/qnap/astro_data/...) has exactly this shape, and so do many
    target directory names.
    """
    leaf = tmp_path / "astro_data" / "Sh2-106"; leaf.mkdir(parents=True)
    # Same length as "astro_data", differing only at the underscore's
    # position -- an unescaped '_' wildcard matches 'X' here just as
    # readily as it matches the real '_'.
    sibling = tmp_path / "astroXdata" / "Sh2-106"; sibling.mkdir(parents=True)

    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    _frame(conn, "h2", str(sibling / "Light_Sh2-106_0002.fit"))

    out = manifest.export_dir(conn, leaf)
    doc = json.loads(out.read_text())
    hashes = {f["content_hash"] for f in doc["frames"]}
    assert hashes == {"h1"}


# --- Correction 2: export_all must work against a read-only archive ---

def test_export_all_writes_into_each_leaf_by_default(tmp_path):
    leaf1 = tmp_path / "2026-09-08" / "Sh2-106"; leaf1.mkdir(parents=True)
    leaf2 = tmp_path / "2026-09-09" / "M 20"; leaf2.mkdir(parents=True)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf1 / "Light_Sh2-106_0001.fit"))
    _frame(conn, "h2", str(leaf2 / "Light_M 20_0001.fit"))

    n = manifest.export_all(conn)
    assert n == 2
    assert (leaf1 / manifest.MANIFEST_NAME).exists()
    assert (leaf2 / manifest.MANIFEST_NAME).exists()


def test_export_all_dry_run_counts_without_writing(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))

    n = manifest.export_all(conn, dry_run=True)
    assert n == 1
    assert not (leaf / manifest.MANIFEST_NAME).exists()


def test_export_all_mirrors_leaf_structure_under_out_dir(tmp_path):
    archive = tmp_path / "archive"
    leaf = archive / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    out_dir = tmp_path / "manifests"; out_dir.mkdir()
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))

    n = manifest.export_all(conn, out_dir=out_dir)
    assert n == 1

    found = list(out_dir.rglob(manifest.MANIFEST_NAME))
    assert len(found) == 1
    # Nothing written into the archive tree itself (read-only on the
    # deployment host).
    assert not (leaf / manifest.MANIFEST_NAME).exists()
    doc = json.loads(found[0].read_text())
    assert doc["frames"][0]["content_hash"] == "h1"
    assert doc["leaf_dir"] == str(leaf)
    # The leaf's own path is preserved in the mirror, not collapsed away.
    assert "Sh2-106" in str(found[0])
    assert "2026-09-08" in str(found[0])


def test_export_all_out_dir_avoids_basename_collision(tmp_path):
    """
    Two different leaf directories can share a basename (a reused target
    or panel name under different nights) -- flattening every manifest
    into one out_dir would collide on filename and silently keep only
    the last one written. Mirroring the full leaf path avoids that.
    """
    archive = tmp_path / "archive"
    leaf1 = archive / "2026-09-08" / "Sh2-106"; leaf1.mkdir(parents=True)
    leaf2 = archive / "2026-09-09" / "Sh2-106"; leaf2.mkdir(parents=True)
    out_dir = tmp_path / "manifests"; out_dir.mkdir()
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf1 / "Light_Sh2-106_0001.fit"))
    _frame(conn, "h2", str(leaf2 / "Light_Sh2-106_0002.fit"))

    n = manifest.export_all(conn, out_dir=out_dir)
    assert n == 2

    found = sorted(out_dir.rglob(manifest.MANIFEST_NAME))
    assert len(found) == 2  # both survive -- no basename collision
    leaf_dirs = {json.loads(p.read_text())["leaf_dir"] for p in found}
    assert leaf_dirs == {str(leaf1), str(leaf2)}


def test_export_all_rejects_relative_leaf_dir_that_escapes_out_dir(tmp_path):
    """
    A frame path is normally an absolute DB-sourced path, but export_dir
    and export_all are public, unvalidated entry points -- a relative
    leaf directory containing '..' segments is not collapsed by
    joinpath alone, so out_dir.joinpath(*parts) could otherwise walk the
    manifest target back out of out_dir entirely before export_dir
    mkdirs it.
    """
    out_dir = tmp_path / "manifests"; out_dir.mkdir()
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "somedir/../../escape/Light_Sh2-106_0001.fit")

    with pytest.raises(ValueError, match="out_dir"):
        manifest.export_all(conn, out_dir=out_dir)

    # Refused before any filesystem call -- nothing created anywhere.
    assert list(out_dir.rglob(manifest.MANIFEST_NAME)) == []
    assert not (out_dir / "somedir").exists()
    assert not (tmp_path / "escape").exists()


def test_export_all_rejects_leaf_dir_that_resolves_outside_out_dir(tmp_path):
    """
    Even a well-formed absolute leaf_dir (as every real DB row is) can
    carry more '..' segments than the anchor-strip accounts for, and
    still resolve outside out_dir after being rejoined beneath it.
    """
    out_dir = tmp_path / "manifests"; out_dir.mkdir()
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    evil_path = "/" + "../" * 6 + "escape/Light_Sh2-106_0001.fit"
    _frame(conn, "h1", evil_path)

    with pytest.raises(ValueError, match="out_dir"):
        manifest.export_all(conn, out_dir=out_dir)

    assert list(out_dir.rglob(manifest.MANIFEST_NAME)) == []


def test_export_dir_raises_loudly_on_write_failure(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))

    readonly_out = tmp_path / "readonly"; readonly_out.mkdir()
    os.chmod(readonly_out, stat.S_IRUSR | stat.S_IXUSR)  # read+execute, no write
    try:
        with pytest.raises(OSError) as exc_info:
            manifest.export_dir(conn, leaf, out_dir=readonly_out)
        assert str(leaf) in str(exc_info.value)
    finally:
        os.chmod(readonly_out, stat.S_IRWXU)  # let tmp_path clean up


def test_export_all_raises_and_names_the_failing_leaf(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))

    out_dir = tmp_path / "readonly"; out_dir.mkdir()
    os.chmod(out_dir, stat.S_IRUSR | stat.S_IXUSR)  # read+execute, no write
    try:
        with pytest.raises(OSError) as exc_info:
            manifest.export_all(conn, out_dir=out_dir)
        assert str(leaf) in str(exc_info.value)
    finally:
        os.chmod(out_dir, stat.S_IRWXU)  # let tmp_path clean up
