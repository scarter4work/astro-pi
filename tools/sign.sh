#!/bin/bash
# sign.sh - Sign EZ Stretch BSC scripts and/or manifest (headless, no running PI)
#
# Usage:
#   ./tools/sign.sh            # sign scripts AND manifest
#   ./tools/sign.sh scripts    # sign only the .js scripts (-> .xsgn)
#   ./tools/sign.sh xri        # sign only repository/updates.xri
#
# Requires the signing password in /tmp/.pi_codesign_pass
#
# Both signers are PixInsight-native, so signatures are guaranteed PI-valid:
#   - updates.xri  -> native `PixInsight.sh --sign-xml-file` (NukeX pattern).
#                     A short-lived core process.
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
# So the release order must be:  sign scripts -> build-packages -> sign xri.
# (build-packages.sh runs this in the correct order for you.)
#
# History: the old approach ran CLICodeSign.js via `--automation-mode --run=`.
# Unreliable here - automation-mode routes console output to PI's internal
# console (not stdout) and the run flag silently no-ops in some states, so
# signing appeared to succeed while writing nothing.

set -u

TARGET="${1:-all}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PI_DIR="${PIXINSIGHT_DIR:-/opt/PixInsight}"
PI="$PI_DIR/bin/PixInsight.sh"
KEYS_XSSK="/home/scarter4work/projects/keys/scarter4work_keys.xssk"
PASS_FILE="/tmp/.pi_codesign_pass"
NATIVE_SIGNER="$SCRIPT_DIR/SignScriptsNative.js"
SIGN_RESULT="/tmp/.ez_sign_result.json"
SCRIPTS_DIR="$PROJECT_DIR/src/scripts/EZ Stretch BSC"
XRI="$PROJECT_DIR/repository/updates.xri"

# Scripts to sign (.js -> .xsgn)
SCRIPTS=( EZStretch EZDonutRepair EZHazeKill )

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
    LD_LIBRARY_PATH="$PI_DIR/bin/lib:$PI_DIR/bin:${LD_LIBRARY_PATH:-}" \
        "$PI" -n --automation-mode --no-startup-scripts --no-startup-check-updates \
              --no-startup-gui-messages -r="$NATIVE_SIGNER" --force-exit >/dev/null 2>&1

    if [ ! -f "$SIGN_RESULT" ]; then
        echo "  ERROR: signer produced no result file - did PixInsight run? ($SIGN_RESULT)" >&2; exit 1
    fi
    if ! python3 - "$SIGN_RESULT" "${SCRIPTS[@]}" <<'PY'
import json, sys
res = json.load(open(sys.argv[1])); want = set(sys.argv[2:])
ok = res.get("ok") and set(res.get("signed", [])) >= want and not res.get("failed")
for s in res.get("signed", []): print(f"  Signed: {s}")
for f in res.get("failed", []): print(f"  FAILED: {f}", file=sys.stderr)
if res.get("error"): print(f"  ERROR: {res['error']}", file=sys.stderr)
sys.exit(0 if ok else 1)
PY
    then
        echo "  ERROR: native script signing failed (see above)" >&2; exit 1
    fi
}

sign_xri() {
    echo "Manifest (PixInsight --sign-xml-file):"
    # The native signer appends a <Signature>; strip any existing one first.
    sed -i '/<Signature developerId=/d' "$XRI"
    "$PI" --sign-xml-file="$XRI" \
          --xssk-file="$KEYS_XSSK" \
          --xssk-password="$(cat "$PASS_FILE")" 2>&1 | grep -iE 'signature|error|fail' | sed 's/^/  /'
    if ! grep -q "<Signature developerId=" "$XRI"; then
        echo "  ERROR: updates.xri NOT SIGNED" >&2; exit 1
    fi
}

echo "Signing EZ Stretch BSC ($TARGET)..."
echo ""
case "$TARGET" in
    scripts) sign_scripts ;;
    xri)     sign_xri ;;
    all)     sign_scripts; echo ""; sign_xri ;;
    *) echo "Unknown target '$TARGET' (use: scripts | xri | all)" >&2; exit 1 ;;
esac

echo ""
echo "Done."
