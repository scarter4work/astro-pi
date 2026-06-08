#!/usr/bin/env bash
#
# NukeX v4 — E2E harness driver.
#
# Usage:
#   tools/run_e2e.sh              # verify against committed goldens
#   tools/run_e2e.sh regen        # refresh goldens from current binary
#
# Invoked by `make e2e`.  Kept in its own script because CMake's COMMAND
# substitution does not handle nested `$$`/bash-c quoting cleanly.

set -euo pipefail   # pipefail so `PixInsight.sh … | tee` surfaces PI crashes

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${REPO}/test/fixtures/e2e_manifest.json"
BUILD_DIR="${NUKEX_BUILD_DIR:-${REPO}/build}"
E2E_LOG="${BUILD_DIR}/e2e.log"

REGEN_ARG=""
if [ "${1:-}" = "regen" ] || [ "${NUKEX_E2E_REGEN:-}" = "1" ]; then
    REGEN_ARG=",regen=1"
    echo "NukeX E2E: regen mode — goldens in test/fixtures/golden/ WILL be rewritten."
fi

rm -f /tmp/nukex_e2e_meta.txt /tmp/nukex_e2e_console.log
# Belt-and-braces: wipe the harness output root so there is no chance of an
# overwrite prompt from PI's saveAs during a regen.  The harness re-creates
# per-case subdirs as needed.
OUTPUT_ROOT="$(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m.get("output_root","/tmp/nukex_e2e"))' "${MANIFEST}")"
rm -rf "${OUTPUT_ROOT}"

# Cap the run so a hung harness can't block CI forever.  60 min is ~3×
# the longest observed good E2E on NGC7635 (primary + 3 sweeps ≈ 20 min)
# and ~1.7× the worst-case fresh-GPU-compile + cold-cache baseline.
NUKEX_E2E_TIMEOUT="${NUKEX_E2E_TIMEOUT:-3600}"

# Phase 8: suppress the post-Execute rating popup.  Without this, the
# dialog would block the harness and the run would time out. The popup
# is purely a user-opinion capture step; it has no effect on the stacked
# / stretched pixel output the E2E validator hashes.
export NUKEX_PHASE8_NO_POPUP=1
# --default-modules forces PI to rescan its bin/ directory on startup and
# re-register every module found there.  Without this, PI relies on its
# persistent "installed modules" list from user settings, which can fall
# out of sync with the on-disk binaries (e.g. after a sign + reinstall
# cycle during dev).  For a test harness that installs fresh modules, the
# fresh-scan behaviour is what we want every run.
timeout --kill-after=30s "${NUKEX_E2E_TIMEOUT}" \
    /opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit --default-modules \
        "-r=${REPO}/tools/validate_e2e.js,manifest=${MANIFEST}${REGEN_ARG}" \
        2>&1 | tee "${E2E_LOG}"

echo ""
echo "========================================================================"
if [ -f /tmp/nukex_e2e_meta.txt ]; then
    cat /tmp/nukex_e2e_meta.txt
    STATUS="$(grep '^STATUS ' /tmp/nukex_e2e_meta.txt | awk '{print $2}')"
else
    echo "STATUS fail"
    echo "REASON harness did not write meta file"
    STATUS="fail"
fi
echo "========================================================================"

test "${STATUS}" = "ok"
