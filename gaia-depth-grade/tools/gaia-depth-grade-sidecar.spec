# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for the frozen gaia-depth-grade sidecar (one-file binary).
#
# Data files for the astro stack: astropy's CDS/IERS tables and the
# photutils/astroquery package resources. Hidden imports and compiled extensions
# (erfa, scipy, numpy) come from PyInstaller's bundled contrib hooks, which fire
# automatically from the imports Analysis discovers in the cli chain. `--version`
# (which imports that whole chain) is the runtime proof the bundle is complete.
#
# matplotlib is a BUILD-TIME-ONLY dependency: the contrib hook-astropy.py runs
# collect_submodules('astropy'), which imports astropy.visualization.wcsaxes,
# whose __init__ does pytest.importorskip("matplotlib") and raises (aborting the
# build) if matplotlib is absent. With it installed the import resolves; we then
# exclude matplotlib AND the unused astropy.visualization tree below so neither
# adds a byte to the shipped binary.
from PyInstaller.utils.hooks import collect_data_files, copy_metadata

datas = []
for pkg in ("astropy", "astroquery", "photutils"):
    datas += collect_data_files(pkg)
# photutils (and friends) introspect their own dist-info at import via
# importlib.metadata.requires(...) — PyInstaller bundles code but not metadata
# unless told, so copy each package's *.dist-info or the import aborts with
# PackageNotFoundError.
for pkg in ("photutils", "astropy", "astroquery"):
    datas += copy_metadata(pkg)

a = Analysis(
    ["sidecar_entry.py"],
    pathex=[],                       # gaia_depth_grade resolves via the venv (editable install)
    binaries=[],
    datas=datas,
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["tkinter", "matplotlib", "astropy.visualization", "PyQt5", "PyQt6",
              "PySide2", "PySide6", "IPython", "pytest"],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="gaia-depth-grade-sidecar",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    runtime_tmpdir=None,
    console=True,
)
