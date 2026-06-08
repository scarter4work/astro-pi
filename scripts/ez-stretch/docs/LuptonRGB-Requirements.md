# Lupton RGB Stretch - PixInsight Script Requirements

## Overview

Implement the Lupton et al. (2004) RGB stretch algorithm as a PixInsight script with a full GUI dialog including real-time preview. This algorithm is used by professional astronomical surveys (SDSS, HST) for creating color-preserving stretched images from high dynamic range data.

**Reference Paper:** Lupton, R. et al. (2004) "Preparing Red-Green-Blue Images from CCD Data" PASP 116:133-137
https://ui.adsabs.harvard.edu/abs/2004PASP..116..133L

---

## Algorithm Specification

### Core Algorithm

The Lupton RGB stretch works by computing a combined intensity, applying an arcsinh stretch to that intensity, then scaling each RGB channel proportionally to preserve color ratios.

```javascript
// Input: R, G, B channels (normalized 0-1 float values)
// Parameters: alpha (stretch), Q (softening), minimum (black point per channel)

// Step 1: Compute intensity
I = (R + G + B) / 3

// Step 2: Apply arcsinh stretch to intensity
// The stretch function:
function F(x, alpha, Q, minimum) {
    return Math.asinh(alpha * Q * (x - minimum)) / Q
}

// Step 3: Compute scale factor
// Handle division by zero when I = 0
if (I > 0) {
    scale = F(I, alpha, Q, minimum) / I
} else {
    scale = 0
}

// Step 4: Apply scale to each channel
R_out = R * scale
G_out = G * scale
B_out = B * scale

// Step 5: Color-preserving clipping
// If any channel exceeds 1.0, scale ALL channels down proportionally
maxVal = Math.max(R_out, G_out, B_out)
if (maxVal > 1.0) {
    R_out = R_out / maxVal
    G_out = G_out / maxVal
    B_out = B_out / maxVal
}

// Output: R_out, G_out, B_out (0-1 range, color preserved)
```

### Parameter Details

| Parameter | Symbol | Range | Default | Description |
|-----------|--------|-------|---------|-------------|
| Stretch | α (alpha) | 0.1 - 50.0 | 5.0 | Linear amplification factor. Higher = brighter image |
| Softening | Q | 0.1 - 30.0 | 8.0 | Controls linear→logarithmic transition. Lower Q = earlier transition to log behavior |
| Black Point | minimum | -0.1 - 0.5 | 0.0 | Value subtracted before stretch. Can be per-channel or linked |
| Saturation | - | 0.5 - 2.0 | 1.0 | Post-stretch saturation boost (optional enhancement) |

### Clipping Modes

1. **Preserve Color (Lupton)** - Default. Scale all channels proportionally when any clips. Preserves hue/saturation.
2. **Hard Clip** - Independently clip each channel at 1.0. May cause color shifts.
3. **Rescale to Max** - Normalize entire image so max value = 1.0.

### Mathematical Notes

The arcsinh function provides a smooth transition:
- For small x: `asinh(x) ≈ x` (linear behavior)
- For large x: `asinh(x) ≈ ln(2x)` (logarithmic behavior)

The Q parameter controls where this transition occurs:
- High Q (e.g., 20): Stays linear longer, more aggressive stretch
- Low Q (e.g., 2): Transitions to log earlier, more compressed highlights

---

## User Interface Specification

### Dialog Layout

Two-panel horizontal layout:
- **Left Panel** (320px): Controls and parameters
- **Right Panel** (flexible): Preview window

### Left Panel - Controls

#### Group: Input Images
```
[x] Use active RGB image

    R: [dropdown: image list     ▼]  (disabled when checkbox checked)
    G: [dropdown: image list     ▼]
    B: [dropdown: image list     ▼]
```

#### Group: Stretch Parameters
```
    Stretch (α): [====|=========] [  5.0 ]
                 0.1              50.0

  Q (softening): [===|==========] [  8.0 ]
                 0.1              30.0
                 
                 Lower Q = earlier log transition
```

#### Group: Black Point
```
[x] Link RGB channels

   Black Point: [=|=============] [ 0.000]
                -0.1             0.5
                
                [Auto] [Sample]
                
--- When unlinked: ---

     Black (R): [=|=============] [ 0.000]
     Black (G): [=|=============] [ 0.000]
     Black (B): [=|=============] [ 0.000]
```

