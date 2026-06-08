# AstroStretchStudio PCL Module

PixInsight Class Library module wrapper for AstroStretch Studio.

## Overview

This PCL module provides a native PixInsight interface for the AstroStretch Studio algorithms:

- **Optimal Transport Stretch (OTS)**: Histogram mapping using optimal transport theory
- **Starlet Arctan Stretch (SAS)**: Multiscale wavelet stretching with arctan compression

The module embeds the React WebView application and communicates with it for real-time preview, while the actual image processing is done natively in C++ for maximum performance.

## Building

### Prerequisites

1. PixInsight development environment installed
2. PCL (PixInsight Class Library) SDK
3. Node.js and npm (for building the WebView app)

### Build Steps

1. **Build the WebView application:**

   ```bash
   cd ..
   npm install
   npm run build
   ```

2. **Bundle the WebView HTML:**

   ```bash
   cd pcl-module/AstroStretchStudio
   ./bundle-webview.sh
   ```

   This generates `WebViewContent.h` with the embedded HTML/CSS/JS.

3. **Build the PCL module:**

   **Linux:**
   ```bash
   cd linux/g++
   make -f makefile-x64
   ```

   **macOS:**
   ```bash
   cd macos/clang
   make -f makefile-x64
   ```

   **Windows:**
   Open the Visual Studio project in `windows/vc17/` and build.

4. **Install:**

   Copy the built module (`.so`, `.dylib`, or `.dll`) to your PixInsight modules directory:
   - Linux: `~/.PixInsight/modules/`
   - macOS: `~/Library/PixInsight/modules/`
   - Windows: `C:\Users\<user>\AppData\Local\PixInsight\modules\`

## File Structure

```
AstroStretchStudio/
├── AstroStretchStudioModule.cpp      # Module registration
├── AstroStretchStudioModule.h
├── AstroStretchStudioProcess.cpp     # Process definition
├── AstroStretchStudioProcess.h
├── AstroStretchStudioInstance.cpp    # Instance with algorithm implementations
├── AstroStretchStudioInstance.h
├── AstroStretchStudioInterface.cpp   # WebView-based UI
├── AstroStretchStudioInterface.h
├── AstroStretchStudioParameters.cpp  # Parameter definitions
├── AstroStretchStudioParameters.h
├── WebViewContent.h                  # Generated: embedded HTML
├── bundle-webview.sh                 # Script to generate WebViewContent.h
├── linux/g++/makefile-x64            # Linux build
├── macos/clang/makefile-x64          # macOS build
└── windows/vc17/                     # Windows project files
```

## Architecture

```
┌────────────────────────────────────────────────────────────┐
│                    PixInsight                               │
├────────────────────────────────────────────────────────────┤
│  AstroStretchStudio Module                                 │
│  ┌──────────────────┐  ┌─────────────────────────────────┐ │
│  │  Native C++      │  │  WebView (React App)            │ │
│  │  Processing      │  │  - UI Controls                  │ │
│  │                  │◄─┤  - Real-time Preview            │ │
│  │  - OTS Engine    │  │  - Histogram Visualization      │ │
│  │  - SAS Engine    │  │                                 │ │
│  │  - Starlet       │  │  Communicates via JSON          │ │
│  │    Transform     │─►│  postMessage API                │ │
│  └──────────────────┘  └─────────────────────────────────┘ │
└────────────────────────────────────────────────────────────┘
```

## Communication Protocol

The PCL module and WebView communicate via JSON messages:

### PCL → WebView

**setImage**: Send image data for preview
```json
{
  "type": "setImage",
  "width": 1920,
  "height": 1080,
  "data": "<base64-encoded RGBA>"
}
```

**setParameters**: Sync current parameters
```json
{
  "type": "setParameters",
  "algorithm": "ots",
  "ots": { ... },
  "sas": { ... }
}
```

### WebView → PCL

**parametersChanged**: User changed parameters
```json
{
  "type": "parametersChanged",
  "algorithm": "ots",
  "ots": {
    "objectType": "nebula",
    "backgroundTarget": 0.15,
    ...
  }
}
```

**apply**: User clicked Apply
```json
{ "type": "apply" }
```

## License

MIT License - See LICENSE file for details.
