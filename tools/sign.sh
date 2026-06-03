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
# Two signers, because PixInsight only exposes a CLI signer for XML, not scripts:
#   - updates.xri  -> native `PixInsight.sh --sign-xml-file` (NukeX pattern).
#                     A short-lived core process; signature guaranteed PI-valid.
#   - *.js scripts -> tools/pi_codesign.py standalone Ed25519 signer, using keys
#                     extracted to ~/.pi_signing_keys.json (DumpKeys.js, one-time).
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
PI="${PIXINSIGHT_DIR:-/opt/PixInsight}/bin/PixInsight.sh"
KEYS_XSSK="/home/scarter4work/projects/keys/scarter4work_keys.xssk"
KEYS_JSON="$HOME/.pi_signing_keys.json"
PASS_FILE="/tmp/.pi_codesign_pass"
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
    echo "Scripts (pi_codesign.py):"
    if [ ! -f "$KEYS_JSON" ]; then
        echo "  ERROR: $KEYS_JSON missing. Run DumpKeys.js in PixInsight once to extract keys." >&2
        exit 1
    fi
    local js_files=()
    local s
    for s in "${SCRIPTS[@]}"; do
        [ -f "$SCRIPTS_DIR/$s.js" ] && js_files+=( "$SCRIPTS_DIR/$s.js" )
    done
    python3 "$SCRIPT_DIR/pi_codesign.py" -k "$KEYS_JSON" "${js_files[@]}" || exit 1
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
