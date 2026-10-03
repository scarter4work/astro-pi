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
# NUKEX_E2E_MANIFEST points the run at another manifest (an attribution or
# bisect run with instance_overrides); the default is the repo corpus.
MANIFEST="${NUKEX_E2E_MANIFEST:-${REPO}/test/fixtures/e2e_manifest.json}"
BUILD_DIR="${NUKEX_BUILD_DIR:-${REPO}/build}"
E2E_LOG="${BUILD_DIR}/e2e.log"

# ── Borrow the module, never leave it ────────────────────────────────────
#
# PixInsight only loads modules from <PI>/bin, so verifying a build means
# putting one there. It does NOT mean leaving one there. The rule, from the
# user, 2026-09-07:
#
#   "the rule is you don't install. you create the image that i download"
#   "so then install it, run it, but remove it. i don't test till i pull it
#    from github"
#
# A build left in <PI>/bin silently becomes the thing being tested, and the
# release is then never proved through the path a real user takes. So the
# install is scoped to this script and torn down in an EXIT trap, which fires
# on failure, on the timeout, and on Ctrl-C.
#
# Never sudo: /opt/PixInsight/bin is user-owned, and a root-owned module is
# one PixInsight's own updater cannot replace.
PI_BIN="${NUKEX_PI_BIN:-/opt/PixInsight/bin}"
MODULE_SRC="${BUILD_DIR}/src/module/NukeX-pxm.so"
MODULE_DST="${PI_BIN}/NukeX-pxm.so"
SIGN_DST="${PI_BIN}/NukeX-pxm.xsgn"

# The user may have a RELEASE installed from GitHub -- that is the whole point
# of the rule, and it is theirs. Never destroy it: stash it and put it back.
STASH=""
if [ -e "${MODULE_DST}" ] || [ -e "${SIGN_DST}" ]; then
    STASH="$(mktemp -d)"
    echo "NukeX E2E: an installed module is present; stashing it to ${STASH}"
    [ -e "${MODULE_DST}" ] && mv "${MODULE_DST}" "${STASH}/"
    [ -e "${SIGN_DST}" ]   && mv "${SIGN_DST}"   "${STASH}/"
fi
if [ ! -f "${MODULE_SRC}" ]; then
    echo "ERROR: ${MODULE_SRC} not found. Build the module first."
    exit 1
fi

KEYS="${NUKEX_SIGN_KEYS:-/home/scarter4work/projects/keys/scarter4work_keys.xssk}"
if [ -n "${NUKEX_SIGN_PASS:-}" ]; then
    PASS="${NUKEX_SIGN_PASS}"
elif [ -r "${NUKEX_SIGN_PASS_FILE:-/tmp/.pi_codesign_pass}" ]; then
    PASS="$(cat "${NUKEX_SIGN_PASS_FILE:-/tmp/.pi_codesign_pass}")"
else
    echo "ERROR: neither NUKEX_SIGN_PASS env nor NUKEX_SIGN_PASS_FILE is set."
    echo "Set one, or put the password in /tmp/.pi_codesign_pass."
    exit 1
fi

uninstall_module() {
    rm -f "${MODULE_DST}" "${SIGN_DST}"
    if [ -n "${STASH}" ]; then
        # Put the user's own installation back exactly as it was.
        [ -e "${STASH}/NukeX-pxm.so" ]   && mv "${STASH}/NukeX-pxm.so"   "${MODULE_DST}"
        [ -e "${STASH}/NukeX-pxm.xsgn" ] && mv "${STASH}/NukeX-pxm.xsgn" "${SIGN_DST}"
        rmdir "${STASH}" 2>/dev/null || true
        echo "NukeX E2E: borrowed module removed; the installed one is restored."
    else
        echo "NukeX E2E: module removed from ${PI_BIN} (nothing is left installed)."
    fi
}
trap uninstall_module EXIT

echo "NukeX E2E: borrowing ${MODULE_SRC} -> ${MODULE_DST} for this run only."
cp "${MODULE_SRC}" "${MODULE_DST}"
"${PI_BIN}/PixInsight.sh" --sign-module-file="${MODULE_DST}" \
    --xssk-file="${KEYS}" --xssk-password="${PASS}" >/dev/null

# Run a single case by name. The full v5 corpus takes hours across four
# stacks of real data; being able to do one case at a time keeps a long run
# from being all-or-nothing.
#   NUKEX_E2E_ONLY=bayer_nb_hao3_m16 make e2e-regen
ONLY_ARG=""
if [ -n "${NUKEX_E2E_ONLY:-}" ]; then
    ONLY_ARG=",only=${NUKEX_E2E_ONLY}"
    echo "NukeX E2E: restricted to case '${NUKEX_E2E_ONLY}'."
fi

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

# The frame cache too. NukeX creates a fresh nukex_cache_XXXXXX subdirectory
# per run and does not always remove it, so without this the cache directory
# grows by gigabytes per run. That went unnoticed while the cache lived in
# /tmp (tmpfs, wiped on reboot) right up until it exhausted RAM mid-run.
CACHE_DIR="$(python3 -c 'import json,sys,os; m=json.load(open(sys.argv[1])); print(m.get("cache_dir", os.path.expanduser("~/.cache/nukex_e2e_frames")))' "${MANIFEST}")"
if [ -n "${CACHE_DIR}" ] && [ "${CACHE_DIR}" != "/" ]; then
    rm -rf "${CACHE_DIR:?}"/nukex_cache_* 2>/dev/null || true
    mkdir -p "${CACHE_DIR}"
fi

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
        "-r=${REPO}/tools/validate_e2e.js,manifest=${MANIFEST}${REGEN_ARG}${ONLY_ARG}" \
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
