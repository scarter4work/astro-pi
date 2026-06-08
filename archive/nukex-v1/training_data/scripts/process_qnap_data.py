#!/usr/bin/env python3
"""
Process QNAP linear FITS data for training.
Applies arcsinh stretch and generates training pairs with rule-based labeling.

PRIORITY: This script processes HIGHEST PRIORITY data (Priority 1).
QNAP data is the user's own images with known equipment, processing, and quality.
See DATA_SOURCES.md for the full data source priority hierarchy.

Source: QNAP mount at /mnt/qnap (CARTER-NAS at 192.168.68.81)
Priority: 1 (HIGHEST)
"""

import argparse
import numpy as np
import torch
from PIL import Image
from pathlib import Path
from astropy.io import fits
from scipy import ndimage
from skimage import filters, morphology
import sys
import json
from datetime import datetime

sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet

NUM_CLASSES = 21
CLASS_NAMES = [
    'background', 'star_bright', 'star_medium', 'star_faint', 'star_saturated',
    'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
    'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
    'dust_lane', 'star_cluster_open', 'star_cluster_globular',
    'artifact_hot_pixel', 'artifact_satellite', 'artifact_diffraction',
    'artifact_gradient', 'artifact_noise'
]

# Class indices
CLASS_BACKGROUND = 0
CLASS_STAR_BRIGHT = 1
CLASS_STAR_MEDIUM = 2
CLASS_STAR_FAINT = 3
CLASS_STAR_SATURATED = 4
CLASS_NEBULA_EMISSION = 5
CLASS_NEBULA_REFLECTION = 6
CLASS_NEBULA_DARK = 7
CLASS_NEBULA_PLANETARY = 8
CLASS_GALAXY_SPIRAL = 9
CLASS_GALAXY_ELLIPTICAL = 10
CLASS_GALAXY_IRREGULAR = 11
CLASS_GALAXY_CORE = 12
CLASS_DUST_LANE = 13
CLASS_STAR_CLUSTER_OPEN = 14
CLASS_STAR_CLUSTER_GLOBULAR = 15
CLASS_ARTIFACT_HOT_PIXEL = 16
CLASS_ARTIFACT_SATELLITE = 17
CLASS_ARTIFACT_DIFFRACTION = 18
CLASS_ARTIFACT_GRADIENT = 19
CLASS_ARTIFACT_NOISE = 20


def arcsinh_stretch(data, scale=0.1):
    """Apply arcsinh stretch to linear data."""
    data = data.astype(np.float32)
    if np.all(data == 0):
        return data
    valid = data[data > 0]
    if len(valid) == 0:
        return np.zeros_like(data)
    vmin, vmax = np.percentile(valid, [1, 99.5])
    data = np.clip((data - vmin) / (vmax - vmin + 1e-10), 0, 1)
    stretched = np.arcsinh(data / scale) / np.arcsinh(1.0 / scale)
    return np.clip(stretched, 0, 1)


def load_fits(fits_path):
    """Load FITS file and return stretched RGB."""
    with fits.open(fits_path) as hdul:
        data = hdul[0].data
        if data is None:
            data = hdul[1].data

        header = hdul[0].header

        if len(data.shape) == 3:
            if data.shape[0] == 3:
                data = np.transpose(data, (1, 2, 0))
            stretched = np.zeros_like(data, dtype=np.float32)
            for i in range(3):
                stretched[:, :, i] = arcsinh_stretch(data[:, :, i])
        else:
            stretched = arcsinh_stretch(data)
            stretched = np.stack([stretched, stretched, stretched], axis=-1)

        return stretched, header, data


def detect_stars(img, stretched):
    """Detect stars using multiple methods - conservative dilation to preserve nebula."""
    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img

    if len(stretched.shape) == 3:
        stretched_gray = np.mean(stretched, axis=2)
    else:
        stretched_gray = stretched

    # Find local maxima (star candidates)
    from scipy.ndimage import maximum_filter, minimum_filter

    local_max = maximum_filter(stretched_gray, size=5)
    local_min = minimum_filter(stretched_gray, size=5)

    # Stars are local maxima with significant contrast
    contrast = local_max - local_min
    stars = (stretched_gray == local_max) & (contrast > 0.08)  # Increased from 0.05

    # Classify by brightness
    star_mask = np.zeros_like(stretched_gray, dtype=np.uint8)

    # Saturated stars (very bright, possibly clipped)
    saturated = stars & (stretched_gray > 0.95)
    star_mask[saturated] = CLASS_STAR_SATURATED
    # Minimal dilation for saturated - just the core
    saturated_dilated = morphology.dilation(saturated, morphology.disk(1))
    star_mask[saturated_dilated & (star_mask == 0)] = CLASS_STAR_SATURATED

    # Bright stars
    bright = stars & (stretched_gray > 0.75) & (~saturated)  # Increased threshold
    star_mask[bright] = CLASS_STAR_BRIGHT
    # No additional dilation for bright stars

    # Medium stars
    medium = stars & (stretched_gray > 0.5) & (stretched_gray <= 0.75)
    star_mask[medium] = CLASS_STAR_MEDIUM

    # Faint stars - only very clear point sources
    faint = stars & (stretched_gray > 0.25) & (stretched_gray <= 0.5) & (contrast > 0.15)
    star_mask[faint] = CLASS_STAR_FAINT

    return star_mask