#### Group: Color Options
```
    Saturation: [======|=======] [ 1.00 ]
                0.5              2.0

      Clipping: [Preserve Color (Lupton) ▼]
                 - Preserve Color (Lupton)
                 - Hard Clip
                 - Rescale to Max
```

#### Bottom Toolbar
```
[+] [💾] [📂]                    [Reset] [Apply]
```

### Right Panel - Preview

#### Preview Toolbar
```
[x] Real-Time Preview     [Before|Split|After]  [-] 100% [+] [1:1] [Fit]
```

#### Preview Canvas
- Black background
- Displays current image with stretch applied
- Split view: vertical divider with "BEFORE" / "AFTER" labels
- Crosshair at center (subtle, green, 50% opacity)
- Draggable split position slider below (only visible in Split mode)

#### Preview Info Bar
```
Cursor: (1247, 892) | R=0.342 G=0.287 B=0.198     512 × 384 px | 32-bit
```

### Status Bar (Bottom of Dialog)
```
Lupton RGB v1.0 | Based on Lupton et al. (2004) PASP 116:133    Processing: 0.34s
```

---

## Implementation Notes for PixInsight

### Script Type
JavaScript (.js) script with Dialog-based UI

### Key PixInsight Classes to Use

```javascript
// Dialog and controls
#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/NumericControl.jsh>

// Image access
var window = ImageWindow.activeWindow;
var view = window.currentView;
var image = view.image;

// For preview
var previewBitmap = new Bitmap(width, height);
```

### Image Processing Pattern

```javascript
// Get image data
var R = new Vector(image.width * image.height);
var G = new Vector(image.width * image.height);
var B = new Vector(image.width * image.height);

image.getSamples(R, new Rect(image.width, image.height), 0); // Red channel
image.getSamples(G, new Rect(image.width, image.height), 1); // Green channel
image.getSamples(B, new Rect(image.width, image.height), 2); // Blue channel

// Process pixel by pixel
for (var i = 0; i < R.length; i++) {
    var r = R.at(i);
    var g = G.at(i);
    var b = B.at(i);
    
    // Apply Lupton algorithm...
    
    R.set(i, r_out);
    G.set(i, g_out);
    B.set(i, b_out);
}

// Write back
image.setSamples(R, new Rect(image.width, image.height), 0);
image.setSamples(G, new Rect(image.width, image.height), 1);
image.setSamples(B, new Rect(image.width, image.height), 2);
```

### Preview Implementation

For real-time preview, use a scaled-down version of the image:

```javascript
// Create preview at reduced resolution for performance
var previewScale = 0.25; // 25% of full resolution
var previewWidth = Math.round(image.width * previewScale);
var previewHeight = Math.round(image.height * previewScale);

// Use Image.resample() or process every Nth pixel
// Update preview on parameter change with debouncing (100-200ms delay)
```

### Auto Black Point Calculation

```javascript
function calculateAutoBlackPoint(channel) {
    // Use median of lowest 1% of pixels, or
    // Use PixInsight's built-in statistics
    var stats = new ImageStatistics();
    stats.generate(image, channel);
    
    // Black point slightly above the noise floor
    return stats.median - 2.8 * stats.MAD; // ~0.5% clip point
}
```

### Sample Background (from preview click)

```javascript
// On preview click, sample a small region
function sampleBackground(x, y, radius) {
    radius = radius || 10;
    var samples = [];
    // Collect pixels in radius
    // Return median value
}
```

---

## File Structure

```
LuptonRGB/
├── LuptonRGB.js           # Main script
├── LuptonRGB-engine.js    # Algorithm implementation (optional separate file)
├── LuptonRGB-gui.js       # UI components (optional separate file)
└── README.md              # User documentation
```

---

## Testing Checklist

- [ ] Works on RGB color images
- [ ] Works with three separate mono images as input
- [ ] Preview updates in real-time without lag
- [ ] Split view divider is draggable
- [ ] Before/After/Split toggle works correctly
- [ ] Zoom controls work (-, +, 1:1, Fit)
- [ ] Auto Black Point calculates reasonable values
- [ ] Sample Background works from preview click
- [ ] Linked/Unlinked black points work correctly
- [ ] All three clipping modes produce expected results
- [ ] Apply creates new image instance (non-destructive)
- [ ] Reset restores default parameter values
- [ ] Save/Load instance icons work (process icon paradigm)
- [ ] Handles edge cases (pure black image, saturated image)
- [ ] Performance acceptable on large images (8k × 8k)

