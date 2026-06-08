# EZ Stretch BSC

A collection of PixInsight tools for astrophotography - stretching scripts and Bayesian stacking.

## Scripts (PJSR)

### PhotometricStretch
Physics-based RGB asinh stretch with automatic sensor/filter detection from FITS metadata. Combines Lupton RGB mathematics, RNC-Color-Stretch workflow, and Veralux sensor-aware QE weighting.

**Features:**
- Automatic hardware detection from FITS headers
- Physics-based RGB weights from QE x filter curves
- Supports 24+ camera models, 8 sensor types, 20+ filters
- Presets for Galaxy, Nebula, Starfield

### LuptonRGB
Implementation of the Lupton et al. (2004) RGB stretch algorithm for creating color-preserving stretched images from high dynamic range astronomical data.

**Features:**
- Arcsinh stretch with configurable alpha and Q parameters
- Color-preserving clipping (scales all channels proportionally)
- Real-time preview with Before/Split/After modes
- Per-channel or linked black point support

### RNC-ColorStretch
Implementation of Roger N. Clark's rnc-color-stretch algorithm for advanced image stretching.

**Features:**
- Power stretch with x^(1/rootpower) transformation
- Two-pass stretch for finer control
- Automatic sky calibration
- Color recovery to preserve original ratios
- Multiple S-curve variants

## Native Modules (C++/Julia)

### BayesianAstro (In Development)
Distribution-aware image stacking with per-pixel confidence scoring. Uses Welford's algorithm for statistical accumulation, automatic distribution classification, and GPU acceleration via CUDA.

**Features:**
- Per-pixel statistical distribution tracking
- Distribution classification (Gaussian, Poisson, Bimodal, Skewed, Uniform)
- Confidence scoring based on distribution properties
- Multiple fusion strategies (MLE, confidence-weighted, lucky imaging, multi-scale)
- GPU acceleration (CUDA.jl)
- React-based UI embedded in PixInsight via Qt WebView

**Status:** Awaiting PixInsight certified developer access. See [BayesianAstro/README.md](BayesianAstro/README.md) for details.

## Installation

### Via PixInsight Repository Manager (Recommended)

1. In PixInsight, go to **Resources > Updates > Manage Repositories**
2. Click **Add** and enter this URL:
   ```
   https://raw.githubusercontent.com/scarter4work/EZ-Stretch-BSC/main/repository/
   ```
3. Click **OK**, then go to **Resources > Updates > Check for Updates**
4. Select the scripts you want and click **Apply**

### Manual Installation

1. Download or clone this repository
2. Copy the contents of `src/scripts/EZ Stretch BSC/` to your PixInsight scripts directory:
   - Windows: `C:\Users\<you>\AppData\Roaming\PixInsight\scripts\EZ Stretch BSC\`
   - macOS: `~/Library/Application Support/PixInsight/scripts/EZ Stretch BSC/`
   - Linux: `~/.PixInsight/scripts/EZ Stretch BSC/`
3. Restart PixInsight or use Script > Feature Scripts > Add

## References

- [Lupton et al. (2004)](https://www.astro.princeton.edu/~rhl/PrettyPictures/) - "Preparing Red-Green-Blue Images from CCD Data"
- [RNC-Color-Stretch](http://www.clarkvision.com/articles/astrophotography.software/) - Roger Clark
- [Veralux](https://gitlab.com/RikyPate/vera-lux-siril-scripts) - Riccardo Paterniti

## License

MIT License - See individual script headers for details.
