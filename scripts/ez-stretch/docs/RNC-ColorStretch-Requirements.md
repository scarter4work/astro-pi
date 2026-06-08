# RNC-Color-Stretch for PixInsight

## Requirements Specification

**Version:** 1.0  
**Date:** December 27, 2025  
**Target Platform:** PixInsight JavaScript Runtime (PJSR)  
**Algorithm Source:** Roger N. Clark, Clarkvision.com (GPL Licensed)

---

## 1. Executive Summary

RNC-Color-Stretch is an advanced image stretching algorithm designed specifically for astrophotography. Unlike traditional stretching methods that cause color loss (especially in faint nebulae and star fields), RNC-Color-Stretch actively **recovers** color that would otherwise be lost during the stretch process.

### Key Differentiators from Existing PixInsight Tools

| Feature | GHS/Veralux | MaskedStretch | RNC-Color-Stretch |
|---------|-------------|---------------|-------------------|
| Stretch Type | Global | Global w/mask | Global + Color Recovery |
| Color Preservation | Passive | Passive | **Active Recovery** |
| Sky Subtraction | Manual | Manual | **Automatic** |
| Black Point | Manual | Manual | **Auto-analyzed** |
| Multi-pass Stretch | No | No | **Yes (iterative)** |

---

## 2. Algorithm Specification

### 2.1 Core Algorithm Steps

The algorithm performs three main operations:

```
1. BLACK POINT ANALYSIS
   - Compute histogram for each RGB channel
   - Find histogram peak for each channel
   - Calculate sky level at skylevelfactor × peak
   - Align RGB sky levels to neutral reference

2. POWER STRETCH (iterative)
   For each iteration:
     - Apply power function: output = input^(1/rootpower)
     - Re-analyze histogram
     - Re-align sky levels
     - Maintain black point throughout

3. COLOR RECOVERY
   - Compute pre-stretch color ratios (6 ratios: gr, br, rg, bg, gb, rb)
   - Compute post-stretch color ratios
   - Calculate intensity-dependent correction factors
   - Apply correction to restore original color relationships
```

### 2.2 Mathematical Details

#### Power Stretch Function
```
F(x) = x^(1/p)

Where:
  x = normalized input intensity [0,1]
  p = rootpower parameter (1.0 - 599.0)
  
For multiple iterations:
  Pass 1: x' = x^(1/p1)
  Pass 2: x'' = x'^(1/p2)
```

#### Sky Level Alignment
```
For each channel c ∈ {R, G, B}:
  1. Compute histogram H_c
  2. Find peak: peak_c = argmax(H_c)
  3. Find sky level: sky_c = H_c at (skylevelfactor × peak_c)
  4. Compute offset: offset_c = sky_c - reference
  5. Subtract: channel_c = channel_c - offset_c
```

#### Color Recovery
```
Pre-stretch ratios (on linear data):
  gr_orig = G / R (clamped to [limit_low, limit_high])
  br_orig = B / R
  rg_orig = R / G
  bg_orig = B / G
  gb_orig = G / B
  rb_orig = R / B

Post-stretch ratios (same calculation on stretched data)

Correction factors:
  For each pixel:
    cf_gr = gr_orig / gr_stretched
    cf_br = br_orig / br_stretched
    ... (for all 6 ratios)

Intensity-dependent blending:
  blend = f(intensity)  // Higher intensity = more correction
  
Final correction:
  R_out = R_stretched × blend_r
  G_out = G_stretched × blend_g  
  B_out = B_stretched × blend_b
```

#### S-Curve Functions

**S-Curve 1** (lower midtone contrast):
```
// Approximate form - adds contrast to shadows/lower midtones
y = x + a × sin(π × x) × (1 - x)
```

**S-Curve 2** (overall brightening):
```
// Approximate form - brightens with emphasis on highlights
y = x^0.85
```

### 2.3 Parameter Reference

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| rootpower | 1.0 - 599.0 | 6.0 | Primary power stretch factor. Higher = stronger lift of faint areas |
| rootpower2 | 1.0 - 599.0 | 0 (disabled) | Secondary power for iteration 2 |
| rootiter | 1 - 3 | 1 | Number of stretch iterations |
| skylevelfactor | 0.000001 - 0.5 | 0.06 | Sky level as fraction of histogram peak (6% default) |
| rgbskyzero | 0 - 65535 each | 4096,4096,4096 | Target sky zero point per channel |
| setmin | 0 - 65535 each | 5140,5200,5650 | Minimum RGB levels (noise floor) |
| enhance | 0.1 - 3.0 | 1.0 | Post-stretch saturation multiplier |
| scurve | 0-4 | 0 | S-curve selection (0=none, 1-4=variants) |
| percent_clip | 0 - 1.0 | 0.00005 | Max fraction of pixels clipped to zero |
| colorRecovery | bool | true | Enable/disable color recovery |
| colorLimit | 0.1 - 1.0 | 0.2 | Lower limit for color ratio clamping |

