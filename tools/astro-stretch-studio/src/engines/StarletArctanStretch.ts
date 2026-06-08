/**
 * StarletArctanStretch Engine
 *
 * Implements multiscale stretching using the starlet (isotropic undecimated wavelet)
 * transform with arctangent dynamic range compression and scale-dependent gain control.
 * Particularly suited for revealing faint extended structures while controlling bright cores.
 */

export interface SASParams {
  numScales: number;           // 4 - 8
  backgroundTarget: number;    // 0.05 - 0.25
  fineScaleGain: number;       // 0.5 - 2.0 (scales 1-2: stars, noise)
  midScaleGain: number;        // 1.0 - 5.0 (scales 3-4: fine structure)
  coarseScaleGain: number;     // 1.0 - 8.0 (scales 5-6: diffuse nebulosity)
  compressionAlpha: number;    // 1.0 - 20.0
  highlightProtection: number; // 0.0 - 1.0
  noiseThreshold: number;      // 0.0 - 0.01
  flattenBackground: boolean;
  preserveColor: boolean;
}

export const DEFAULT_SAS_PARAMS: SASParams = {
  numScales: 6,
  backgroundTarget: 0.12,
  fineScaleGain: 0.8,
  midScaleGain: 2.5,
  coarseScaleGain: 4.0,
  compressionAlpha: 8.0,
  highlightProtection: 0.5,
  noiseThreshold: 0.001,
  flattenBackground: true,
  preserveColor: true
};

// Preset configurations
export const SAS_PRESETS = {
  emissionNebula: {
    numScales: 6,
    fineScaleGain: 0.7,
    midScaleGain: 2.5,
    coarseScaleGain: 5.0,
    compressionAlpha: 10.0,
    highlightProtection: 0.5,
    backgroundTarget: 0.12,
    noiseThreshold: 0.001,
    flattenBackground: true,
    preserveColor: true
  },
  galaxy: {
    numScales: 6,
    fineScaleGain: 0.9,
    midScaleGain: 1.8,
    coarseScaleGain: 3.0,
    compressionAlpha: 6.0,
    highlightProtection: 0.7,
    backgroundTarget: 0.10,
    noiseThreshold: 0.0005,
    flattenBackground: true,
    preserveColor: true
  },
  starCluster: {
    numScales: 5,
    fineScaleGain: 1.2,
    midScaleGain: 1.5,
    coarseScaleGain: 2.0,
    compressionAlpha: 5.0,
    highlightProtection: 0.3,
    backgroundTarget: 0.08,
    noiseThreshold: 0.001,
    flattenBackground: true,
    preserveColor: true
  },
  faintExtended: {
    numScales: 7,
    fineScaleGain: 0.5,
    midScaleGain: 3.5,
    coarseScaleGain: 7.0,
    compressionAlpha: 15.0,
    highlightProtection: 0.6,
    backgroundTarget: 0.15,
    noiseThreshold: 0.002,
    flattenBackground: true,
    preserveColor: true
  }
};

// B3-spline filter coefficients [1, 4, 6, 4, 1] / 16
const B3_SPLINE = [1 / 16, 4 / 16, 6 / 16, 4 / 16, 1 / 16];

// Convolve 1D with B3 spline at given spacing (à trous algorithm)
function convolve1D(
  input: Float32Array,
  size: number,
  spacing: number
): Float32Array {
  const output = new Float32Array(size);

  for (let i = 0; i < size; i++) {
    let sum = 0;
    for (let k = -2; k <= 2; k++) {
      const idx = i + k * spacing;
      const clampedIdx = Math.max(0, Math.min(size - 1, idx));
      sum += B3_SPLINE[k + 2] * input[clampedIdx];
    }
    output[i] = sum;
  }

  return output;
}

// Separable 2D convolution with B3 spline
function convolveB3Spline2D(
  image: Float32Array,
  width: number,
  height: number,
  scale: number
): Float32Array {
  const spacing = Math.pow(2, scale);
  const temp = new Float32Array(width * height);
  const output = new Float32Array(width * height);

  // Horizontal pass
  for (let y = 0; y < height; y++) {
    const row = new Float32Array(width);
    for (let x = 0; x < width; x++) {
      row[x] = image[y * width + x];
    }
    const filteredRow = convolve1D(row, width, spacing);
    for (let x = 0; x < width; x++) {
      temp[y * width + x] = filteredRow[x];
    }
  }

  // Vertical pass
  for (let x = 0; x < width; x++) {
    const col = new Float32Array(height);
    for (let y = 0; y < height; y++) {
      col[y] = temp[y * width + x];
    }
    const filteredCol = convolve1D(col, height, spacing);
    for (let y = 0; y < height; y++) {
      output[y * width + x] = filteredCol[y];
    }
  }

  return output;
}