def detect_emission_nebula(stretched, star_mask, object_type='emission'):
    """Detect emission nebula regions - more aggressive detection."""
    if len(stretched.shape) == 3:
        gray = np.mean(stretched, axis=2)
    else:
        gray = stretched

    # Emission nebulae: diffuse regions with brightness above background
    # Use multiple scales to capture both large and small nebula features
    smoothed_fine = ndimage.gaussian_filter(gray, sigma=5)
    smoothed_coarse = ndimage.gaussian_filter(gray, sigma=20)

    # Calculate local background at large scale
    local_bg = ndimage.minimum_filter(smoothed_coarse, size=100)

    # Global background estimate (robust)
    bg_level = np.percentile(gray[star_mask == 0] if np.any(star_mask == 0) else gray, 10)

    # Emission detection with multiple criteria (more aggressive)
    # 1. Above local background
    above_local = (smoothed_fine - local_bg) > 0.02  # Reduced from 0.05

    # 2. Above global background threshold
    above_global = gray > (bg_level + 0.03)

    # 3. Has diffuse structure (not just noise)
    structure = smoothed_fine > 0.08

    # Combine criteria - any diffuse brightness counts as emission
    emission = (above_local | above_global) & structure

    # Remove star regions
    emission = emission & (star_mask == 0)

    # Gentle cleanup - preserve nebula structure
    emission = morphology.opening(emission, morphology.disk(1))  # Reduced from disk(3)
    emission = morphology.closing(emission, morphology.disk(3))  # Reduced from disk(5)

    # Fill small holes in nebula regions
    emission = ndimage.binary_fill_holes(emission)

    return emission


def detect_dark_nebula(stretched, star_mask):
    """Detect dark nebula regions."""
    if len(stretched.shape) == 3:
        gray = np.mean(stretched, axis=2)
    else:
        gray = stretched

    # Dark nebulae: regions darker than surroundings
    smoothed = ndimage.gaussian_filter(gray, sigma=5)
    local_max = ndimage.maximum_filter(smoothed, size=30)

    # Dark regions have significant absorption
    dark = (local_max - smoothed) > 0.1
    dark = dark & (smoothed < 0.4)  # Must be actually dark
    dark = dark & (star_mask == 0)  # Not stars

    # Clean up
    dark = morphology.opening(dark, morphology.disk(2))

    return dark


def detect_gradients(stretched):
    """Detect sensor gradients (artifacts)."""
    if len(stretched.shape) == 3:
        gray = np.mean(stretched, axis=2)
    else:
        gray = stretched

    # Very smooth gradient detection
    very_smooth = ndimage.gaussian_filter(gray, sigma=50)

    # Gradients are smooth variations at edges
    h, w = gray.shape
    gradient_mask = np.zeros_like(gray, dtype=bool)

    # Check edges for gradients
    edge_width = int(0.1 * min(h, w))

    # Top edge
    top_gradient = np.mean(very_smooth[:edge_width, :]) - np.mean(very_smooth[edge_width:2*edge_width, :])
    if abs(top_gradient) > 0.02:
        gradient_mask[:edge_width, :] = True

    # Bottom edge
    bottom_gradient = np.mean(very_smooth[-edge_width:, :]) - np.mean(very_smooth[-2*edge_width:-edge_width, :])
    if abs(bottom_gradient) > 0.02:
        gradient_mask[-edge_width:, :] = True

    # Left edge
    left_gradient = np.mean(very_smooth[:, :edge_width]) - np.mean(very_smooth[:, edge_width:2*edge_width])
    if abs(left_gradient) > 0.02:
        gradient_mask[:, :edge_width] = True

    # Right edge
    right_gradient = np.mean(very_smooth[:, -edge_width:]) - np.mean(very_smooth[:, -2*edge_width:-edge_width])
    if abs(right_gradient) > 0.02:
        gradient_mask[:, -edge_width:] = True

    return gradient_mask


