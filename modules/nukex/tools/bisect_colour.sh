#!/usr/bin/env bash
# Build ONE old release's module and measure M16's colour with the CURRENT
# harness. The old checkouts have no saturation measurement, so the module
# comes from the tag and validate_e2e.js/e2e_manifest.json come from HEAD.
set -euo pipefail
TAG="${1:?tag}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Worktrees and their builds live under ~/.cache, NOT /tmp: /tmp is tmpfs on
# this machine and a NukeX build there has already exhausted RAM once.
WT="${NUKEX_BISECT_DIR:-$HOME/.cache/nukex_bisect}/$TAG"

git -C "$REPO" worktree add -f --detach "$WT" "$TAG" >/dev/null 2>&1 || true
mkdir -p "$WT/build-bisect"
( cd "$WT/build-bisect" && cmake .. -DNUKEX_BUILD_MODULE=ON -DNUKEX_BUILD_TESTS=OFF \
    -DPCLDIR="$HOME/PCL" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
  && cmake --build . --target NukeX-pxm -j32 >/dev/null 2>&1 ) \
  || { echo "$TAG: BUILD FAILED"; exit 2; }

# The signing password is NEVER written by this script. run_e2e.sh reads
# NUKEX_SIGN_PASS or NUKEX_SIGN_PASS_FILE (default /tmp/.pi_codesign_pass);
# supply one of those before running. A credential embedded in a repo file is
# one `git add -A` away from a public remote -- which has already happened once
# in this repo, see docs/superpowers/plans/2026-09-02-*.md line 2400.
if [ -z "${NUKEX_SIGN_PASS:-}" ] && [ ! -r "${NUKEX_SIGN_PASS_FILE:-/tmp/.pi_codesign_pass}" ]; then
    echo "ERROR: set NUKEX_SIGN_PASS, or put the password in /tmp/.pi_codesign_pass." >&2
    exit 2
fi
NUKEX_BUILD_DIR="$WT/build-bisect" NUKEX_E2E_ONLY=bayer_nb_hao3_m16 \
    "$REPO/tools/run_e2e.sh" > "$HOME/.cache/bisect_$TAG.log" 2>&1 || true

python3 - "$TAG" <<'PY'
import json, os, sys
tag = sys.argv[1]
d = json.load(open(os.path.expanduser('~/.cache/nukex_e2e/e2e_report.json')))
for c in d['cases']:
    if c['name'] != 'bayer_nb_hao3_m16': continue
    sat = (c.get('primary') or {}).get('pixel_hashes', {}).get('stretched_bright_saturation')
    verdict = 'GOOD (colour present)' if (sat or 0) >= 0.05 else 'BAD (achromatic)'
    print(f"{tag}: bright saturation {sat}  -> {verdict}")
PY
