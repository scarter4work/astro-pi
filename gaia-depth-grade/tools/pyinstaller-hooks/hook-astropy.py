# Replaces pyinstaller-hooks-contrib's hook-astropy.py for the gaia-depth-grade
# sidecar (hooks in the spec's hookspath take precedence over contrib hooks).
#
# The contrib hook bundles ALL of astropy: collect_submodules('astropy') (which
# imports astropy.visualization at build time and therefore needs matplotlib)
# and collect_data_files('astropy') (which includes ~1 MB of test data). This
# version keeps the same "collect every submodule" safety net for the astropy
# code the sidecar can reach (registry-based readers such as Table.read(...,
# format="ascii.ecsv") are found by import name, not by static analysis), but
# leaves out the subpackages the sidecar never imports. Run-time import traces
# of every sidecar subcommand (footprint/prepare offline+online/preview/render/
# nebula/grade) never load any of them; the frozen smoke test and the
# end-to-end parity check against sidecar 1.0.7 prove nothing else needs them.
from PyInstaller.utils.hooks import collect_data_files, collect_submodules, copy_metadata

_UNUSED = (
    "astropy.visualization",   # plotting; needs matplotlib
    "astropy.cosmology",
    "astropy.timeseries",
    "astropy.samp",
)
# `import astropy` itself imports astropy.tests.runner (for astropy.test()), so
# that one module of astropy.tests stays; every unit-test package goes.
_NEEDED_TEST_MODULES = ("astropy.tests", "astropy.tests.runner")


def _keep(name):
    if any(name == p or name.startswith(p + ".") for p in _UNUSED):
        return False
    if name in _NEEDED_TEST_MODULES:
        return True
    parts = name.split(".")
    return "tests" not in parts and "conftest" not in parts


hiddenimports = collect_submodules("astropy", filter=_keep)
hiddenimports += ["numpy.lib.recfunctions"]

datas = collect_data_files("astropy", excludes=["**/tests/**", "**/tests"])
# astropy's PLY-generated parser tables are .py data files loaded by path.
datas += [(p, t) for p, t in collect_data_files("astropy", include_py_files=True)
          if p.endswith(("_parsetab.py", "_lextab.py"))]
datas += copy_metadata("astropy")
datas += copy_metadata("numpy")