// Starlet (à trous wavelet) decomposition
function starletDecompose(
  image: Float32Array,
  width: number,
  height: number,
  numScales: number
): Float32Array[] {
  const scales: Float32Array[] = [];
  let current: Float32Array = Float32Array.from(image);

  for (let j = 0; j < numScales; j++) {
    const smooth = convolveB3Spline2D(current, width, height, j);

    // Wavelet coefficients = difference between scales
    const wavelet = new Float32Array(width * height);
    for (let i = 0; i < wavelet.length; i++) {
      wavelet[i] = current[i] - smooth[i];
    }

    scales.push(wavelet);
    current = Float32Array.from(smooth);
  }

  // Add residual (coarsest approximation)
  scales.push(current);

  return scales;
}

// Reconstruct image from starlet scales
function starletReconstruct(scales: Float32Array[]): Float32Array {
  const size = scales[0].length;
  const output = new Float32Array(size);

  for (const scale of scales) {
    for (let i = 0; i < size; i++) {
      output[i] += scale[i];
    }
  }

  return output;
}

// Estimate noise using MAD (Median Absolute Deviation)
function estimateNoise(fineScale: Float32Array): number {
  // Get absolute values
  const absValues: number[] = [];
  for (let i = 0; i < fineScale.length; i++) {
    absValues.push(Math.abs(fineScale[i]));
  }

  // Sort for median
  absValues.sort((a, b) => a - b);
  const median = absValues[Math.floor(absValues.length / 2)];

  // MAD
  const absDeviations: number[] = [];
  for (const v of absValues) {
    absDeviations.push(Math.abs(v - median));
  }
  absDeviations.sort((a, b) => a - b);
  const mad = absDeviations[Math.floor(absDeviations.length / 2)];

  // Convert to standard deviation (assuming Gaussian)
  return mad * 1.4826;
}

// Soft thresholding for noise reduction
function softThreshold(scale: Float32Array, threshold: number): void {
  for (let i = 0; i < scale.length; i++) {
    const w = scale[i];
    if (Math.abs(w) <= threshold) {
      scale[i] = 0;
    } else {
      scale[i] = w > 0 ? w - threshold : w + threshold;
    }
  }
}

// Compute scale-dependent gain
function computeScaleGain(j: number, params: SASParams): number {
  if (j <= 1) {
    return params.fineScaleGain;
  } else if (j <= 3) {
    const t = (j - 1.5) / 2.0;
    return (1 - t) * params.fineScaleGain + t * params.midScaleGain;
  } else if (j <= 5) {
    const t = (j - 3.5) / 2.0;
    return (1 - t) * params.midScaleGain + t * params.coarseScaleGain;
  }
  return params.coarseScaleGain;
}

// Sigmoid function
function sigmoid(x: number, center: number, sharpness: number): number {
  return 1 / (1 + Math.exp(-sharpness * (x - center)));
}

// Gaussian blur for intensity modulation (simple box filter approximation)
function gaussianBlur(
  image: Float32Array,
  width: number,
  height: number,
  sigma: number
): Float32Array {
  const radius = Math.ceil(sigma * 2);
  const output = new Float32Array(width * height);

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      let sum = 0;
      let count = 0;

      for (let dy = -radius; dy <= radius; dy++) {
        for (let dx = -radius; dx <= radius; dx++) {
          const nx = Math.max(0, Math.min(width - 1, x + dx));
          const ny = Math.max(0, Math.min(height - 1, y + dy));
          sum += image[ny * width + nx];
          count++;
        }
      }

      output[y * width + x] = sum / count;
    }
  }

  return output;
}

// Compute intensity modulation to prevent halos
function computeIntensityModulation(
  luminance: Float32Array,
  width: number,
  height: number,
  scale: number,
  strength: number
): Float32Array {
  const sigma = Math.pow(2, scale + 1);
  const smoothed = gaussianBlur(luminance, width, height, Math.min(sigma, 16));

  const modulation = new Float32Array(luminance.length);

  for (let i = 0; i < luminance.length; i++) {
    const intensity = smoothed[i];
    const sig = sigmoid(intensity, 0.5, 8);
    const mod = 1.0 - strength * sig;
    modulation[i] = Math.max(mod, 0.2);
  }

  return modulation;
}

// Arctangent compression
function arctanCompress(
  image: Float32Array,
  alpha: number,
  pivot: number
): void {
  const twoOverPi = 2 / Math.PI;

  for (let i = 0; i < image.length; i++) {
    const x = image[i];

    if (x > pivot) {
      const normalized = (x - pivot) / (1.0 - pivot);
      const compressed = twoOverPi * Math.atan(alpha * normalized);
      image[i] = pivot + compressed * (1.0 - pivot);
    }
  }
}

