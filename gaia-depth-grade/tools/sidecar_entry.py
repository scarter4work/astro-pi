"""Frozen-sidecar entry point: dispatch the gaia_depth_grade CLI verbatim.

PyInstaller freezes this into the `gaia-depth-grade-sidecar` binary; the PJSR
bootstrap invokes `gaia-depth-grade-sidecar <subcommand> ...` exactly like the
source CLI (`python -m gaia_depth_grade.cli <subcommand> ...`), so there is no
behavioural divergence between frozen and source — same argv, same main().
"""
import sys

from gaia_depth_grade.cli import main

if __name__ == "__main__":
    sys.exit(main())