def label_emission_nebula_image(stretched, linear_data, filename):
    """Label an emission nebula image."""
    mask = np.zeros(stretched.shape[:2], dtype=np.uint8)

    # First detect stars
    star_mask = detect_stars(linear_data, stretched)
    mask[star_mask > 0] = star_mask[star_mask > 0]

    # Detect emission nebula
    emission = detect_emission_nebula(stretched, star_mask, 'emission')
    mask[emission & (mask == 0)] = CLASS_NEBULA_EMISSION

    # Detect dark nebula
    dark = detect_dark_nebula(stretched, star_mask)
    mask[dark & (mask == 0)] = CLASS_NEBULA_DARK

    # Detect gradients
    gradients = detect_gradients(stretched)
    mask[gradients & (mask == 0)] = CLASS_ARTIFACT_GRADIENT

    # Remaining is background
    # mask stays 0 for background

    return mask


def label_reflection_nebula_image(stretched, linear_data, filename):
    """Label a reflection nebula image."""
    mask = np.zeros(stretched.shape[:2], dtype=np.uint8)

    # First detect stars
    star_mask = detect_stars(linear_data, stretched)
    mask[star_mask > 0] = star_mask[star_mask > 0]

    # Reflection nebulae: diffuse blue regions near bright stars
    if len(stretched.shape) == 3:
        gray = np.mean(stretched, axis=2)
        # Reflection nebulae often appear bluish
        blue_excess = stretched[:, :, 2] - stretched[:, :, 0]  # B - R
    else:
        gray = stretched
        blue_excess = np.zeros_like(gray)

    # Multi-scale diffuse detection
    smoothed_fine = ndimage.gaussian_filter(gray, sigma=5)
    smoothed_coarse = ndimage.gaussian_filter(gray, sigma=15)

    # Local background
    local_bg = ndimage.minimum_filter(smoothed_coarse, size=80)

    # Global background
    bg_level = np.percentile(gray[star_mask == 0] if np.any(star_mask == 0) else gray, 10)

    # Reflection nebula detection criteria
    # 1. Above background
    above_bg = (smoothed_fine - local_bg) > 0.015

    # 2. Diffuse brightness
    diffuse = smoothed_fine > 0.06  # Reduced from 0.15

    # 3. Above global background
    above_global = gray > (bg_level + 0.02)

    # Combine - more permissive for reflection nebula
    reflection = (above_bg | diffuse) & above_global
    reflection = reflection & (star_mask == 0)

    # Gentle cleanup
    reflection = morphology.opening(reflection, morphology.disk(1))  # Reduced from disk(3)
    reflection = morphology.closing(reflection, morphology.disk(2))

    # Fill holes
    reflection = ndimage.binary_fill_holes(reflection)

    mask[reflection & (mask == 0)] = CLASS_NEBULA_REFLECTION

    # Detect dark nebula
    dark = detect_dark_nebula(stretched, star_mask)
    mask[dark & (mask == 0)] = CLASS_NEBULA_DARK

    # Gradients
    gradients = detect_gradients(stretched)
    mask[gradients & (mask == 0)] = CLASS_ARTIFACT_GRADIENT

    return mask


def label_planetary_nebula_image(stretched, linear_data, filename):
    """Label a planetary nebula image."""
    mask = np.zeros(stretched.shape[:2], dtype=np.uint8)

    # First detect stars
    star_mask = detect_stars(linear_data, stretched)
    mask[star_mask > 0] = star_mask[star_mask > 0]

    if len(stretched.shape) == 3:
        gray = np.mean(stretched, axis=2)
    else:
        gray = stretched

    # Planetary nebulae: ring or shell-like structures
    # Find the central bright region
    smoothed = ndimage.gaussian_filter(gray, sigma=5)

    # Threshold for nebula
    nebula = smoothed > 0.2
    nebula = nebula & (star_mask == 0)

    # Clean up
    nebula = morphology.opening(nebula, morphology.disk(2))

    mask[nebula & (mask == 0)] = CLASS_NEBULA_PLANETARY

    # Gradients
    gradients = detect_gradients(stretched)
    mask[gradients & (mask == 0)] = CLASS_ARTIFACT_GRADIENT

    return mask


