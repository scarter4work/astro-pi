# NukeX Layer-Aware Segmentation Architecture

## Overview

Traditional segmentation assigns each pixel to exactly one class. Layer-aware segmentation recognizes that astrophotography images contain **overlapping structures** at different depths:

```
Example: Horsehead Nebula with bright star
─────────────────────────────────────────
Layer 3 (front):  Star core (opaque, bright)
Layer 2:          Star halo/PSF (semi-transparent blue)
Layer 1:          Emission nebula (Ha-red)
Layer 0 (back):   Background sky
```

## Goals

1. **Detect overlapping regions** - Identify where foreground objects occlude background
2. **Estimate layer opacities** - How much does the halo obscure the nebula?
3. **Enable layer separation** - Subtract halos to reveal hidden structure
4. **Preserve physics** - Model PSF/halos mathematically for accurate subtraction

## Architecture

### 1. Multi-Head Neural Network

```
Input Image (H x W x 11 channels)
         │
         ▼
    ┌─────────────┐
    │   Encoder   │  (Shared U-Net backbone)
    │   (ResNet)  │
    └─────────────┘
         │
    ┌────┴────┬────────┬────────┐
    ▼         ▼        ▼        ▼
┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐
│Class  │ │Alpha  │ │Depth  │ │PSF    │
│Head   │ │Head   │ │Head   │ │Head   │
│(18ch) │ │(1ch)  │ │(1ch)  │ │(3ch)  │
└───────┘ └───────┘ └───────┘ └───────┘
    │         │        │        │
    ▼         ▼        ▼        ▼
 Classes   Opacity   Z-Order   PSF Params
```

### 2. Output Channels

| Head | Channels | Description |
|------|----------|-------------|
| **Class** | 18 | Probability for each region class |
| **Alpha** | 1 | Foreground opacity (0=transparent, 1=opaque) |
| **Depth** | 1 | Relative depth (0=back, 1=front) |
| **PSF** | 3 | PSF parameters (intensity, radius, beta) |

### 3. Layer Stack Data Structure

```cpp
struct Layer {
    RegionClass regionClass;    // What type of object
    float opacity;              // How opaque (0-1)
    float depth;                // Z-order (0=back, 1=front)
    Image mask;                 // Spatial extent
    Image intensity;            // Original intensity contribution

    // For star halos specifically
    PSFParameters psf;          // Point spread function params
};

struct LayerStack {
    std::vector<Layer> layers;  // Sorted by depth (back to front)
    Image composite;            // Final rendered composite

    // Decomposition methods
    Image RemoveLayer(int index);           // Remove a layer
    Image RevealBehind(int frontLayerIdx);  // Show what's behind
    Image SubtractPSF(const Layer& star);   // Subtract star halo
};
```

### 4. PSF (Point Spread Function) Model

Star halos follow predictable mathematical patterns:

```cpp
struct PSFParameters {
    float intensity;    // Peak brightness
    float fwhm;         // Full width at half maximum
    float beta;         // Moffat beta parameter (shape)
    float ellipticity;  // Elongation (diffraction spikes)
    float angle;        // Position angle

    // Compute PSF value at distance r from center
    float Evaluate(float r) const {
        // Moffat profile: I(r) = I0 * (1 + (r/alpha)^2)^(-beta)
        float alpha = fwhm / (2 * sqrt(pow(2, 1/beta) - 1));
        return intensity * pow(1 + pow(r/alpha, 2), -beta);
    }
};
```

### 5. Layer Separation Algorithm

```
Input: Observed image, Layer predictions

For each pixel (x, y):
    1. Get predicted layers at this pixel (sorted by depth)
    2. For each layer from front to back:
        a. If star halo: compute PSF contribution
        b. Subtract foreground contribution
        c. Divide by (1 - opacity) to recover background

Output: Separated layer images
```

**Mathematical formulation:**

```
observed(x,y) = Σ layer[i].intensity * layer[i].opacity * Π(1 - layer[j].opacity)
                i                                        j<i

To recover layer[k] behind layer[i]:
    recovered[k] = (observed - Σ layer[j].intensity * layer[j].opacity) / Π(1 - layer[j].opacity)
                               j>k                                       j>k
```

### 6. Training Data Format

