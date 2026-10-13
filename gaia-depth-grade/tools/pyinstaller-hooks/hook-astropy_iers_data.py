# Replaces pyinstaller-hooks-contrib's hook-astropy_iers_data.py, which bundles
# the package's Earth-orientation (IERS) tables (~9 MB uncompressed).
#
# `import astropy.coordinates` imports astropy_iers_data, but only for the file
# PATHS it defines; the tables are read solely for UT1/polar-motion work (Time
# UT1 scales, AltAz/ITRS transforms), which the sidecar never does. A run-time
# trace of every sidecar subcommand opens none of them. So the module ships
# (its import must succeed) and the tables do not. Should a future code path
# need IERS data, astropy fails loudly opening the missing table -- it is not a
# silent degradation.
datas = []
