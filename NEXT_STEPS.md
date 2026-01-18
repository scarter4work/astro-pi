# NukeX2 - Implementation Plan

## Current State Summary

The NukeXStack stacking implementation is **substantially complete** (~20K lines of C++ code):

- PixelStackAnalyzer - Per-pixel distribution fitting across frame stacks
- PixelSelector - ML-guided pixel selection with 21-class awareness
- TransitionChecker - Tile-based transition smoothing
- Full NukeXStackInstance with file I/O, FITS header parsing, and integration pipeline
- Complete parameter definitions and interface

**What's Missing:**
1. Build system (no Makefile)
2. Verification that module loads in PixInsight
3. PixInsight update repository structure

---

## Phase 1: Build System

**Goal**: Get the module to compile

### Tasks

1. **Create Makefile**
   - Reference PCL paths: `/opt/PixInsight/include`
   - Link PCL libraries
   - Optional ONNX Runtime linking
   - Output: `NukeX-pxm.so`

2. **Compile and Fix Errors**
   - Likely will have some missing includes or PCL API changes
   - Fix any compilation issues

3. **Initial Test**
   - Sign module
   - Copy to `/opt/PixInsight/bin/`
   - Launch PixInsight
   - Check if NukeX and NukeXStack appear in Process menu

---

## Phase 2: Module Loading Fix

**Goal**: Ensure module loads in PixInsight

### Current Status
The static initializer pattern was added in `NukeXModule.cpp`:
```cpp
NukeXModule* GetNukeXModuleInstance()
{
   static NukeXModule* s_instance = new NukeXModule;
   return s_instance;
}

namespace {
   struct ModuleInitializer {
      ModuleInitializer() {
         GetNukeXModuleInstance();
      }
   };
   static ModuleInitializer s_moduleInit;
}
```

### If Module Still Doesn't Load

1. Check PixInsight console for errors on startup
2. Run `ldd NukeX-pxm.so` to check for missing shared libraries
3. Add debug output to `IdentifyPixInsightModule`
4. Compare module structure to working reference: `/home/scarter4work/projects/EZ-suite-bsc/EZ-Stretch-BSC/BayesianAstro/cpp/src/BayesianAstroModule.cpp`

---

## Phase 3: Functional Testing

**Goal**: Verify stacking works correctly

### Test Cases

1. **Basic Stack** (3-5 frames)
   - Load prestretched subframes
   - Run NukeXStack without ML segmentation
   - Verify output image is created
   - Check pixel values are reasonable

2. **Outlier Rejection**
   - Include a frame with artificial defect (simulated cloud)
   - Verify defective pixels are rejected

3. **ML Segmentation** (if ONNX available)
   - Run with segmentation enabled
   - Verify different regions get different treatment

4. **Transition Smoothing**
   - Check for hard edges in output
   - Verify smoothing corrects artifacts

---

## Phase 4: PixInsight Update Repository

**Goal**: Enable installation via PI's updater

### Repository Structure
```
NukeX2/
├── repository/
│   ├── updates.xri           # Update manifest (signed)
│   └── updates.xri.xsgn      # Signature
├── packages/
│   ├── NukeX-1.0.0-linux-x64.zip
│   ├── NukeX-1.0.0-linux-x64.zip.sha1
│   └── ... (other platforms)
└── ...
```

### updates.xri Format
```xml
<?xml version="1.0" encoding="UTF-8"?>
<xri version="1.0">
   <package id="NukeX" version="1.0.0" releaseDate="2026-01-18">
      <title>NukeX - Intelligent Stacking</title>
      <description>AI-driven pixel selection for subframe integration</description>
      <platform os="linux" arch="x64">
         <file name="NukeX-pxm.so" sha1="..." />
         <file name="NukeX-pxm.xsgn" sha1="..." />
      </platform>
   </package>
</xri>
```

### Signing
```bash
/opt/PixInsight/bin/PixInsight.sh --sign-update-repository \
  --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
  --xssk-password="***REDACTED***"
```

---

## Immediate Next Steps

1. **Create Makefile** - Get it compiling
2. **Build and sign module** - `make && sign`
3. **Test module loading** - Launch PI, check for NukeXStack
4. **Test basic stacking** - Run on real subframes
5. **Set up repository structure** - For distribution

---

## Files to Potentially Remove

These are outdated and replaced by PROJECT.md:
- `HANDOFF.md` - Merged into PROJECT.md
- `IMPLEMENTATION-PLAN.md` - Old 8-session plan for stretch module
- `NukeX-Specification.md` - Detailed spec for stretch, not stacking
- `LAYER_ARCHITECTURE.md` - Future feature, not priority

Keep for reference or delete?

---

## Reference: Working Module Example

**BayesianAstro module**: `/home/scarter4work/projects/EZ-suite-bsc/EZ-Stretch-BSC/BayesianAstro/cpp/src/`

This module loads correctly in PixInsight and can be used as reference for:
- Module structure
- Makefile format
- Static initialization pattern
