#!/bin/bash
# install-local.sh - Install EZ Stretch BSC scripts to user's PixInsight directory
#
# Usage: ./tools/install-local.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PI_USER_DIR="$HOME/.PixInsight/src/scripts"

echo "Installing EZ Stretch BSC to $PI_USER_DIR..."

mkdir -p "$PI_USER_DIR"
cp -r "$PROJECT_DIR/src/scripts/EZ Stretch BSC" "$PI_USER_DIR/"

echo "Installed:"
ls -la "$PI_USER_DIR/EZ Stretch BSC/"

echo ""
echo "Done. Restart PixInsight and run Script > Feature Scripts > Scan for New Scripts"
