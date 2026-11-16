#!/bin/bash
# sign.sh - Sign the EZ Stretch BSC scripts (headless, no running PI). Called by the astro-pi
# release.sh, which packages them and signs the ONE manifest, repository/updates.xri at the repo root.
# (This folder's own repository/ and its manifest were retired 2026-11-16: one signed repository only.)
#
# Usage:
#   ./tools/sign.sh scripts    # sign the .js scripts (-> .xsgn)
#   ./tools/sign.sh scripts EZHazeKill   # sign only the named scripts (release.sh passes the changed ones)
#
# Requires the signing password in /tmp/.pi_codesign_pass
#
# The signer is PixInsight-native, so signatures are guaranteed PI-valid:
#   - *.js scripts -> tools/SignScriptsNative.js, run via PI automation mode,
#                     which calls Security.generateScriptSignatureFile() - the
#                     exact code path PI's loader verifies against.
#
# DO NOT use tools/pi_codesign.py for scripts. It reimplements Ed25519 from the
# outside and guessed the signed-message format wrong: signatures were internally
# self-consistent but PI rejected every script with "Invalid code signature".
# (The key was correct; only the message canonicalization differed.) Native
# signing sidesteps the whole problem - see git history / 2026-06 debugging.
#
# IMPORTANT ordering: .xsgn files embed a timestamp, so signing is NOT
# deterministic - each run changes the file and therefore the package SHA1.
# release.sh signs scripts -> packages -> hashes -> signs the manifest last.
#
# History: the old approach ran CLICodeSign.js via `--automation-mode --run=`.
# Unreliable here - automation-mode routes console output to PI's internal
# console (not stdout) and the run flag silently no-ops in some states, so
# signing appeared to succeed while writing nothing.

set -u

TARGET="${1:-scripts}"
[ $# -gt 0 ] && shift
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PI_DIR="${PIXINSIGHT_DIR:-/opt/PixInsight}"
PI="$PI_DIR/bin/PixInsight.sh"
# Every PixInsight launch goes through pi_headless (private Xvfb, never the
# user's desktop) -- see tools/pi-headless.sh at the astro-pi root.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/tools/pi-headless.sh"
KEYS_XSSK="/home/scarter4work/projects/keys/scarter4work_keys.xssk"
PASS_FILE="/tmp/.pi_codesign_pass"
NATIVE_SIGNER="$SCRIPT_DIR/SignScriptsNative.js"
SIGN_RESULT="/tmp/.ez_sign_result.json"
SIGN_REQUEST="/tmp/.ez_sign_request.json"   # the script names SignScriptsNative.js signs
SCRIPTS_DIR="$PROJECT_DIR/src/scripts/EZ Stretch BSC"

# Scripts to sign (.js -> .xsgn)
SCRIPTS=( EZStretch EZHazeKill )
# Only these, when names follow the target ("scripts" only): an unchanged script is never re-signed
# (its .xsgn embeds a timestamp, so a re-sign rewrites a published signature).
if [ $# -gt 0 ]; then
    [ "$TARGET" = scripts ] || { echo "ERROR: script names are only taken with the 'scripts' target" >&2; exit 1; }
    for n in "$@"; do
        printf '%s\n' "${SCRIPTS[@]}" | grep -qxF "$n" || { echo "ERROR: $n is not one of: ${SCRIPTS[*]}" >&2; exit 1; }
    done
    SCRIPTS=( "$@" )
fi

if [ ! -f "$PASS_FILE" ]; then
    echo "ERROR: signing password not found at $PASS_FILE" >&2
    echo "  Write it first, e.g.:  python3 -c \"open('$PASS_FILE','w').write('<pass>')\"; chmod 600 $PASS_FILE" >&2
    exit 1
fi

sign_scripts() {
    echo "Scripts (PixInsight native, automation mode):"
    if [ ! -f "$NATIVE_SIGNER" ]; then
        echo "  ERROR: native signer not found: $NATIVE_SIGNER" >&2; exit 1
    fi
    # The script (SignScriptsNative.js) writes $SIGN_RESULT; we trust that file,
    # not the exit code, because PI automation-mode console output never reaches
    # stdout and the process exits 0 even on a silent no-op.
    rm -f "$SIGN_RESULT"
    python3 -c 'import json,sys; json.dump({"scripts": sys.argv[2:]}, open(sys.argv[1], "w"))' "$SIGN_REQUEST" "${SCRIPTS[@]}" \
        || { echo "  ERROR: could not write $SIGN_REQUEST" >&2; exit 1; }
    LD_LIBRARY_PATH="$PI_DIR/bin/lib:$PI_DIR/bin:${LD_LIBRARY_PATH:-}" \
        pi_headless "$PI" -n --automation-mode --no-startup-scripts --no-startup-check-updates \
              --no-startup-gui-messages -r="$NATIVE_SIGNER" --force-exit >/dev/null 2>&1

    if [ ! -f "$SIGN_RESULT" ]; then
        echo "  ERROR: signer produced no result file - did PixInsight run? ($SIGN_RESULT)" >&2; exit 1
    fi
    if ! python3 - "$SIGN_RESULT" "$SCRIPTS_DIR" "${SCRIPTS[@]}" <<'PY'
import json, os, sys
res = json.load(open(sys.argv[1])); want = set(sys.argv[3:])
ok = res.get("ok") and set(res.get("signed", [])) == want and not res.get("failed")
if os.path.realpath(res.get("dir") or "") != os.path.realpath(sys.argv[2]):
    print(f"  ERROR: signer signed {res.get('dir')!r}, expected {sys.argv[2]!r}", file=sys.stderr); ok = False
for s in res.get("signed", []): print(f"  Signed: {s}")
for f in res.get("failed", []): print(f"  FAILED: {f}", file=sys.stderr)
if res.get("error"): print(f"  ERROR: {res['error']}", file=sys.stderr)
sys.exit(0 if ok else 1)
PY
    then
        echo "  ERROR: native script signing failed (see above)" >&2; exit 1
    fi
}

echo "Signing EZ Stretch BSC ($TARGET)..."
echo ""
case "$TARGET" in
    scripts) sign_scripts ;;
    *) echo "Unknown target '$TARGET' (use: scripts; the manifest is signed by the astro-pi release.sh)" >&2; exit 1 ;;
esac

echo ""
echo "Done."