---

## UI Mockup (React Reference)

The following React component demonstrates the intended UI layout and behavior. Use this as a visual reference for the PixInsight dialog implementation.

```jsx
import React, { useState } from 'react';

const LuptonRGBMockup = () => {
  const [stretch, setStretch] = useState(5.0);
  const [Q, setQ] = useState(8.0);
  const [blackPoint, setBlackPoint] = useState(0.0);
  const [saturationBoost, setSaturationBoost] = useState(1.0);
  const [linkedChannels, setLinkedChannels] = useState(true);
  const [preview, setPreview] = useState(true);
  const [selectedR, setSelectedR] = useState('');
  const [selectedG, setSelectedG] = useState('');
  const [selectedB, setSelectedB] = useState('');
  const [useActiveImage, setUseActiveImage] = useState(true);
  const [blackR, setBlackR] = useState(0.0);
  const [blackG, setBlackG] = useState(0.0);
  const [blackB, setBlackB] = useState(0.0);
  const [previewMode, setPreviewMode] = useState('split');
  const [zoomLevel, setZoomLevel] = useState(100);
  const [splitPosition, setSplitPosition] = useState(50);

  const mockImages = [
    'integration_R', 'integration_G', 'integration_B',
    'integration_Ha', 'integration_OIII', 'integration_SII', 'RGB_master'
  ];

  const SliderControl = ({ label, value, onChange, min, max, step, tooltip, compact = false }) => (
    <div className={`flex items-center gap-2 ${compact ? 'mb-2' : 'mb-3'}`}>
      <label className={`${compact ? 'w-24' : 'w-28'} text-right text-sm text-gray-300`} title={tooltip}>
        {label}:
      </label>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(e) => onChange(parseFloat(e.target.value))}
        className="flex-1 h-1.5 bg-gray-600 rounded-lg appearance-none cursor-pointer accent-blue-500"
      />
      <input
        type="number"
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(e) => onChange(parseFloat(e.target.value))}
        className="w-16 px-1.5 py-0.5 bg-gray-700 border border-gray-600 rounded text-xs text-gray-200 text-right"
      />
    </div>
  );

  const ImageSelector = ({ label, value, onChange }) => (
    <div className="flex items-center gap-2 mb-1.5">
      <label className="w-8 text-right text-xs text-gray-300">{label}:</label>
      <select
        value={value}
        onChange={(e) => onChange(e.target.value)}
        className="flex-1 px-1.5 py-0.5 bg-gray-700 border border-gray-600 rounded text-xs text-gray-200"
        disabled={useActiveImage}
      >
        <option value="">Select...</option>
        {mockImages.map(img => (
          <option key={img} value={img}>{img}</option>
        ))}
      </select>
    </div>
  );

  const GroupBox = ({ title, children, compact = false }) => (
    <div className="border border-gray-600 rounded mb-3">
      <div className="bg-gray-700 px-2 py-1 border-b border-gray-600 rounded-t">
        <span className="text-xs font-medium text-gray-200">{title}</span>
      </div>
      <div className={compact ? 'p-2' : 'p-3'}>
        {children}
      </div>
    </div>
  );

  const PreviewCanvas = () => (
    <div className="relative w-full h-full bg-black overflow-hidden">
      <svg viewBox="0 0 400 300" className="w-full h-full">
        <defs>
          <radialGradient id="nebulaBefore" cx="50%" cy="50%" r="60%">
            <stop offset="0%" stopColor="#1a0a0a" />
            <stop offset="40%" stopColor="#0d0508" />
            <stop offset="100%" stopColor="#020102" />
          </radialGradient>
          
          <radialGradient id="nebulaAfter" cx="50%" cy="50%" r="60%">
            <stop offset="0%" stopColor="#ff6b6b" stopOpacity="0.8" />
            <stop offset="30%" stopColor="#c92a2a" stopOpacity="0.6" />
            <stop offset="50%" stopColor="#862e9c" stopOpacity="0.4" />
            <stop offset="70%" stopColor="#364fc7" stopOpacity="0.3" />
            <stop offset="100%" stopColor="#0a0a12" />
          </radialGradient>

          <radialGradient id="starGlow">
            <stop offset="0%" stopColor="#ffffff" />
            <stop offset="30%" stopColor="#ffffcc" stopOpacity="0.8" />
            <stop offset="100%" stopColor="#ffff00" stopOpacity="0" />
          </radialGradient>

          <radialGradient id="blueStarGlow">
            <stop offset="0%" stopColor="#ffffff" />
            <stop offset="30%" stopColor="#99ccff" stopOpacity="0.8" />
            <stop offset="100%" stopColor="#3366ff" stopOpacity="0" />
          </radialGradient>

          <radialGradient id="redStarGlow">
            <stop offset="0%" stopColor="#ffffff" />
            <stop offset="30%" stopColor="#ffcccc" stopOpacity="0.8" />
            <stop offset="100%" stopColor="#ff6666" stopOpacity="0" />
          </radialGradient>

          <clipPath id="leftHalf">
            <rect x="0" y="0" width={splitPosition * 4} height="300" />
          </clipPath>
          <clipPath id="rightHalf">
            <rect x={splitPosition * 4} y="0" width={400 - splitPosition * 4} height="300" />
          </clipPath>
        </defs>

        <rect width="400" height="300" fill="#030306" />

        {previewMode === 'split' ? (
          <>
            <g clipPath="url(#leftHalf)">
              <ellipse cx="200" cy="150" rx="150" ry="100" fill="url(#nebulaBefore)" />
              <circle cx="80" cy="60" r="1.5" fill="#333333" />
              <circle cx="320" cy="80" r="1" fill="#222222" />
              <circle cx="150" cy="220" r="1.2" fill="#2a2a2a" />
              <circle cx="280" cy="200" r="0.8" fill="#252525" />
              <circle cx="50" cy="180" r="1" fill="#282828" />
              <circle cx="350" cy="140" r="1.3" fill="#2c2c2c" />
            </g>

            <g clipPath="url(#rightHalf)">
              <ellipse cx="200" cy="150" rx="150" ry="100" fill="url(#nebulaAfter)" />
              <circle cx="80" cy="60" r="8" fill="url(#starGlow)" />
              <circle cx="80" cy="60" r="2" fill="#ffffff" />
              <circle cx="320" cy="80" r="6" fill="url(#blueStarGlow)" />
              <circle cx="320" cy="80" r="1.5" fill="#ffffff" />
              <circle cx="150" cy="220" r="7" fill="url(#redStarGlow)" />
              <circle cx="150" cy="220" r="1.8" fill="#ffffff" />
              <circle cx="280" cy="200" r="4" fill="url(#starGlow)" />
              <circle cx="280" cy="200" r="1" fill="#ffffff" />
              <circle cx="50" cy="180" r="5" fill="url(#blueStarGlow)" />
              <circle cx="50" cy="180" r="1.2" fill="#ffffff" />
              <circle cx="350" cy="140" r="6" fill="url(#redStarGlow)" />
              <circle cx="350" cy="140" r="1.5" fill="#ffffff" />
            </g>

            <line x1={splitPosition * 4} y1="0" x2={splitPosition * 4} y2="300" stroke="#ffffff" strokeWidth="2" strokeDasharray="4,4" opacity="0.7" />
            
            <text x="10" y="20" fill="#888888" fontSize="11" fontFamily="monospace">BEFORE</text>
            <text x={400 - 50} y="20" fill="#ffffff" fontSize="11" fontFamily="monospace">AFTER</text>
          </>
        ) : previewMode === 'before' ? (
          <>
            <ellipse cx="200" cy="150" rx="150" ry="100" fill="url(#nebulaBefore)" />
            <circle cx="80" cy="60" r="1.5" fill="#333333" />
            <circle cx="320" cy="80" r="1" fill="#222222" />
            <circle cx="150" cy="220" r="1.2" fill="#2a2a2a" />
            <text x="10" y="20" fill="#888888" fontSize="11" fontFamily="monospace">BEFORE (Linear)</text>
          </>
        ) : (
          <>
            <ellipse cx="200" cy="150" rx="150" ry="100" fill="url(#nebulaAfter)" />
            <circle cx="80" cy="60" r="8" fill="url(#starGlow)" />
            <circle cx="80" cy="60" r="2" fill="#ffffff" />
            <circle cx="320" cy="80" r="6" fill="url(#blueStarGlow)" />
            <circle cx="320" cy="80" r="1.5" fill="#ffffff" />
            <circle cx="150" cy="220" r="7" fill="url(#redStarGlow)" />
            <circle cx="150" cy="220" r="1.8" fill="#ffffff" />
            <circle cx="280" cy="200" r="4" fill="url(#starGlow)" />
            <circle cx="280" cy="200" r="1" fill="#ffffff" />
            <text x="10" y="20" fill="#ffffff" fontSize="11" fontFamily="monospace">AFTER (Lupton RGB)</text>
          </>
        )}
      </svg>

      <div className="absolute inset-0 pointer-events-none flex items-center justify-center opacity-30">
        <div className="w-8 h-px bg-green-400"></div>
        <div className="absolute w-px h-8 bg-green-400"></div>
      </div>
    </div>
  );

  return (
    <div className="min-h-screen bg-gray-900 p-4 font-sans">
      <div className="max-w-4xl mx-auto bg-gray-800 rounded-lg shadow-2xl border border-gray-700">
        {/* Title Bar */}
        <div className="bg-gradient-to-r from-gray-700 to-gray-800 px-4 py-2 rounded-t-lg border-b border-gray-600 flex items-center justify-between">
          <div className="flex items-center gap-2">
            <div className="w-5 h-5 bg-blue-500 rounded flex items-center justify-center">
              <span className="text-white text-xs font-bold">L</span>
            </div>
            <span className="text-gray-200 font-medium">Lupton RGB Stretch</span>
          </div>
          <div className="flex gap-1.5">
            <div className="w-3 h-3 rounded-full bg-yellow-500 opacity-70 hover:opacity-100 cursor-pointer"></div>
            <div className="w-3 h-3 rounded-full bg-green-500 opacity-70 hover:opacity-100 cursor-pointer"></div>
            <div className="w-3 h-3 rounded-full bg-red-500 opacity-70 hover:opacity-100 cursor-pointer"></div>
          </div>
        </div>

        {/* Content - Two Column Layout */}
        <div className="flex">
          {/* Left Panel - Controls */}
          <div className="w-80 p-3 border-r border-gray-700 flex flex-col">
            <GroupBox title="Input Images" compact>
              <label className="flex items-center gap-2 text-xs text-gray-300 cursor-pointer mb-2">
                <input
                  type="checkbox"
                  checked={useActiveImage}
                  onChange={(e) => setUseActiveImage(e.target.checked)}
                  className="w-3 h-3 rounded bg-gray-700 border-gray-600 text-blue-500"
                />
                Use active RGB image
              </label>
              <div className={useActiveImage ? 'opacity-50' : ''}>
                <ImageSelector label="R" value={selectedR} onChange={setSelectedR} />
                <ImageSelector label="G" value={selectedG} onChange={setSelectedG} />
                <ImageSelector label="B" value={selectedB} onChange={setSelectedB} />
              </div>
            </GroupBox>

            <GroupBox title="Stretch Parameters" compact>
              <SliderControl
                label="Stretch (α)"
                value={stretch}
                onChange={setStretch}
                min={0.1}
                max={50}
                step={0.1}
                compact
              />
              <SliderControl
                label="Q (softening)"
                value={Q}
                onChange={setQ}
                min={0.1}
                max={30}
                step={0.1}
                compact
              />
              <p className="text-xs text-gray-500 ml-24 mt-1">Lower Q = earlier log transition</p>
            </GroupBox>

            <GroupBox title="Black Point" compact>
              <label className="flex items-center gap-2 text-xs text-gray-300 cursor-pointer mb-2">
                <input
                  type="checkbox"
                  checked={linkedChannels}
                  onChange={(e) => setLinkedChannels(e.target.checked)}
                  className="w-3 h-3 rounded bg-gray-700 border-gray-600 text-blue-500"
                />
                Link RGB channels
              </label>
              
              {linkedChannels ? (
                <SliderControl
                  label="Black Point"
                  value={blackPoint}
                  onChange={setBlackPoint}
                  min={-0.1}
                  max={0.5}
                  step={0.001}
                  compact
                />
              ) : (
                <>
                  <SliderControl label="Black (R)" value={blackR} onChange={setBlackR} min={-0.1} max={0.5} step={0.001} compact />
                  <SliderControl label="Black (G)" value={blackG} onChange={setBlackG} min={-0.1} max={0.5} step={0.001} compact />
                  <SliderControl label="Black (B)" value={blackB} onChange={setBlackB} min={-0.1} max={0.5} step={0.001} compact />
                </>
              )}
              
              <div className="flex gap-1.5 mt-2 ml-24">
                <button className="px-2 py-0.5 text-xs bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600">
                  Auto
                </button>
                <button className="px-2 py-0.5 text-xs bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600">
                  Sample
                </button>
              </div>
            </GroupBox>

            <GroupBox title="Color Options" compact>
              <SliderControl
                label="Saturation"
                value={saturationBoost}
                onChange={setSaturationBoost}
                min={0.5}
                max={2.0}
                step={0.05}
                compact
              />
              <div className="flex items-center gap-2 mt-1">
                <label className="w-24 text-right text-xs text-gray-300">Clipping:</label>
                <select className="flex-1 px-1.5 py-0.5 bg-gray-700 border border-gray-600 rounded text-xs text-gray-200">
                  <option value="preserve">Preserve Color</option>
                  <option value="clip">Hard Clip</option>
                  <option value="rescale">Rescale</option>
                </select>
              </div>
            </GroupBox>

            <div className="flex-1"></div>

            <div className="flex justify-between items-center pt-2 border-t border-gray-700 mt-2">
              <div className="flex gap-1">
                <button className="p-1.5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs" title="New Instance">➕</button>
                <button className="p-1.5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs" title="Save">💾</button>
                <button className="p-1.5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs" title="Load">📂</button>
              </div>
              
              <div className="flex gap-1.5">
                <button className="px-3 py-1 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs">
                  Reset
                </button>
                <button className="px-3 py-1 bg-blue-600 hover:bg-blue-500 text-white rounded border border-blue-500 text-xs font-medium">
                  Apply
                </button>
              </div>
            </div>
          </div>

          {/* Right Panel - Preview */}
          <div className="flex-1 p-3 flex flex-col">
            <div className="flex items-center justify-between mb-2 pb-2 border-b border-gray-700">
              <div className="flex items-center gap-2">
                <label className="flex items-center gap-1.5 text-xs text-gray-300 cursor-pointer">
                  <input
                    type="checkbox"
                    checked={preview}
                    onChange={(e) => setPreview(e.target.checked)}
                    className="w-3 h-3 rounded bg-gray-700 border-gray-600 text-blue-500"
                  />
                  Real-Time Preview
                </label>
              </div>
              
              <div className="flex items-center gap-3">
                <div className="flex bg-gray-700 rounded border border-gray-600">
                  <button 
                    onClick={() => setPreviewMode('before')}
                    className={`px-2 py-0.5 text-xs rounded-l ${previewMode === 'before' ? 'bg-blue-600 text-white' : 'text-gray-300 hover:bg-gray-600'}`}
                  >
                    Before
                  </button>
                  <button 
                    onClick={() => setPreviewMode('split')}
                    className={`px-2 py-0.5 text-xs border-l border-r border-gray-600 ${previewMode === 'split' ? 'bg-blue-600 text-white' : 'text-gray-300 hover:bg-gray-600'}`}
                  >
                    Split
                  </button>
                  <button 
                    onClick={() => setPreviewMode('after')}
                    className={`px-2 py-0.5 text-xs rounded-r ${previewMode === 'after' ? 'bg-blue-600 text-white' : 'text-gray-300 hover:bg-gray-600'}`}
                  >
                    After
                  </button>
                </div>

                <div className="flex items-center gap-1">
                  <button 
                    onClick={() => setZoomLevel(Math.max(25, zoomLevel - 25))}
                    className="w-5 h-5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs"
                  >
                    −
                  </button>
                  <span className="text-xs text-gray-400 w-10 text-center">{zoomLevel}%</span>
                  <button 
                    onClick={() => setZoomLevel(Math.min(400, zoomLevel + 25))}
                    className="w-5 h-5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs"
                  >
                    +
                  </button>
                  <button 
                    onClick={() => setZoomLevel(100)}
                    className="px-1.5 h-5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs ml-1"
                  >
                    1:1
                  </button>
                  <button 
                    className="px-1.5 h-5 bg-gray-700 hover:bg-gray-600 text-gray-300 rounded border border-gray-600 text-xs"
                    title="Fit to window"
                  >
                    Fit
                  </button>
                </div>
              </div>
            </div>

            <div className="flex-1 bg-black rounded border border-gray-700 overflow-hidden relative" style={{ minHeight: '320px' }}>
              <PreviewCanvas />
              
              {!preview && (
                <div className="absolute inset-0 bg-gray-900 bg-opacity-80 flex items-center justify-center">
                  <span className="text-gray-500 text-sm">Preview Disabled</span>
                </div>
              )}
            </div>

            {previewMode === 'split' && (
              <div className="flex items-center gap-2 mt-2">
                <span className="text-xs text-gray-400">Split:</span>
                <input
                  type="range"
                  min={10}
                  max={90}
                  value={splitPosition}
                  onChange={(e) => setSplitPosition(parseInt(e.target.value))}
                  className="flex-1 h-1 bg-gray-600 rounded-lg appearance-none cursor-pointer accent-blue-500"
                />
              </div>
            )}

            <div className="flex items-center justify-between mt-2 pt-2 border-t border-gray-700">
              <span className="text-xs text-gray-500">
                Cursor: (1247, 892) | Value: R=0.342 G=0.287 B=0.198
              </span>
              <span className="text-xs text-gray-500">
                512 × 384 px | 32-bit float
              </span>
            </div>
          </div>
        </div>

        <div className="bg-gray-750 px-4 py-1.5 rounded-b-lg border-t border-gray-700 flex justify-between">
          <span className="text-xs text-gray-500">
            Lupton RGB v1.0 | Based on Lupton et al. (2004) PASP 116:133
          </span>
          <span className="text-xs text-gray-500">
            Processing time: 0.34s
          </span>
        </div>
      </div>

      <div className="max-w-4xl mx-auto mt-4 bg-gray-800 rounded-lg border border-gray-700">
        <details className="group">
          <summary className="px-4 py-2 cursor-pointer text-sm text-gray-300 hover:bg-gray-750 rounded-lg flex items-center gap-2">
            <span className="transform group-open:rotate-90 transition-transform">▶</span>
            Algorithm Reference
          </summary>
          <div className="px-4 pb-3">
            <div className="bg-gray-900 rounded p-3 font-mono text-xs text-gray-300">
              <div className="text-green-400 mb-2">// Lupton RGB Stretch - Color Preserving</div>
              <div className="grid grid-cols-2 gap-4">
                <div>
                  <div className="mb-1"><span className="text-blue-400">I</span> = (R + G + B) / 3</div>
                  <div className="mb-1"><span className="text-blue-400">F(x)</span> = asinh(α × Q × (x - min)) / Q</div>
                  <div className="mb-1"><span className="text-blue-400">scale</span> = F(I) / I</div>
                </div>
                <div>
                  <div className="mb-1"><span className="text-purple-400">R'</span> = R × scale</div>
                  <div className="mb-1"><span className="text-green-400">G'</span> = G × scale</div>
                  <div className="mb-1"><span className="text-blue-400">B'</span> = B × scale</div>
                </div>
              </div>
              <div className="mt-2 text-yellow-400 text-xs">
                ★ If max(R',G',B') &gt; 1: scale all by 1/max → preserves hue during clipping
              </div>
            </div>
          </div>
        </details>
      </div>
    </div>
  );
};

export default LuptonRGBMockup;
```

---

## Acceptance Criteria

1. **Functional**: Algorithm produces output matching reference implementation (Astropy's `make_lupton_rgb`)
2. **Visual**: UI matches mockup layout with working preview
3. **Performance**: Real-time preview updates within 200ms on typical images
4. **Usability**: All controls respond correctly, parameter changes are reflected immediately
5. **Stability**: No crashes on edge cases (empty images, mono images, etc.)

---

## References

- Lupton et al. (2004) PASP 116:133 - https://ui.adsabs.harvard.edu/abs/2004PASP..116..133L
- Astropy implementation - https://docs.astropy.org/en/stable/visualization/rgb.html
- PixInsight JavaScript Reference - https://pixinsight.com/doc/pjsr/

---

*Document Version: 1.0*  
*Created: December 2024*