---

## 3. UI Specification

### 3.1 Dialog Layout

```
+==============================================================================+
|  RNC-Color-Stretch                                                    [?][X] |
+==============================================================================+
|                                                                              |
|  +-- Input Image -------------------------------------------------------+   |
|  |  [✓] Use active RGB image                                            |   |
|  |  Target: [NGC7380_RGB_stretched ▼]                                   |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  +-- Stretch Parameters ------------------------------------------------+   |
|  |                                                                      |   |
|  |  Root Power     [====●============] 6.0        (1.0 - 599.0)        |   |
|  |                                                                      |   |
|  |  [✓] Two-Pass Stretch                                                |   |
|  |  Root Power 2   [===●=============] 3.0        (1.0 - 599.0)        |   |
|  |                                                                      |   |
|  |  S-Curve:  ( ) None  (●) SCurve1  ( ) SCurve2  ( ) SCurve3          |   |
|  |                                                                      |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  +-- Sky Calibration ---------------------------------------------------+   |
|  |                                                                      |   |
|  |  Sky Level Factor  [=====●========] 0.06       (0.0001 - 0.5)       |   |
|  |                                                                      |   |
|  |  [✓] Link RGB Sky Zero                                               |   |
|  |  Sky Zero (16-bit)  [=====●=======] 4096       (0 - 65535)          |   |
|  |                                                                      |   |
|  |  [ ] Custom per-channel:  R [4096]  G [4096]  B [4096]              |   |
|  |                                                                      |   |
|  |  [Auto Detect Sky] [Sample from Preview]                            |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  +-- Color Recovery ----------------------------------------------------+   |
|  |                                                                      |   |
|  |  [✓] Enable Color Recovery                                           |   |
|  |                                                                      |   |
|  |  Color Ratio Limit [===●==========] 0.20       (0.1 - 1.0)          |   |
|  |                                                                      |   |
|  |  Enhancement       [========●=====] 1.00       (0.1 - 3.0)          |   |
|  |                                                                      |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  +-- Advanced ----------------------------------------------------------+   |
|  |                                                                      |   |
|  |  Set Minimum (noise floor):  R [5140]  G [5200]  B [5650]           |   |
|  |                                                                      |   |
|  |  Clip Percent [0.005] %                                              |   |
|  |                                                                      |   |
|  |  [ ] Apply tone curve (for linear input)                             |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  +-- Preview -----------------------------------------------------------+   |
|  |  [✓] Real-Time Preview    [Before | Split | After]    [Fit] [1:1]   |   |
|  |  +----------------------------------------------------------------+  |   |
|  |  |                                                                |  |   |
|  |  |     ┌─────────────────┬─────────────────┐                     |  |   |
|  |  |     │                 │                 │                     |  |   |
|  |  |     │    BEFORE       │     AFTER       │                     |  |   |
|  |  |     │  (dim, gray)    │  (colorful!)    │                     |  |   |
|  |  |     │                 │                 │                     |  |   |
|  |  |     └─────────────────┴─────────────────┘                     |  |   |
|  |  |                                                                |  |   |
|  |  +----------------------------------------------------------------+  |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  +-- Histogram ---------------------------------------------------------+   |
|  |  [Input] [Output]                                                    |   |
|  |  ╭──────────────────────────────────────╮                           |   |
|  |  │  ▄▄▄                                 │  R ──                     |   |
|  |  │ ▄████▄                               │  G ──                     |   |
|  |  │▄██████▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄     │  B ──                     |   |
|  |  ╰──────────────────────────────────────╯                           |   |
|  +----------------------------------------------------------------------+   |
|                                                                              |
|  [New Instance] [▼] [Reset]              [Apply] [Cancel] [Execute (●)]     |
+==============================================================================+
```

### 3.2 Control Specifications

#### Stretch Parameters Group
- **Root Power**: NumericControl, range 1.0-599.0, precision 1, slider logarithmic scale
- **Two-Pass Checkbox**: Enables rootpower2 control
- **Root Power 2**: NumericControl, same as rootpower, grayed unless two-pass enabled
- **S-Curve Radio Buttons**: None, SCurve1, SCurve2, SCurve3, SCurve4

#### Sky Calibration Group
- **Sky Level Factor**: NumericControl, range 0.0001-0.5, precision 4, logarithmic
- **Link RGB Checkbox**: When checked, single slider; when unchecked, show RGB triplet
- **Sky Zero**: NumericControl or triplet, 0-65535, integer
- **Auto Detect Button**: Analyzes histogram to find optimal sky level
- **Sample Button**: Click-to-sample from preview

