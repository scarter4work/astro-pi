#!/bin/bash
# ============================================================================
# sign-scripts.sh - Sign all EZ Stretch BSC scripts
# ============================================================================
#
# Two signing methods available:
#
# Method 1: PixInsight IPC (requires running PixInsight)
#   ./tools/sign-scripts.sh <password>
#
# Method 2: Standalone Python tool (requires key extraction first)
#   # One-time setup: Run DumpKeys.js in PixInsight to extract keys
#   ./tools/sign-scripts.sh --standalone
#
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

KEYS_JSON="$HOME/.pi_signing_keys.json"
PASS_FILE="/tmp/.pi_codesign_pass"

# Files to sign
FILES=(
    "$PROJECT_DIR/src/scripts/EZ Stretch BSC/EZStretch.js"
    "$PROJECT_DIR/src/scripts/EZ Stretch BSC/LuptonRGB/LuptonRGB.js"
    "$PROJECT_DIR/src/scripts/EZ Stretch BSC/RNC-ColorStretch/RNC-ColorStretch.js"
    "$PROJECT_DIR/src/scripts/EZ Stretch BSC/PhotometricStretch/PhotometricStretch.js"
    "$PROJECT_DIR/repository/updates.xri"
)

echo "============================================"
echo "EZ Stretch BSC - Code Signing"
echo "============================================"
echo ""

# Check for standalone mode
if [ "$1" = "--standalone" ] || [ "$1" = "--stand-alone" ] || [ "$1" = "-s" ]; then
    echo "Using standalone signing tool..."
    echo ""

    if [ ! -f "$KEYS_JSON" ]; then
        echo "Error: Keys file not found: $KEYS_JSON"
        echo ""
        echo "To extract keys:"
        echo "  1. Start PixInsight"
        echo "  2. Write password to $PASS_FILE"
        echo "  3. Run DumpKeys.js in PixInsight"
        echo "  4. Move output to $KEYS_JSON"
        echo "  5. chmod 600 $KEYS_JSON"
        exit 1
    fi

    # Check file permissions
    PERMS=$(stat -c %a "$KEYS_JSON" 2>/dev/null || stat -f %Lp "$KEYS_JSON")
    if [ "$PERMS" != "600" ]; then
        echo "Warning: Keys file should have mode 600 (current: $PERMS)"
        echo "Run: chmod 600 $KEYS_JSON"
    fi

    python3 "$SCRIPT_DIR/pi_codesign.py" -k "$KEYS_JSON" "${FILES[@]}"
    exit $?
fi

# Method 1: PixInsight IPC mode
if [ $# -lt 1 ]; then
    echo "Usage:"
    echo "  $0 <password>           # Use PixInsight IPC (PI must be running)"
    echo "  $0 --standalone         # Use standalone tool (requires key extraction)"
    echo ""
    echo "NOTE: For IPC mode, PixInsight must be running!"
    exit 1
fi

PASSWORD="$1"

# Check if PixInsight is running
if ! pgrep -x "PixInsight" > /dev/null 2>&1; then
    echo "Error: PixInsight is not running"
    echo "Please start PixInsight first, then run this script."
    echo ""
    echo "Or use standalone mode (if keys are extracted):"
    echo "  $0 --standalone"
    exit 1
fi

echo "PixInsight is running - sending sign command..."
echo ""

# Write password to temp file (secure permissions)
echo -n "$PASSWORD" > "$PASS_FILE"
chmod 600 "$PASS_FILE"

# Find PixInsight
for dir in "/opt/PixInsight" "/usr/local/PixInsight" "$HOME/PixInsight"; do
    if [ -d "$dir" ]; then
        PI_DIR="$dir"
        break
    fi
done

PI_EXE="$PI_DIR/bin/PixInsight"
export LD_LIBRARY_PATH="$PI_DIR/bin/lib:$PI_DIR/bin:${LD_LIBRARY_PATH:-}"

# Execute CLICodeSign.js on the running instance
"$PI_EXE" -x="$SCRIPT_DIR/CLICodeSign.js" 2>&1

# Clean up
rm -f "$PASS_FILE"

echo ""
echo "Done!"