// Normalize background to target
function normalizeBackground(image: Float32Array, target: number): void {
  // Find current background (5th percentile)
  const sorted = Array.from(image).sort((a, b) => a - b);
  const percentile5 = sorted[Math.floor(sorted.length * 0.05)];

  if (percentile5 > 0 && percentile5 !== target) {
    const scale = target / percentile5;

    for (let i = 0; i < image.length; i++) {
      const v = image[i];
      if (v <= percentile5) {
        image[i] = v * scale;
      } else {
        image[i] = target + (v - percentile5) / (1.0 - percentile5) * (1.0 - target);
      }
    }
  }

  // Clamp to [0, 1]
  for (let i = 0; i < image.length; i++) {
    image[i] = Math.max(0, Math.min(1, image[i]));
  }
}

// Extract luminance from RGB
function rgbToLuminance(r: number, g: number, b: number): number {
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// Main SAS processing function
export function applySAS(
  imageData: ImageData,
  params: SASParams
): ImageData {
  const { width, height, data } = imageData;
  const numPixels = width * height;
  const result = new ImageData(width, height);
  const resultData = result.data;

  // Extract luminance
  const luminance = new Float32Array(numPixels);
  for (let i = 0; i < numPixels; i++) {
    const idx = i * 4;
    luminance[i] = rgbToLuminance(
      data[idx] / 255,
      data[idx + 1] / 255,
      data[idx + 2] / 255
    );
  }

  // Store original luminance for color reconstruction
  const originalLuminance = new Float32Array(luminance);

  // Starlet decomposition
  const scales = starletDecompose(luminance, width, height, params.numScales);

  // Estimate noise from finest scale
  const estimatedNoise = estimateNoise(scales[0]);

  // Process each wavelet scale
  for (let j = 0; j < params.numScales; j++) {
    const gain = computeScaleGain(j, params);

    // Noise thresholding for fine scales
    if (j <= 1) {
      const threshold = params.noiseThreshold * estimatedNoise * 5;
      softThreshold(scales[j], threshold);
    }

    // Apply gain with intensity modulation
    if (params.highlightProtection > 0) {
      const modulation = computeIntensityModulation(
        originalLuminance,
        width,
        height,
        j,
        params.highlightProtection
      );

      for (let i = 0; i < scales[j].length; i++) {
        scales[j][i] *= gain * modulation[i];
      }
    } else {
      for (let i = 0; i < scales[j].length; i++) {
        scales[j][i] *= gain;
      }
    }
  }

  // Process coarsest scale (background)
  if (params.flattenBackground) {
    const coarseTarget = params.backgroundTarget * 0.5;
    const blendFactor = 0.8;
    const coarseScale = scales[params.numScales];

    for (let i = 0; i < coarseScale.length; i++) {
      coarseScale[i] = coarseScale[i] * (1 - blendFactor) + coarseTarget * blendFactor;
    }
  }

  // Reconstruct from modified scales
  const enhanced = starletReconstruct(scales);

  // Apply arctangent compression
  arctanCompress(enhanced, params.compressionAlpha, params.backgroundTarget);

  // Normalize to target background
  normalizeBackground(enhanced, params.backgroundTarget);

  // Reconstruct color image
  for (let i = 0; i < numPixels; i++) {
    const idx = i * 4;
    const r = data[idx] / 255;
    const g = data[idx + 1] / 255;
    const b = data[idx + 2] / 255;
    const a = data[idx + 3];

    const origLum = originalLuminance[i];
    const newLum = enhanced[i];

    if (params.preserveColor && origLum > 0.001) {
      const scale = newLum / origLum;
      resultData[idx] = Math.min(255, Math.max(0, r * scale * 255));
      resultData[idx + 1] = Math.min(255, Math.max(0, g * scale * 255));
      resultData[idx + 2] = Math.min(255, Math.max(0, b * scale * 255));
    } else {
      resultData[idx] = Math.min(255, Math.max(0, newLum * 255));
      resultData[idx + 1] = Math.min(255, Math.max(0, newLum * 255));
      resultData[idx + 2] = Math.min(255, Math.max(0, newLum * 255));
    }
    resultData[idx + 3] = a;
  }

  return result;
}

// Get scale gains for visualization
export function getScaleGains(params: SASParams): number[] {
  const gains: number[] = [];
  for (let j = 0; j < params.numScales; j++) {
    gains.push(computeScaleGain(j, params));
  }
  return gains;
}
