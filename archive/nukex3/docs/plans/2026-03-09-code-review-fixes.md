# NukeX v3 Code Review Fixes

**Date:** 2026-03-09
**Scope:** All issues from 5-agent comprehensive review

## Design Decisions

1. **FrameAligner**: Keep `valid=false` when alignment fails, log warning, but still include frame with zero offset. Be EXTREMELY pessimistic about removing frames — use ALL data.

2. **outlierSigmaThreshold**: Derive `maxOutliers = max(1, nSubs/3)` from stack depth. Use sigma threshold to set ESD alpha via `alpha = 2*(1-Phi(sigma))`. Outlier detection is only for pixel selection, never frame rejection.

3. **Real-time logging**: Reduce chunk size to 10 rows. All console output via WriteLn (with newline). Add `console.Flush()` + `Module->ProcessEvents()` in progress callback. Log per-chunk throughput and error counts.

4. **SkewNormal log-PDF**: Implement analytical form: `log(2) - log(ω) - 0.5*log(2π) - 0.5*z² + log Φ(α*z)` using `log(erfc(-t/√2)/2)` for stability.

## Fix Groups

### Group A: Error Handling
- PixelSelector: specific catches + atomic error counter + re-throw bad_alloc
- SkewNormalFitter: specific catches + re-throw bad_alloc
- FrameLoader::GetKeywordValue: specific catches
- NukeXStretchInstance::ExecuteOn: add try-catch wrapper
- NaN guards in fitGaussian/fitPoisson

### Group B: Logic/Semantic
- FrameAligner: preserve valid=false, include frame anyway with zero offset
- NukeXStackInstance: derive maxOutliers from nSubs, sigma→alpha conversion
- Mutable stretch state: compute on stack in Apply(), remove mutable fields
- StretchLibrary::Create(): return nullptr instead of silent MTF fallback

### Group C: Descriptions/Comments
- NukeXModule.cpp: remove "AI segmentation" from description
- NukeXStretchProcess.cpp: rewrite to describe actual v3 features
- NukeXStackProcess.cpp: fix inaccurate feature descriptions
- Stale comments: DistributionFitter.h, NukeXStackInstance.h, StretchLibrary.h

### Group D: Real-Time Logging
- Reduce CHUNK from 100 to 10 in PixelSelector
- All console.Write → console.WriteLn
- Add console.Flush + ProcessEvents in progress callback
- Log error fallback count after each channel
- Log per-frame alignment progress

### Group E: Tests
- Unit tests for all 11 stretch Apply(double) functions
- Boundary: Apply(0), Apply(1), Apply(0.5)
- Monotonicity check
- AutoConfigure regression
