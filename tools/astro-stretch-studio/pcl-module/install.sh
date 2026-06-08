#!/bin/bash
# ----------------------------------------------------------------------------
# Install script for AstroStretchStudio PCL Module
# ----------------------------------------------------------------------------

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=============================================="
echo "  AstroStretch Studio - Module Installer"
echo "=============================================="
echo ""

# Detect OS and set paths
OS="$(uname -s)"
case "$OS" in
    Linux*)
        MODULE_DIR="$HOME/.PixInsight/modules"
        PI_BIN="/opt/PixInsight/bin/PixInsight"
        MODULE_FILE="AstroStretchStudio-pxm.so"
        BUILD_DIR="${SCRIPT_DIR}/AstroStretchStudio/linux/g++"
        ;;
    Darwin*)
        MODULE_DIR="$HOME/Library/PixInsight/modules"
        PI_BIN="/Applications/PixInsight/PixInsight.app/Contents/MacOS/PixInsight"
        MODULE_FILE="AstroStretchStudio-pxm.dylib"
        BUILD_DIR="${SCRIPT_DIR}/AstroStretchStudio/macos/clang"
        ;;
    *)
        echo "Error: Unsupported OS: $OS"
        exit 1
        ;;
esac

# Find the built module
BUILT_MODULE=""
if [ -f "${BUILD_DIR}/x64/Release/${MODULE_FILE}" ]; then
    BUILT_MODULE="${BUILD_DIR}/x64/Release/${MODULE_FILE}"
elif [ -f "${PCLBINDIR64}/${MODULE_FILE}" ]; then
    BUILT_MODULE="${PCLBINDIR64}/${MODULE_FILE}"
fi

if [ -z "$BUILT_MODULE" ] || [ ! -f "$BUILT_MODULE" ]; then
    echo "Error: Built module not found."
    echo "Please run ./build.sh first."
    exit 1
fi

echo "Found module: $BUILT_MODULE"

# Create modules directory if needed
mkdir -p "$MODULE_DIR"

# Copy module
echo "Installing to: $MODULE_DIR"
cp "$BUILT_MODULE" "$MODULE_DIR/"

echo ""
echo "=============================================="
echo "  Installation Complete!"
echo "=============================================="
echo ""
echo "Module installed to: $MODULE_DIR/$MODULE_FILE"
echo ""
echo "To start PixInsight with unsigned modules enabled:"
echo ""
echo "  $PI_BIN --allow-unsigned-modules"
echo ""
echo "Or create an alias:"
echo "  alias pidev='$PI_BIN --allow-unsigned-modules'"
echo ""
echo "Then in PixInsight:"
echo "  Process > IntensityTransformations > AstroStretchStudio"
echo ""

# Ask if user wants to launch PI
read -p "Launch PixInsight now with unsigned modules? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Launching PixInsight..."
    "$PI_BIN" --allow-unsigned-modules &
fi
