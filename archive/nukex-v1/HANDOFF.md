# NukeX Handoff Document

## Current State (January 18, 2026)

This document is for the next Claude instance picking up this project.

---

## The Problem

**NukeX module is not loading in PixInsight.** When PI starts, it shows "Installing 66 modules" but NukeX is not in the list - it's being silently skipped.

The module file exists at `/opt/PixInsight/bin/NukeX-pxm.so` and is properly signed.

---

## Root Cause Identified

Comparing NukeX to a working module (BayesianAstro at `/home/scarter4work/projects/EZ-suite-bsc/EZ-Stretch-BSC/BayesianAstro/cpp/src/BayesianAstroModule.cpp`), we found:

**NukeX was missing the static initializer pattern.**

PixInsight calls `IdentifyPixInsightModule()` BEFORE calling `InstallPixInsightModule()`. The original NukeX code only created the module singleton in `InstallPixInsightModule()`, so when PI tried to identify the module, `TheNukeXModule` was still `nullptr`.

### The Fix (already applied to src/NukeXModule.cpp)

1. Added `TheNukeXModule = this;` in the constructor
2. Added static initializer pattern:

```cpp
// Module singleton accessor - creates module on first call
NukeXModule* GetNukeXModuleInstance()
{
   static NukeXModule* s_instance = new NukeXModule;
   return s_instance;
}

// Static initializer to ensure Module global is set before IdentifyPixInsightModule
namespace {
   struct ModuleInitializer {
      ModuleInitializer() {
         GetNukeXModuleInstance();
      }
   };
   static ModuleInitializer s_moduleInit;
}
```

3. Removed the `new pcl::NukeXModule;` line from `InstallPixInsightModule()`

**This fix has been applied but the module still doesn't load.** There may be another issue, or the fix needs verification.

---

## What Needs To Be Done

1. **Get the module loading in PixInsight** - The static initializer fix should work but hasn't been verified
2. **Push code to GitHub** - There are many uncommitted changes (NukeXStack, new engine components)
3. **Set up PixInsight update repository** - User wants to install via PI's updater system

---

## Project Overview

### NukeX (single image stretch)
- AI-driven region-aware stretch for PixInsight
- Uses ONNX segmentation model (21 classes: star core, nebula, background, etc.)
- Applies different stretch algorithms per region
- Blends results smoothly

### NukeXStack (intelligent stacking) - NEW
- Intelligent pixel selection for subframe integration
- Per-pixel distribution fitting across stack
- ML-guided selection strategies based on pixel type
- Components:
  - `PixelStackAnalyzer` - fits distributions across stack at each (x,y)
  - `PixelSelector` - chooses best pixel value based on class + stats
  - `TransitionChecker` - smooths hard transitions in final image
  - `DistributionFitter` - statistical distribution fitting
  - `LayerStack` - manages the stack of subframes

---

## File Structure

```
src/
├── NukeXModule.cpp          # Module definition (FIX APPLIED HERE)
├── NukeXProcess/Instance/Interface/Parameters  # Original stretch process
├── NukeXStackProcess/Instance/Interface/Parameters  # NEW stacking process
└── engine/
    ├── Segmentation.cpp     # ONNX segmentation
    ├── StretchLibrary.cpp   # 11 stretch algorithms
    ├── SelectionRules.cpp   # Per-region algorithm selection
    ├── Compositor.cpp       # Blends regional stretches
    ├── PixelStackAnalyzer.cpp  # NEW - per-pixel distribution fitting
    ├── PixelSelector.cpp       # NEW - intelligent pixel selection
    ├── TransitionChecker.cpp   # NEW - smoothing
    ├── DistributionFitter.cpp  # NEW - distribution fitting
    └── LayerStack.cpp          # NEW - stack management
```

---

## Build & Install

```bash
cd /home/scarter4work/projects/NukeX
make clean && make -j8
# Sign the module
/opt/PixInsight/bin/PixInsight.sh --sign-module-file=NukeX-pxm.so \
  --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
  --xssk-password="***REDACTED***"
# Install
sudo cp NukeX-pxm.so NukeX-pxm.xsgn /opt/PixInsight/bin/
```

---

## Key References

- **Working module example**: `/home/scarter4work/projects/EZ-suite-bsc/EZ-Stretch-BSC/BayesianAstro/cpp/src/BayesianAstroModule.cpp`
- **PI launch script**: `/home/scarter4work/bin/pixinsight-hidpi.sh`
- **CLAUDE.md**: Contains the intelligent pixel selection refactor plan
- **IMPLEMENTATION-PLAN.md**: Original 8-session implementation plan (all complete)

---

## Git Status

There are many uncommitted changes. The git index was corrupted (20MB file). Fixed with:
```bash
rm -f .git/index && git reset
```

Then stage and commit all changes, push to origin.

---

## GitHub

- Repo: https://github.com/scarter4work/NukeX
- User wants to set up PixInsight update repository here for distribution via PI updater

---

## Next Steps

1. Verify the module initialization fix is correct
2. If still not loading, add debug output to `IdentifyPixInsightModule` or check for crashes during static init
3. Commit and push all changes to GitHub
4. Create PixInsight update repository structure (updates.xri manifest, etc.)