#### Color Recovery Group  
- **Enable Checkbox**: Master on/off for color recovery
- **Color Ratio Limit**: NumericControl, 0.1-1.0, precision 2
- **Enhancement**: NumericControl, 0.1-3.0, precision 2

#### Advanced Group (Collapsible)
- **Set Minimum**: RGB triplet inputs, 0-65535
- **Clip Percent**: NumericControl, 0-1.0, precision 4
- **Tone Curve Checkbox**: For linear input images

#### Preview Panel
- **Real-Time Toggle**: Checkbox
- **View Mode**: Three-button toggle (Before/Split/After)
- **Zoom Controls**: Fit, 1:1, +/-
- **Canvas**: Standard PixInsight preview with split-view support

#### Histogram Panel
- **Tab Toggle**: Input/Output
- **RGB Histogram Display**: Standard 3-channel overlay

---

## 4. PixInsight Implementation Notes

### 4.1 Required PJSR Includes

```javascript
#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/SectionBar.jsh>
#include <pjsr/Color.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/UndoFlag.jsh>
```

### 4.2 Core Processing Pattern

```javascript
function RNCColorStretchEngine() {
   this.rootpower = 6.0;
   this.rootpower2 = 0;
   this.twoPass = false;
   this.skylevelfactor = 0.06;
   this.rgbskyzero = [4096, 4096, 4096];
   this.setmin = [5140, 5200, 5650];
   this.enhance = 1.0;
   this.scurve = 0;
   this.colorRecovery = true;
   this.colorLimit = 0.2;
   this.clipPercent = 0.00005;
}

RNCColorStretchEngine.prototype.apply = function(image) {
   // 1. Store original color ratios (before any stretch)
   var originalRatios = this.computeColorRatios(image);
   
   // 2. Analyze histogram and determine sky levels
   var skyLevels = this.analyzeSkyLevels(image);
   
   // 3. Subtract sky offset per channel
   this.subtractSky(image, skyLevels);
   
   // 4. Apply power stretch (pass 1)
   this.applyPowerStretch(image, this.rootpower);
   
   // 5. Re-analyze and re-align sky
   skyLevels = this.analyzeSkyLevels(image);
   this.subtractSky(image, skyLevels);
   
   // 6. Apply power stretch (pass 2, if enabled)
   if (this.twoPass && this.rootpower2 > 1) {
      this.applyPowerStretch(image, this.rootpower2);
      skyLevels = this.analyzeSkyLevels(image);
      this.subtractSky(image, skyLevels);
   }
   
   // 7. Apply S-curve if selected
   if (this.scurve > 0) {
      this.applySCurve(image, this.scurve);
   }
   
   // 8. Color recovery
   if (this.colorRecovery) {
      var stretchedRatios = this.computeColorRatios(image);
      this.recoverColor(image, originalRatios, stretchedRatios);
   }
   
   // 9. Apply enhancement (saturation)
   if (this.enhance != 1.0) {
      this.applySaturation(image, this.enhance);
   }
   
   // 10. Apply minimum levels
   this.applyMinimumLevels(image);
};
```

### 4.3 Histogram Analysis

```javascript
RNCColorStretchEngine.prototype.analyzeSkyLevels = function(image) {
   var histograms = [
      image.computeHistogram(0, 0, 65536),  // R
      image.computeHistogram(0, 1, 65536),  // G
      image.computeHistogram(0, 2, 65536)   // B
   ];
   
   var skyLevels = [];
   
   for (var c = 0; c < 3; c++) {
      // Find histogram peak
      var peakBin = 0;
      var peakCount = 0;
      for (var i = 0; i < histograms[c].length; i++) {
         if (histograms[c][i] > peakCount) {
            peakCount = histograms[c][i];
            peakBin = i;
         }
      }
      
      // Find sky level at skylevelfactor of peak
      var targetCount = peakCount * this.skylevelfactor;
      var skyBin = peakBin;
      while (skyBin > 0 && histograms[c][skyBin] > targetCount) {
         skyBin--;
      }
      
      skyLevels.push(skyBin / 65535.0);
   }
   
   return skyLevels;
};
```

### 4.4 Color Ratio Computation