def label_dark_nebula_image(stretched, linear_data, filename):
    """Label a dark nebula image."""
    mask = np.zeros(stretched.shape[:2], dtype=np.uint8)

    # First detect stars
    star_mask = detect_stars(linear_data, stretched)
    mask[star_mask > 0] = star_mask[star_mask > 0]

    # Detect dark nebula more aggressively
    if len(stretched.shape) == 3:
        gray = np.mean(stretched, axis=2)
    else:
        gray = stretched

    smoothed = ndimage.gaussian_filter(gray, sigma=5)
    local_max = ndimage.maximum_filter(smoothed, size=20)

    # Dark regions
    dark = (local_max - smoothed) > 0.05
    dark = dark & (smoothed < 0.5)
    dark = dark & (star_mask == 0)
    dark = morphology.opening(dark, morphology.disk(2))

    mask[dark & (mask == 0)] = CLASS_NEBULA_DARK

    # Gradients
    gradients = detect_gradients(stretched)
    mask[gradients & (mask == 0)] = CLASS_ARTIFACT_GRADIENT

    return mask


def process_fits_file(fits_path, output_dir, object_type, tile_size=512):
    """Process a FITS file and generate training tiles."""
    fits_path = Path(fits_path)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Processing: {fits_path.name}")

    # Load FITS
    stretched, header, linear_data = load_fits(fits_path)
    h, w = stretched.shape[:2]

    print(f"  Size: {w}x{h}")

    # Generate label mask based on object type
    if object_type == 'emission':
        mask = label_emission_nebula_image(stretched, linear_data, fits_path.name)
    elif object_type == 'reflection':
        mask = label_reflection_nebula_image(stretched, linear_data, fits_path.name)
    elif object_type == 'planetary':
        mask = label_planetary_nebula_image(stretched, linear_data, fits_path.name)
    elif object_type == 'dark':
        mask = label_dark_nebula_image(stretched, linear_data, fits_path.name)
    else:
        # Default to emission
        mask = label_emission_nebula_image(stretched, linear_data, fits_path.name)

    # Generate tiles
    tiles_created = 0
    base_name = fits_path.stem

    # Calculate tile grid
    n_tiles_x = max(1, w // tile_size)
    n_tiles_y = max(1, h // tile_size)

    for ty in range(n_tiles_y):
        for tx in range(n_tiles_x):
            # Calculate tile bounds
            x1 = tx * tile_size
            y1 = ty * tile_size
            x2 = min(x1 + tile_size, w)
            y2 = min(y1 + tile_size, h)

            # Extract tile
            tile_img = stretched[y1:y2, x1:x2]
            tile_mask = mask[y1:y2, x1:x2]

            # Resize if needed
            if tile_img.shape[0] != tile_size or tile_img.shape[1] != tile_size:
                tile_img_pil = Image.fromarray((tile_img * 255).astype(np.uint8))
                tile_img_pil = tile_img_pil.resize((tile_size, tile_size), Image.BILINEAR)
                tile_img = np.array(tile_img_pil).astype(np.float32) / 255.0

                tile_mask_pil = Image.fromarray(tile_mask)
                tile_mask_pil = tile_mask_pil.resize((tile_size, tile_size), Image.NEAREST)
                tile_mask = np.array(tile_mask_pil)

            # Save tile
            tile_name = f"{base_name}_tile_{ty}_{tx}"

            img_path = output_dir / f"{tile_name}_img.png"
            mask_path = output_dir / f"{tile_name}_mask.png"

            Image.fromarray((tile_img * 255).astype(np.uint8)).save(img_path)
            Image.fromarray(tile_mask).save(mask_path)

            tiles_created += 1

    print(f"  Created {tiles_created} tiles")
    return tiles_created


def main():
    parser = argparse.ArgumentParser(description='Process QNAP data for training')
    parser.add_argument('input', help='Input FITS file or directory')
    parser.add_argument('--output', required=True, help='Output directory')
    parser.add_argument('--type', choices=['emission', 'reflection', 'planetary', 'dark', 'narrowband'],
                        default='emission', help='Object type for labeling')
    parser.add_argument('--tile-size', type=int, default=512, help='Tile size')
    parser.add_argument('--limit', type=int, default=0, help='Limit number of files (0=all)')
    args = parser.parse_args()

    input_path = Path(args.input)

    if input_path.is_file():
        files = [input_path]
    else:
        files = list(input_path.glob('*.fits')) + list(input_path.glob('*.FITS'))

    if args.limit > 0:
        files = files[:args.limit]

    print(f"Processing {len(files)} files as '{args.type}' type")

    total_tiles = 0
    for f in files:
        tiles = process_fits_file(f, args.output, args.type, args.tile_size)
        total_tiles += tiles

    print(f"\nTotal tiles created: {total_tiles}")


if __name__ == '__main__':
    main()
