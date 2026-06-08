/**
 * OptimalTransportStretch Engine
 *
 * Implements optimal transport (Wasserstein distance minimization) based
 * image stretching for astrophotography. Finds the mathematically optimal
 * mapping between source histogram and target distribution.
 */

export type ObjectType = 'nebula' | 'galaxy' | 'starCluster' | 'darkNebula' | 'custom';

export interface OTSParams {
  objectType: ObjectType;
  backgroundTarget: number;      // 0.05 - 0.30
  stretchIntensity: number;      // 0.0 - 1.0
  protectHighlights: number;     // 0.0 - 1.0
  preserveColor: boolean;
}

export const DEFAULT_OTS_PARAMS: OTSParams = {
  objectType: 'nebula',
  backgroundTarget: 0.15,
  stretchIntensity: 0.75,
  protectHighlights: 0.3,
  preserveColor: true
};

// Beta distribution PDF approximation
function betaPDF(x: number, alpha: number, beta: number): number {
  if (x <= 0 || x >= 1) return 0;
  const B = (gamma(alpha) * gamma(beta)) / gamma(alpha + beta);
  return (Math.pow(x, alpha - 1) * Math.pow(1 - x, beta - 1)) / B;
}

// Gamma function approximation (Stirling)
function gamma(z: number): number {
  if (z < 0.5) {
    return Math.PI / (Math.sin(Math.PI * z) * gamma(1 - z));
  }
  z -= 1;
  const g = 7;
  const c = [
    0.99999999999980993, 676.5203681218851, -1259.1392167224028,
    771.32342877765313, -176.61502916214059, 12.507343278686905,
    -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7
  ];
  let x = c[0];
  for (let i = 1; i < g + 2; i++) {
    x += c[i] / (z + i);
  }
  const t = z + g + 0.5;
  return Math.sqrt(2 * Math.PI) * Math.pow(t, z + 0.5) * Math.exp(-t) * x;
}

// Gaussian PDF
function gaussianPDF(x: number, mean: number, sigma: number): number {
  const coef = 1 / (sigma * Math.sqrt(2 * Math.PI));
  const exp = -0.5 * Math.pow((x - mean) / sigma, 2);
  return coef * Math.exp(exp);
}

// Scaled Beta PDF for a specific range
function scaledBetaPDF(x: number, alpha: number, beta: number, low: number, high: number): number {
  if (x < low || x > high) return 0;
  const normalized = (x - low) / (high - low);
  return betaPDF(normalized, alpha, beta) / (high - low);
}

// Generate target CDF for object type
export function generateTargetCDF(
  objectType: ObjectType,
  bgTarget: number,
  resolution: number = 256
): Float32Array {
  const pdf = new Float32Array(resolution);

  for (let i = 0; i < resolution; i++) {
    const x = i / (resolution - 1);

    switch (objectType) {
      case 'nebula':
        // Background peak (narrow Gaussian)
        pdf[i] += 0.3 * gaussianPDF(x, bgTarget, 0.03);
        // Nebula body (broad Beta distribution)
        pdf[i] += 0.5 * scaledBetaPDF(x, 2.0, 3.0, bgTarget, 0.7);
        // Highlight region (decreasing)
        pdf[i] += 0.2 * scaledBetaPDF(x, 1.5, 4.0, 0.6, 0.95);
        break;

      case 'galaxy':
        pdf[i] += 0.25 * gaussianPDF(x, bgTarget, 0.025);
        pdf[i] += 0.35 * scaledBetaPDF(x, 2.5, 2.5, bgTarget, 0.5);
        pdf[i] += 0.25 * scaledBetaPDF(x, 3.0, 2.0, 0.4, 0.75);
        // Uniform for bright regions
        if (x >= 0.7 && x <= 0.9) pdf[i] += 0.15 / 0.2;
        break;

      case 'starCluster':
        pdf[i] += 0.20 * gaussianPDF(x, bgTarget * 0.8, 0.02);
        pdf[i] += 0.50 * scaledBetaPDF(x, 1.5, 2.0, 0.15, 0.70);
        pdf[i] += 0.30 * scaledBetaPDF(x, 2.0, 5.0, 0.60, 0.95);
        break;

      case 'darkNebula':
        // Emphasize darker regions more
        pdf[i] += 0.15 * gaussianPDF(x, bgTarget * 1.3, 0.04);
        pdf[i] += 0.40 * scaledBetaPDF(x, 3.0, 2.0, 0.05, bgTarget);
        pdf[i] += 0.30 * scaledBetaPDF(x, 2.0, 2.5, bgTarget, 0.55);
        if (x >= 0.5 && x <= 0.85) pdf[i] += 0.15 / 0.35;
        break;

      default:
        // Linear fallback
        pdf[i] = 1.0;
    }
  }

  // Normalize PDF
  let sum = 0;
  for (let i = 0; i < resolution; i++) sum += pdf[i];
  for (let i = 0; i < resolution; i++) pdf[i] /= sum;

  // Compute CDF
  const cdf = new Float32Array(resolution);
  cdf[0] = pdf[0];
  for (let i = 1; i < resolution; i++) {
    cdf[i] = cdf[i - 1] + pdf[i];
  }

  // Normalize CDF to [0, 1]
  for (let i = 0; i < resolution; i++) {
    cdf[i] /= cdf[resolution - 1];
  }

  return cdf;
}

