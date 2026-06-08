#!/bin/bash
# ----------------------------------------------------------------------------
# Build script for AstroStretchStudio PCL Module
# ----------------------------------------------------------------------------

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="${SCRIPT_DIR}/AstroStretchStudio"

echo "=============================================="
echo "  AstroStretch Studio - PCL Module Builder"
echo "=============================================="
echo ""

# Check for PCLSRCDIR
if [ -z "$PCLSRCDIR" ]; then
    echo "Warning: PCLSRCDIR not set. Trying common locations..."

    # Try common PixInsight SDK locations
    if [ -d "/opt/PixInsight/src" ]; then
        export PCLSRCDIR="/opt/PixInsight/src"
    elif [ -d "$HOME/PixInsight/src" ]; then
        export PCLSRCDIR="$HOME/PixInsight/src"
    elif [ -d "/usr/local/PixInsight/src" ]; then
        export PCLSRCDIR="/usr/local/PixInsight/src"
    else
        echo "Error: Cannot find PixInsight SDK. Please set PCLSRCDIR."
        echo "Example: export PCLSRCDIR=/path/to/PixInsight/src"
        exit 1
    fi
fi

echo "PCLSRCDIR: $PCLSRCDIR"

# Set other PCL paths
export PCLINCDIR="${PCLINCDIR:-$PCLSRCDIR/pcl}"
export PCLLIBDIR64="${PCLLIBDIR64:-$PCLSRCDIR/../lib/x64}"
export PCLBINDIR64="${PCLBINDIR64:-$PCLSRCDIR/../bin}"

echo "PCLINCDIR: $PCLINCDIR"
echo "PCLLIBDIR64: $PCLLIBDIR64"
echo "PCLBINDIR64: $PCLBINDIR64"
echo ""

# Step 1: Build WebView app if needed
if [ ! -f "${SCRIPT_DIR}/../dist/index.html" ]; then
    echo "[1/3] Building WebView React app..."
    cd "${SCRIPT_DIR}/.."
    npm install
    npm run build
else
    echo "[1/3] WebView app already built (dist/index.html exists)"
fi

# Step 2: Bundle WebView HTML
echo "[2/3] Bundling WebView HTML..."
cd "${MODULE_DIR}"
./bundle-webview.sh

# Step 3: Build PCL module
echo "[3/3] Building PCL module..."

# Detect OS
OS="$(uname -s)"
case "$OS" in
    Linux*)
        BUILD_DIR="${MODULE_DIR}/linux/g++"
        MAKEFILE="makefile-x64"
        MODULE_EXT="so"
        ;;
    Darwin*)
        BUILD_DIR="${MODULE_DIR}/macos/clang"
        MAKEFILE="makefile-x64"
        MODULE_EXT="dylib"
        ;;
    *)
        echo "Error: Unsupported OS: $OS"
        echo "Please build manually on Windows using Visual Studio."
        exit 1
        ;;
esac

# Create obj directory
OBJ_DIR="${PCLSRCDIR}/pcl/AstroStretchStudio/${OS,,}/$(basename ${BUILD_DIR})/x64/Release"
mkdir -p "$OBJ_DIR"

cd "$BUILD_DIR"
make -f "$MAKEFILE" clean 2>/dev/null || true
make -f "$MAKEFILE"

echo ""
echo "=============================================="
echo "  Build Complete!"
echo "=============================================="
echo ""
echo "Module built: AstroStretchStudio-pxm.${MODULE_EXT}"
echo ""
echo "To install, copy to your PixInsight modules directory:"
echo "  Linux:  ~/.PixInsight/modules/"
echo "  macOS:  ~/Library/PixInsight/modules/"
echo ""
echo "To run PixInsight with unsigned modules enabled:"
echo "  Linux:  /opt/PixInsight/bin/PixInsight --allow-unsigned-modules"
echo "  macOS:  /Applications/PixInsight/PixInsight.app/Contents/MacOS/PixInsight --allow-unsigned-modules"
echo ""