```javascript
RNCColorStretchEngine.prototype.computeColorRatios = function(image) {
   // Compute 6 color ratios across the image
   // Returns object with ratio statistics (mean, for intensity-dependent recovery)
   
   var R = image.getSamples(new Rect(0, 0, image.width, image.height), 0);
   var G = image.getSamples(new Rect(0, 0, image.width, image.height), 1);
   var B = image.getSamples(new Rect(0, 0, image.width, image.height), 2);
   
   var ratios = {
      gr: [], br: [], rg: [], bg: [], gb: [], rb: []
   };
   
   for (var i = 0; i < R.length; i++) {
      if (R[i] > 0.001) {
         ratios.gr.push(Math.min(Math.max(G[i]/R[i], this.colorLimit), 1.0));
         ratios.br.push(Math.min(Math.max(B[i]/R[i], this.colorLimit), 1.0));
      }
      if (G[i] > 0.001) {
         ratios.rg.push(Math.min(Math.max(R[i]/G[i], this.colorLimit), 1.0));
         ratios.bg.push(Math.min(Math.max(B[i]/G[i], this.colorLimit), 1.0));
      }
      if (B[i] > 0.001) {
         ratios.gb.push(Math.min(Math.max(G[i]/B[i], this.colorLimit), 1.0));
         ratios.rb.push(Math.min(Math.max(R[i]/B[i], this.colorLimit), 1.0));
      }
   }
   
   return ratios;
};
```

### 4.5 Preview Implementation

```javascript
// Use scaled-down preview for performance
function generatePreview(image, engine, previewSize) {
   // Create scaled copy
   var preview = new Image(previewSize, previewSize * image.height / image.width, 
                           image.numberOfChannels, image.colorSpace, 32, SampleType_Real);
   preview.resample(image);
   
   // Apply algorithm to preview
   engine.apply(preview);
   
   return preview;
}
```

---

## 5. Testing Checklist

### Functional Tests
- [ ] Single-pass stretch with various rootpower values (5, 20, 50, 200)
- [ ] Two-pass stretch with different power combinations
- [ ] Sky level auto-detection accuracy
- [ ] Per-channel sky zero alignment
- [ ] Color recovery on/off comparison
- [ ] S-curve variants (1-4)
- [ ] Enhancement/saturation control
- [ ] Set minimum levels functionality
- [ ] Clip percent limiting

### Edge Cases
- [ ] Very dark images (low S/N)
- [ ] High light pollution gradients
- [ ] Saturated stars
- [ ] Narrowband data (single-channel)
- [ ] Already-stretched (nonlinear) input
- [ ] Linear input with tone curve option

### UI Tests
- [ ] All sliders respond correctly
- [ ] Preview updates in real-time
- [ ] Split view works correctly
- [ ] Histogram displays update
- [ ] Instance save/load functions
- [ ] Reset restores defaults
- [ ] Cancel aborts without changes

### Performance
- [ ] Preview updates < 500ms on 4K image
- [ ] Full execution < 30s on 8K×8K image
- [ ] Memory usage reasonable (< 2× image size)

---

## 6. File Structure

```
RNC-ColorStretch/
├── RNC-ColorStretch.js       # Main script entry point
├── RNC-ColorStretch-Engine.js # Core algorithm implementation
├── RNC-ColorStretch-GUI.js    # Dialog and UI components
├── RNC-ColorStretch-Parameters.js # Parameter management
└── README.md                  # User documentation
```

---

## 7. Acceptance Criteria

1. **Algorithm Accuracy**: Color recovery should produce visibly more saturated faint nebulosity compared to same stretch without recovery
2. **Comparison to Original**: Results should be comparable to Roger Clark's reference implementation when given same parameters
3. **UI Responsiveness**: All controls update preview within 500ms
4. **Stability**: No crashes on valid RGB images of any size
5. **Integration**: Works with standard PixInsight workflow (linear and nonlinear input)

---

## 8. References

1. Clark, R.N. "Advanced Image Stretching with the rnc-color-stretch Algorithm"  
   https://clarkvision.com/articles/astrophotography-rnc-color-stretch/

2. Clark, R.N. "rnc-color-stretch Software Download"  
   https://clarkvision.com/articles/astrophotography.software/rnc-color-stretch/

3. PixInsight JavaScript Runtime Reference  
   https://pixinsight.com/doc/pjsr/

---

## Appendix A: Recommended Starting Parameters

### For Typical Deep-Sky (Good S/N)
```
rootpower: 50
twoPass: false
skylevelfactor: 0.06
scurve: 1 (SCurve1)
colorRecovery: true
enhance: 1.0
```

### For Faint Nebulae (Low S/N)
```
rootpower: 5
twoPass: true
rootpower2: 3
skylevelfactor: 0.06
scurve: 1
colorRecovery: true
enhance: 1.2
```

### For High Light Pollution
```
rootpower: 30
twoPass: true  
rootpower2: 2
skylevelfactor: 0.01  (lower = more conservative)
scurve: 1
colorRecovery: true
enhance: 1.0
```

### Aggressive Stretch (Very Good Data)
```
rootpower: 200
twoPass: true
rootpower2: 3
skylevelfactor: 0.06
scurve: 2 (SCurve2)
colorRecovery: true
enhance: 1.3
```