// Compute histogram from image data
export function computeHistogram(
  data: Float32Array,
  bins: number = 256
): Float32Array {
  const hist = new Float32Array(bins);

  for (let i = 0; i < data.length; i++) {
    const bin = Math.min(bins - 1, Math.max(0, Math.floor(data[i] * bins)));
    hist[bin]++;
  }

  return hist;
}

// Compute CDF from histogram
export function histogramToCDF(hist: Float32Array): Float32Array {
  const cdf = new Float32Array(hist.length);
  let sum = 0;

  for (let i = 0; i < hist.length; i++) {
    sum += hist[i];
    cdf[i] = sum;
  }

  // Normalize
  if (sum > 0) {
    for (let i = 0; i < cdf.length; i++) {
      cdf[i] /= sum;
    }
  }

  return cdf;
}

// Inverse CDF lookup using binary search
function inverseCDF(cdf: Float32Array, quantile: number): number {
  let low = 0;
  let high = cdf.length - 1;

  while (low < high) {
    const mid = Math.floor((low + high) / 2);
    if (cdf[mid] < quantile) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  return low / (cdf.length - 1);
}

// Smooth step function
function smoothStep(x: number, edge0: number, edge1: number): number {
  const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
  return t * t * (3 - 2 * t);
}

// Linear interpolation
function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

// Extract luminance from RGB (CIE formula)
function rgbToLuminance(r: number, g: number, b: number): number {
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// Compute transport map
export function computeTransportMap(
  sourceCDF: Float32Array,
  targetCDF: Float32Array,
  params: OTSParams
): Float32Array {
  const resolution = sourceCDF.length;
  const transportMap = new Float32Array(resolution);

  for (let i = 0; i < resolution; i++) {
    const sourceQuantile = sourceCDF[i];
    const mapped = inverseCDF(targetCDF, sourceQuantile);
    transportMap[i] = mapped;
  }

  // Highlight protection: blend with identity in bright regions
  if (params.protectHighlights > 0) {
    for (let i = 0; i < resolution; i++) {
      const x = i / (resolution - 1);
      const blend = smoothStep(x, 0.7, 0.95) * params.protectHighlights;
      transportMap[i] = lerp(transportMap[i], x, blend);
    }
  }

  // Stretch intensity: blend with identity
  for (let i = 0; i < resolution; i++) {
    const identity = i / (resolution - 1);
    transportMap[i] = lerp(identity, transportMap[i], params.stretchIntensity);
  }

  return transportMap;
}

// Apply OTS to ImageData (main entry point)
export function applyOTS(
  imageData: ImageData,
  params: OTSParams
): ImageData {
  const { width, height, data } = imageData;
  const numPixels = width * height;
  const result = new ImageData(width, height);
  const resultData = result.data;

  // Extract luminance channel
  const luminance = new Float32Array(numPixels);
  for (let i = 0; i < numPixels; i++) {
    const idx = i * 4;
    luminance[i] = rgbToLuminance(
      data[idx] / 255,
      data[idx + 1] / 255,
      data[idx + 2] / 255
    );
  }

  // Compute source histogram and CDF
  const sourceHist = computeHistogram(luminance, 256);
  const sourceCDF = histogramToCDF(sourceHist);

  // Generate target CDF
  const targetCDF = generateTargetCDF(params.objectType, params.backgroundTarget, 256);

  // Compute optimal transport map
  const transportMap = computeTransportMap(sourceCDF, targetCDF, params);

  // Apply the transformation
  for (let i = 0; i < numPixels; i++) {
    const idx = i * 4;
    const r = data[idx] / 255;
    const g = data[idx + 1] / 255;
    const b = data[idx + 2] / 255;
    const a = data[idx + 3];

    const lum = luminance[i];
    const bin = Math.min(255, Math.max(0, Math.floor(lum * 256)));
    const newLum = transportMap[bin];

    if (params.preserveColor && lum > 0.001) {
      // Preserve chrominance ratios
      const scale = newLum / lum;
      resultData[idx] = Math.min(255, Math.max(0, r * scale * 255));
      resultData[idx + 1] = Math.min(255, Math.max(0, g * scale * 255));
      resultData[idx + 2] = Math.min(255, Math.max(0, b * scale * 255));
    } else {
      // Grayscale or very dark pixels
      resultData[idx] = Math.min(255, Math.max(0, newLum * 255));
      resultData[idx + 1] = Math.min(255, Math.max(0, newLum * 255));
      resultData[idx + 2] = Math.min(255, Math.max(0, newLum * 255));
    }
    resultData[idx + 3] = a;
  }

  return result;
}

// Get histogram data for visualization
export function getHistogramData(imageData: ImageData): {
  source: Float32Array;
  stretched: Float32Array | null;
} {
  const { width, height, data } = imageData;
  const numPixels = width * height;

  const luminance = new Float32Array(numPixels);
  for (let i = 0; i < numPixels; i++) {
    const idx = i * 4;
    luminance[i] = rgbToLuminance(
      data[idx] / 255,
      data[idx + 1] / 255,
      data[idx + 2] / 255
    );
  }

  return {
    source: computeHistogram(luminance, 256),
    stretched: null
  };
}
