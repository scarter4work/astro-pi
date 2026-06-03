#!/bin/bash
# sign.sh - Sign EZ Stretch BSC scripts using PixInsight (headless)
#
# Usage: ./tools/sign.sh
#
# Requires password in /tmp/.pi_codesign_pass
# Does NOT require PixInsight to be running - starts its own instance

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Signing EZ Stretch BSC scripts (headless)..."

# Run PixInsight in a new slot with automation mode
PixInsight -n=9 --automation-mode --no-splash --no-startup-scripts \
    --run="$SCRIPT_DIR/CLICodeSign.js" --force-exit 2>/dev/null

# Wait a moment for file writes to complete
sleep 1

# Verify signatures were updated
echo ""
echo "Checking signatures..."
if grep -q "Signature" "$PROJECT_DIR/repository/updates.xri"; then
    ts=$(grep -oP 'timestamp="\K[^"]+' "$PROJECT_DIR/repository/updates.xri" 2>/dev/null || echo "unknown")
    echo "  updates.xri: $ts"
else
    echo "  updates.xri: NOT SIGNED"
    exit 1
fi

for script in EZStretch EZDonutRepair EZHazeKill; do
    xsgn="$PROJECT_DIR/src/scripts/EZ Stretch BSC/$script.xsgn"
    if [ -f "$xsgn" ]; then
        ts=$(grep -oP 'Timestamp>\K[^<]+' "$xsgn" 2>/dev/null || echo "unknown")
        echo "  $script.xsgn: $ts"
    else
        echo "  $script.xsgn: NOT FOUND"
    fi
done

echo ""
echo "Done."