Each training sample needs:
- Input image (H x W x 3 RGB)
- Multi-label mask (H x W x N_LAYERS) - which layers are present
- Alpha map (H x W) - foreground opacity
- Depth map (H x W) - relative depth ordering

**Labeling approach:**
1. Identify overlapping regions manually
2. Estimate opacity from color/intensity gradients
3. Use synthetic data (render known layers) to supplement

### 7. Loss Functions

```python
def layer_loss(pred, target):
    # Classification loss (which classes are present)
    class_loss = cross_entropy(pred['class'], target['class'])

    # Alpha loss (opacity estimation)
    alpha_loss = mse(pred['alpha'], target['alpha'])

    # Depth ordering loss
    depth_loss = mse(pred['depth'], target['depth'])

    # PSF parameter loss (for star regions)
    psf_loss = mse(pred['psf'], target['psf']) * target['is_star']

    # Reconstruction loss (can we recreate the image from layers?)
    reconstructed = composite_layers(pred)
    recon_loss = mse(reconstructed, input_image)

    return class_loss + alpha_loss + depth_loss + psf_loss + recon_loss
```

### 8. Inference Pipeline

```
1. Run multi-head model on input image
2. Extract layer predictions:
   - Class probabilities → identify regions
   - Alpha map → opacity at each pixel
   - Depth map → ordering of layers
   - PSF params → star halo characteristics

3. Build layer stack:
   - Group pixels by class and depth
   - Create separate layer images

4. Apply corrections:
   - Subtract star PSFs from occluded regions
   - Reveal hidden nebulosity
   - Blend results smoothly

5. Output options:
   - Corrected composite image
   - Individual layer images
   - Opacity/depth maps for manual adjustment
```

## Implementation Phases

### Phase 1: Multi-Label Segmentation
- Modify model to output multiple class probabilities per pixel
- Train to detect overlapping regions (star halo + nebula)
- No separation yet, just detection

### Phase 2: Alpha/Opacity Estimation
- Add alpha head to model
- Train on synthetic data with known opacities
- Enable basic "how much occlusion" estimation

### Phase 3: PSF Modeling
- Add PSF parameter head
- Train on star images with known PSF characteristics
- Enable mathematical halo subtraction

### Phase 4: Full Layer Separation
- Combine all heads for complete layer decomposition
- Implement reconstruction algorithm
- Add user controls for layer manipulation

## File Structure

```
src/engine/
├── LayerStack.h/.cpp         # Layer data structures
├── PSFModel.h/.cpp           # PSF mathematics
├── LayerSegmentation.h/.cpp  # Multi-head model wrapper
├── LayerSeparation.h/.cpp    # Decomposition algorithms
└── LayerBlending.h/.cpp      # Recombination and output

training/
├── scripts/
│   ├── train_layered.py      # Multi-head training
│   ├── generate_synthetic.py # Create synthetic training data
│   └── label_layers.py       # Multi-layer labeling tool
└── data/
    ├── layered_masks/        # Multi-channel layer masks
    └── synthetic/            # Synthetic training images
```

## Example Use Case: Horsehead Nebula

**Problem:** Bright star (σ Ori) creates massive blue halo obscuring red nebula

**Solution with Layer Architecture:**

1. Model detects:
   - Layer 0: Background (depth=0.0)
   - Layer 1: Ha nebula (depth=0.3, class=NebulaBright)
   - Layer 2: Star halo (depth=0.7, class=StarHalo, alpha=0.6)
   - Layer 3: Star core (depth=1.0, class=StarCore, alpha=1.0)

2. PSF head estimates halo parameters:
   - intensity=0.95, fwhm=150px, beta=2.5

3. Separation algorithm:
   - Subtracts PSF contribution from underlying pixels
   - Recovers nebula intensity: `nebula = (observed - psf) / (1 - 0.6)`

4. Output:
   - Clean nebula without blue halo contamination
   - Optional: keep subtle natural halo for aesthetics

## Future Extensions

1. **Diffraction spike removal** - Model and subtract spikes separately
2. **Gradient background modeling** - Handle light pollution gradients
3. **Multi-scale layers** - Different layer structure at different scales
4. **User-guided refinement** - Interactive layer adjustment tools
5. **Video/time-series** - Track layers across multiple exposures
