#!/usr/bin/env python3
"""
Segment Composition Pipeline for NukeX

Core deliverable: Segment a linear astro image, apply optimal stretch per segment,
compose back into one balanced image.

This script implements:
1. Multiple stretch functions (arcsinh, GHS, MTF, linear)
2. Class-to-stretch mapping for all 21 segmentation classes
3. Soft blending composition using Gaussian-blurred masks
4. CLI interface for batch processing

Usage:
    python compose_segments.py --input linear.fits --mask segmentation.png --output stretched.tiff
    python compose_segments.py --input linear.tiff --mask mask.png --output out.png --blend-sigma 5.0

Copyright (c) 2026 Scott Carter
"""

import argparse
import sys
from pathlib import Path
from typing import Dict, Tuple, Callable, Optional, Any
import numpy as np

try:
    from scipy.ndimage import gaussian_filter
    HAS_SCIPY = True
except ImportError:
    HAS_SCIPY = False

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    from astropy.io import fits
    HAS_ASTROPY = True
except ImportError:
    HAS_ASTROPY = False

try:
    import tifffile
    HAS_TIFFFILE = True
except ImportError:
    HAS_TIFFFILE = False

# Import class definitions from segmentation palette
try:
    from segmentation_palette import CLASS_NAMES, NUM_CLASSES, CLASS_DISPLAY_NAMES
except ImportError:
    # Fallback definitions if palette not found
    NUM_CLASSES = 21
    CLASS_NAMES = [
        'background', 'star_bright', 'star_medium', 'star_faint', 'star_saturated',
        'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
        'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
        'dust_lane', 'star_cluster_open', 'star_cluster_globular',
        'artifact_hot_pixel', 'artifact_satellite', 'artifact_diffraction',
        'artifact_gradient', 'artifact_noise'
    ]
    CLASS_DISPLAY_NAMES = CLASS_NAMES

# Import mask post-processing functions
try:
    from mask_postprocess import postprocess_mask as clean_mask
    HAS_POSTPROCESS = True
except ImportError:
    HAS_POSTPROCESS = False


# =============================================================================
# STRETCH FUNCTIONS
# =============================================================================

def stretch_arcsinh(data: np.ndarray, scale: float = 0.1, normalize: bool = True) -> np.ndarray:
    """
    Asinh (inverse hyperbolic sine) stretch - excellent for stars.

    Compresses bright values while preserving faint detail.
    Similar to logarithmic but handles zero and negative values gracefully.

    Args:
        data: Input array (0-1 normalized linear data)
        scale: Controls compression strength. Lower = more compression.
               0.01 = very aggressive, 0.1 = moderate, 0.5 = mild
        normalize: If True, normalize output to 0-1

    Returns:
        Stretched array
    """
    # Prevent division by zero
    scale = max(scale, 1e-10)

    # Apply arcsinh stretch: arcsinh(x/scale) / arcsinh(1/scale)
    stretched = np.arcsinh(data / scale)

    if normalize:
        # Normalize to 0-1 range based on max theoretical value
        max_val = np.arcsinh(1.0 / scale)
        if max_val > 0:
            stretched = stretched / max_val

    return np.clip(stretched, 0, 1)


def stretch_ghs(data: np.ndarray, D: float = 1.0, b: float = 0.0,
                SP: float = 0.0, HP: float = 1.0, LP: float = 0.0) -> np.ndarray:
    """
    Generalized Hyperbolic Stretch (GHS) - excellent for nebulae.

    A sophisticated non-linear stretch that provides fine control over
    shadows, midtones, and highlights. Based on the PixInsight GHS process.

    Args:
        data: Input array (0-1 normalized linear data)
        D: Stretch factor (D > 1 = more stretch, D < 1 = less stretch)
        b: Local intensity balance (higher = emphasize brighter features)
        SP: Symmetry point (0-1) - pivot point for the stretch
        HP: Highlight protection (0-1) - prevents clipping bright areas
        LP: Shadow protection (0-1) - prevents crushing blacks

    Returns:
        Stretched array
    """
    # Ensure valid ranges
    D = max(D, 0.01)
    HP = np.clip(HP, 0.01, 1.0)
    LP = np.clip(LP, 0.0, 0.99)

    # Create output array
    stretched = np.zeros_like(data)

    # Apply protection limits
    protected = np.clip(data, LP, HP)

    # Normalize to protected range
    if HP > LP:
        normalized = (protected - LP) / (HP - LP)
    else:
        normalized = protected

    # Apply GHS transformation
    # Using simplified GHS formula: y = (1 + D) * x / (1 + D * x + b * (1 - x))
    numerator = (1 + D) * normalized
    denominator = 1 + D * normalized + b * (1 - normalized)

    # Prevent division by zero
    denominator = np.maximum(denominator, 1e-10)
    stretched = numerator / denominator

    # Symmetry point adjustment
    if SP > 0:
        # Shift and rescale around symmetry point
        mask_below = normalized < SP
        mask_above = ~mask_below

        if np.any(mask_below):
            stretched[mask_below] = stretched[mask_below] * SP
        if np.any(mask_above):
            stretched[mask_above] = SP + (stretched[mask_above] - SP) * (1 - SP)

    return np.clip(stretched, 0, 1)


def stretch_mtf(data: np.ndarray, midtone: float = 0.2,
                black_point: float = 0.0, white_point: float = 1.0) -> np.ndarray:
    """
    Midtone Transfer Function (MTF) stretch - excellent for background.

    A classic astro stretch that maps the midtone value to 0.5 in the output.
    Provides predictable, well-balanced results for calibrated data.

    Args:
        data: Input array (0-1 normalized linear data)
        midtone: Input value that will map to 0.5 in output (typical: 0.1-0.3)
        black_point: Input value to clip to black (0)
        white_point: Input value to clip to white (1)

    Returns:
        Stretched array
    """
    # Ensure valid parameters
    midtone = np.clip(midtone, 0.001, 0.999)

    # Apply black and white point
    clipped = np.clip(data, black_point, white_point)

    # Normalize to 0-1 after clipping
    if white_point > black_point:
        normalized = (clipped - black_point) / (white_point - black_point)
    else:
        normalized = clipped

    # MTF formula: y = (m-1) * x / ((2m-1) * x - m)
    # where m = midtone, solved for y = 0.5 when x = m

    # Simplified formula avoiding singularities:
    # y = x * (midtone - 1) / (x * (2 * midtone - 1) - midtone)

    m = midtone
    numerator = normalized * (m - 1)
    denominator = normalized * (2 * m - 1) - m

    # Handle edge cases
    stretched = np.where(
        np.abs(denominator) < 1e-10,
        normalized,  # Fall back to linear if denominator near zero
        numerator / denominator
    )

    return np.clip(stretched, 0, 1)


def stretch_linear(data: np.ndarray, black_point: float = 0.0,
                   white_point: float = 1.0, gamma: float = 1.0) -> np.ndarray:
    """
    Linear stretch with optional gamma - for dark features and artifacts.

    Simple linear rescaling, optionally with gamma correction.
    Preserves relative intensities within the specified range.

    Args:
        data: Input array (0-1 normalized linear data)
        black_point: Input value to map to 0
        white_point: Input value to map to 1
        gamma: Gamma correction (1.0 = no correction)

    Returns:
        Stretched array
    """
    # Apply black and white point
    if white_point > black_point:
        stretched = (data - black_point) / (white_point - black_point)
    else:
        stretched = data

    stretched = np.clip(stretched, 0, 1)

    # Apply gamma if not 1.0
    if gamma != 1.0 and gamma > 0:
        stretched = np.power(stretched, 1.0 / gamma)

    return stretched


def stretch_log(data: np.ndarray, scale: float = 1000.0) -> np.ndarray:
    """
    Logarithmic stretch - alternative for high dynamic range.

    Args:
        data: Input array (0-1 normalized linear data)
        scale: Scale factor (higher = more compression)

    Returns:
        Stretched array
    """
    scale = max(scale, 1.0)
    stretched = np.log1p(data * scale) / np.log1p(scale)
    return np.clip(stretched, 0, 1)


def stretch_sqrt(data: np.ndarray) -> np.ndarray:
    """
    Square root stretch - mild compression.

    Args:
        data: Input array (0-1 normalized linear data)

    Returns:
        Stretched array
    """
    return np.sqrt(np.clip(data, 0, 1))


# =============================================================================
# STRETCH FUNCTION REGISTRY
# =============================================================================

STRETCH_FUNCTIONS: Dict[str, Callable] = {
    'arcsinh': stretch_arcsinh,
    'ghs': stretch_ghs,
    'mtf': stretch_mtf,
    'linear': stretch_linear,
    'log': stretch_log,
    'sqrt': stretch_sqrt,
}


# =============================================================================
# CLASS-TO-STRETCH MAPPING
# =============================================================================

# Configuration mapping each class to its optimal stretch function and parameters
# Format: class_id -> (stretch_function_name, {parameters})

STRETCH_CONFIG: Dict[int, Tuple[str, Dict[str, Any]]] = {
    # Background - use MTF for smooth, even appearance
    0: ('mtf', {'midtone': 0.15, 'black_point': 0.0, 'white_point': 1.0}),

    # Stars - use arcsinh to compress bright cores while preserving halos
    1: ('arcsinh', {'scale': 0.05}),    # star_bright - aggressive compression
    2: ('arcsinh', {'scale': 0.10}),    # star_medium - moderate compression
    3: ('arcsinh', {'scale': 0.15}),    # star_faint - mild compression
    4: ('arcsinh', {'scale': 0.02}),    # star_saturated - very aggressive

    # Nebulae - use GHS for fine control over emission/reflection features
    5: ('ghs', {'D': 1.2, 'b': 0.1, 'HP': 0.85}),   # nebula_emission - enhance detail
    6: ('ghs', {'D': 0.8, 'b': 0.05, 'HP': 0.9}),   # nebula_reflection - preserve blue
    7: ('linear', {'black_point': 0.0, 'gamma': 1.0}),  # nebula_dark - PRESERVE
    8: ('ghs', {'D': 1.0, 'b': 0.15, 'HP': 0.9}),   # nebula_planetary - balanced

    # Galaxies - varying stretches based on morphology
    9: ('ghs', {'D': 1.1, 'b': 0.1, 'HP': 0.88}),   # galaxy_spiral - show arms
    10: ('ghs', {'D': 0.9, 'b': 0.05, 'HP': 0.92}), # galaxy_elliptical - smooth
    11: ('ghs', {'D': 1.0, 'b': 0.08, 'HP': 0.9}),  # galaxy_irregular
    12: ('arcsinh', {'scale': 0.08}),               # galaxy_core - compress core

    # Dust features - preserve dark structures
    13: ('linear', {'black_point': 0.0, 'white_point': 0.8, 'gamma': 1.0}),  # dust_lane

    # Star clusters - balance individual stars with collective appearance
    14: ('arcsinh', {'scale': 0.12}),   # star_cluster_open - see individual stars
    15: ('arcsinh', {'scale': 0.08}),   # star_cluster_globular - compressed core

    # Artifacts - minimize visibility or preserve for removal
    16: ('linear', {'black_point': 0.0, 'white_point': 0.5}),  # hot_pixel - suppress
    17: ('linear', {'black_point': 0.0, 'white_point': 0.3}),  # satellite - suppress
    18: ('arcsinh', {'scale': 0.15}),   # diffraction - reduce but keep natural
    19: ('mtf', {'midtone': 0.3}),      # gradient - smooth out
    20: ('mtf', {'midtone': 0.25}),     # noise - reduce contrast
}


def get_stretch_for_class(class_id: int) -> Tuple[Callable, Dict[str, Any]]:
    """
    Get the stretch function and parameters for a given class ID.

    Args:
        class_id: Segmentation class ID (0-20)

    Returns:
        Tuple of (stretch_function, parameters_dict)
    """
    if class_id not in STRETCH_CONFIG:
        # Default to MTF for unknown classes
        return STRETCH_FUNCTIONS['mtf'], {'midtone': 0.2}

    func_name, params = STRETCH_CONFIG[class_id]
    return STRETCH_FUNCTIONS[func_name], params


# =============================================================================
# COMPOSITION FUNCTIONS
# =============================================================================

def create_soft_mask(hard_mask: np.ndarray, sigma: float = 3.0) -> np.ndarray:
    """
    Create a soft (blurred) mask from a hard binary mask.

    Soft masks enable smooth blending between differently-stretched regions,
    avoiding harsh transitions at segment boundaries.

    Args:
        hard_mask: Binary mask (0 or 1) of shape (H, W)
        sigma: Gaussian blur sigma (larger = softer transitions)

    Returns:
        Soft mask with values 0-1, same shape as input
    """
    if HAS_SCIPY and sigma > 0:
        return gaussian_filter(hard_mask.astype(np.float32), sigma=sigma)
    else:
        # Fallback: return hard mask
        return hard_mask.astype(np.float32)


def compose_segments(
    linear_image: np.ndarray,
    segmentation_mask: np.ndarray,
    stretch_config: Optional[Dict[int, Tuple[str, Dict[str, Any]]]] = None,
    blend_sigma: float = 3.0,
    verbose: bool = False
) -> Tuple[np.ndarray, Dict[str, Any]]:
    """
    Apply per-segment stretching and blend back together.

    This is the core composition function that:
    1. Creates soft masks for each class (Gaussian blur of hard masks)
    2. Applies class-specific stretch to the linear data
    3. Blends using soft masks with proper weight normalization

    Args:
        linear_image: (H, W) or (H, W, 3) float32 linear data in range 0-1
        segmentation_mask: (H, W) int array of class labels 0-20
        stretch_config: Optional custom stretch configuration dict.
                       If None, uses default STRETCH_CONFIG.
        blend_sigma: Sigma for Gaussian blur on class masks (soft blending)
        verbose: Print progress information

    Returns:
        Tuple of (composed_image, metadata_dict)
        - composed_image: (H, W) or (H, W, 3) stretched and blended result
        - metadata_dict: Information about the composition process
    """
    config = stretch_config if stretch_config is not None else STRETCH_CONFIG

    # Handle both grayscale and RGB
    if linear_image.ndim == 2:
        is_rgb = False
        image = linear_image[:, :, np.newaxis]
    else:
        is_rgb = True
        image = linear_image

    H, W, C = image.shape

    # Initialize output and weight accumulator
    composed = np.zeros((H, W, C), dtype=np.float32)
    weight_sum = np.zeros((H, W, 1), dtype=np.float32)

    # Find which classes are present in the mask
    unique_classes = np.unique(segmentation_mask)

    metadata = {
        'classes_found': unique_classes.tolist(),
        'classes_stretched': [],
        'blend_sigma': blend_sigma,
        'image_shape': (H, W, C if is_rgb else 1),
    }

    if verbose:
        print(f"Composing {H}x{W} image with {len(unique_classes)} classes")
        print(f"Classes present: {unique_classes}")

    for class_id in unique_classes:
        class_id = int(class_id)

        # Create hard mask for this class
        hard_mask = (segmentation_mask == class_id)
        pixel_count = np.sum(hard_mask)

        if pixel_count == 0:
            continue

        # Create soft mask for blending
        soft_mask = create_soft_mask(hard_mask, sigma=blend_sigma)
        soft_mask = soft_mask[:, :, np.newaxis]  # Add channel dimension

        # Get stretch function and parameters for this class
        stretch_func, params = get_stretch_for_class(class_id)

        if verbose:
            class_name = CLASS_NAMES[class_id] if class_id < len(CLASS_NAMES) else f"class_{class_id}"
            func_name = STRETCH_CONFIG.get(class_id, ('mtf', {}))[0]
            print(f"  Class {class_id} ({class_name}): {pixel_count} pixels, stretch={func_name}")

        # Apply stretch to the linear image
        # We stretch the entire image but it will only contribute where mask is non-zero
        stretched = np.zeros_like(image)
        for c in range(C):
            stretched[:, :, c] = stretch_func(image[:, :, c], **params)

        # Accumulate weighted contribution
        composed += stretched * soft_mask
        weight_sum += soft_mask

        metadata['classes_stretched'].append({
            'class_id': class_id,
            'class_name': CLASS_NAMES[class_id] if class_id < len(CLASS_NAMES) else f"class_{class_id}",
            'pixel_count': int(pixel_count),
            'stretch_func': STRETCH_CONFIG.get(class_id, ('mtf', {}))[0],
        })

    # Normalize by weight sum (avoid division by zero)
    weight_sum = np.maximum(weight_sum, 1e-10)
    composed = composed / weight_sum

    # Clip to valid range
    composed = np.clip(composed, 0, 1)

    # Return to original dimensionality
    if not is_rgb:
        composed = composed[:, :, 0]

    return composed, metadata


def compose_segments_simple(
    linear_image: np.ndarray,
    segmentation_mask: np.ndarray,
    stretch_config: Optional[Dict[int, Tuple[str, Dict[str, Any]]]] = None
) -> np.ndarray:
    """
    Simplified composition without soft blending (hard boundaries).

    Faster but may show visible seams at segment boundaries.
    Use compose_segments() for production quality.

    Args:
        linear_image: (H, W) or (H, W, 3) float32 linear data
        segmentation_mask: (H, W) int array of class labels
        stretch_config: Optional custom stretch configuration

    Returns:
        Composed and stretched image
    """
    config = stretch_config if stretch_config is not None else STRETCH_CONFIG

    # Handle both grayscale and RGB
    if linear_image.ndim == 2:
        is_rgb = False
        image = linear_image[:, :, np.newaxis]
    else:
        is_rgb = True
        image = linear_image

    H, W, C = image.shape
    composed = np.zeros_like(image)

    for class_id in np.unique(segmentation_mask):
        class_id = int(class_id)
        mask = (segmentation_mask == class_id)

        if not np.any(mask):
            continue

        stretch_func, params = get_stretch_for_class(class_id)

        for c in range(C):
            stretched_channel = stretch_func(image[:, :, c], **params)
            composed[:, :, c][mask] = stretched_channel[mask]

    if not is_rgb:
        composed = composed[:, :, 0]

    return np.clip(composed, 0, 1)


# =============================================================================
# I/O FUNCTIONS
# =============================================================================

def load_image(filepath: str) -> np.ndarray:
    """
    Load an image from various formats (FITS, TIFF, PNG, etc.)

    Args:
        filepath: Path to image file

    Returns:
        Image as float32 array normalized to 0-1
    """
    path = Path(filepath)
    suffix = path.suffix.lower()

    if suffix in ['.fits', '.fit', '.fts']:
        if not HAS_ASTROPY:
            raise ImportError("astropy required for FITS files: pip install astropy")
        with fits.open(filepath) as hdul:
            data = hdul[0].data.astype(np.float32)
            # Handle FITS dimension ordering (often C, H, W)
            if data.ndim == 3 and data.shape[0] in [1, 3, 4]:
                data = np.transpose(data, (1, 2, 0))
                if data.shape[2] == 1:
                    data = data[:, :, 0]

    elif suffix in ['.tif', '.tiff']:
        if HAS_TIFFFILE:
            data = tifffile.imread(filepath).astype(np.float32)
        elif HAS_PIL:
            img = Image.open(filepath)
            data = np.array(img).astype(np.float32)
        else:
            raise ImportError("tifffile or PIL required for TIFF files")

    elif suffix in ['.png', '.jpg', '.jpeg', '.bmp']:
        if not HAS_PIL:
            raise ImportError("PIL required for image files: pip install pillow")
        img = Image.open(filepath)
        data = np.array(img).astype(np.float32)

    else:
        raise ValueError(f"Unsupported file format: {suffix}")

    # Normalize to 0-1
    if data.max() > 1.0:
        if data.max() > 255:
            # Likely 16-bit
            data = data / 65535.0
        else:
            # Likely 8-bit
            data = data / 255.0

    return data


def load_mask(filepath: str) -> np.ndarray:
    """
    Load a segmentation mask image.

    Args:
        filepath: Path to mask file (typically PNG with class IDs as pixel values)

    Returns:
        Mask as int array with class IDs
    """
    path = Path(filepath)
    suffix = path.suffix.lower()

    if suffix in ['.npy']:
        return np.load(filepath).astype(np.int32)

    elif suffix in ['.png', '.tif', '.tiff', '.bmp']:
        if HAS_PIL:
            img = Image.open(filepath)
            # Convert to single channel if RGB
            if img.mode == 'RGB' or img.mode == 'RGBA':
                # Assume R channel contains class IDs, or convert to grayscale
                img = img.convert('L')
            return np.array(img).astype(np.int32)
        elif HAS_TIFFFILE and suffix in ['.tif', '.tiff']:
            return tifffile.imread(filepath).astype(np.int32)
        else:
            raise ImportError("PIL required for mask files: pip install pillow")

    else:
        raise ValueError(f"Unsupported mask format: {suffix}")


def save_image(image: np.ndarray, filepath: str, bit_depth: int = 16):
    """
    Save an image to various formats.

    Args:
        image: Image array (H, W) or (H, W, 3), values 0-1
        filepath: Output path
        bit_depth: 8 or 16 for PNG/TIFF output
    """
    path = Path(filepath)
    suffix = path.suffix.lower()

    # Ensure output directory exists
    path.parent.mkdir(parents=True, exist_ok=True)

    # Convert to appropriate dtype
    if bit_depth == 16:
        output = (np.clip(image, 0, 1) * 65535).astype(np.uint16)
    else:
        output = (np.clip(image, 0, 1) * 255).astype(np.uint8)

    if suffix in ['.fits', '.fit', '.fts']:
        if not HAS_ASTROPY:
            raise ImportError("astropy required for FITS files")
        # FITS expects (C, H, W) for color images
        if output.ndim == 3:
            output = np.transpose(output, (2, 0, 1))
        hdu = fits.PrimaryHDU(output.astype(np.float32) / (65535 if bit_depth == 16 else 255))
        hdu.writeto(filepath, overwrite=True)

    elif suffix in ['.tif', '.tiff']:
        if HAS_TIFFFILE:
            tifffile.imwrite(filepath, output)
        elif HAS_PIL:
            img = Image.fromarray(output)
            img.save(filepath)
        else:
            raise ImportError("tifffile or PIL required for TIFF files")

    elif suffix in ['.png', '.jpg', '.jpeg', '.bmp']:
        if not HAS_PIL:
            raise ImportError("PIL required for image files")
        # PNG supports 16-bit via mode 'I;16', but PIL has issues with it
        # JPG does not support 16-bit at all
        if suffix == '.jpg' or suffix == '.jpeg' or suffix == '.png' or suffix == '.bmp':
            output = (np.clip(image, 0, 1) * 255).astype(np.uint8)
        # Handle grayscale by converting to RGB for compatibility
        if output.ndim == 2:
            output = np.stack([output, output, output], axis=-1)
        img = Image.fromarray(output)
        img.save(filepath)

    else:
        raise ValueError(f"Unsupported output format: {suffix}")


# =============================================================================
# DEMO / TEST FUNCTIONS
# =============================================================================

def create_synthetic_test_data(size: int = 512) -> Tuple[np.ndarray, np.ndarray]:
    """
    Create synthetic test data for demonstration.

    Creates a test image with different intensity regions and a corresponding
    segmentation mask.

    Args:
        size: Image size (square)

    Returns:
        Tuple of (image, mask)
    """
    # Create a synthetic "linear" image with different regions
    image = np.zeros((size, size, 3), dtype=np.float32)
    mask = np.zeros((size, size), dtype=np.int32)

    # Background (class 0) - low noise
    image[:, :] = 0.05 + np.random.normal(0, 0.01, (size, size, 3))
    mask[:, :] = 0

    # Add some "stars" (classes 1-4) - Gaussian blobs
    np.random.seed(42)
    for _ in range(50):
        x, y = np.random.randint(20, size-20, 2)
        brightness = np.random.uniform(0.3, 1.0)
        radius = np.random.randint(3, 15)

        yy, xx = np.ogrid[-radius:radius+1, -radius:radius+1]
        gaussian = np.exp(-(xx**2 + yy**2) / (2 * (radius/3)**2))

        # Determine star class based on brightness
        if brightness > 0.9:
            class_id = 4  # saturated
        elif brightness > 0.7:
            class_id = 1  # bright
        elif brightness > 0.5:
            class_id = 2  # medium
        else:
            class_id = 3  # faint

        # Add to image
        y1, y2 = max(0, y-radius), min(size, y+radius+1)
        x1, x2 = max(0, x-radius), min(size, x+radius+1)
        gy1, gy2 = radius - (y - y1), radius + (y2 - y)
        gx1, gx2 = radius - (x - x1), radius + (x2 - x)

        for c in range(3):
            image[y1:y2, x1:x2, c] += gaussian[gy1:gy2, gx1:gx2] * brightness
        mask[y1:y2, x1:x2] = np.where(gaussian[gy1:gy2, gx1:gx2] > 0.3, class_id, mask[y1:y2, x1:x2])

    # Add "nebula" region (class 5) - large diffuse area
    center_y, center_x = size // 2, size // 2
    yy, xx = np.ogrid[:size, :size]
    nebula_mask = ((xx - center_x)**2 + (yy - center_y)**2) < (size//3)**2
    noise = np.random.normal(0, 0.1, (size, size))
    for c in range(3):
        image[:, :, c] = np.where(nebula_mask, image[:, :, c] + 0.3 + noise * 0.1, image[:, :, c])

    # Mark nebula in mask (but not over stars)
    nebula_pixels = nebula_mask & (mask == 0)
    mask[nebula_pixels] = 5

    # Clip to valid range
    image = np.clip(image, 0, 1)

    return image, mask


def demo_stretches(verbose: bool = True):
    """
    Demonstrate the different stretch functions on a gradient.
    """
    # Create a linear gradient
    gradient = np.linspace(0, 1, 256).reshape(1, -1).astype(np.float32)

    stretches = {
        'linear (gamma=1)': stretch_linear(gradient),
        'linear (gamma=2)': stretch_linear(gradient, gamma=2.0),
        'sqrt': stretch_sqrt(gradient),
        'log (scale=100)': stretch_log(gradient, scale=100),
        'arcsinh (scale=0.1)': stretch_arcsinh(gradient, scale=0.1),
        'arcsinh (scale=0.02)': stretch_arcsinh(gradient, scale=0.02),
        'mtf (mid=0.1)': stretch_mtf(gradient, midtone=0.1),
        'mtf (mid=0.3)': stretch_mtf(gradient, midtone=0.3),
        'ghs (D=1.5)': stretch_ghs(gradient, D=1.5),
        'ghs (D=0.5)': stretch_ghs(gradient, D=0.5),
    }

    if verbose:
        print("\nStretch function comparison (input -> output at 50% gray):")
        print("-" * 50)
        for name, result in stretches.items():
            mid_in = 0.5
            mid_idx = 128
            mid_out = result[0, mid_idx]
            print(f"  {name:25s}: {mid_in:.3f} -> {mid_out:.3f}")

    return stretches


# =============================================================================
# CLI INTERFACE
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='Segment composition pipeline - apply optimal stretch per segment',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic usage
  python compose_segments.py --input linear.fits --mask seg.png --output stretched.tiff

  # With custom blend sigma
  python compose_segments.py --input img.tiff --mask mask.png --output out.png --blend-sigma 5.0

  # Run demo without input files
  python compose_segments.py --demo

  # Show stretch function comparison
  python compose_segments.py --show-stretches
        """
    )

    parser.add_argument('--input', '-i', type=str,
                        help='Input linear image (FITS, TIFF, PNG)')
    parser.add_argument('--mask', '-m', type=str,
                        help='Segmentation mask (PNG with class IDs)')
    parser.add_argument('--output', '-o', type=str,
                        help='Output stretched image')
    parser.add_argument('--blend-sigma', type=float, default=3.0,
                        help='Gaussian sigma for soft mask blending (default: 3.0)')
    parser.add_argument('--bit-depth', type=int, choices=[8, 16], default=16,
                        help='Output bit depth (default: 16)')
    parser.add_argument('--no-blend', action='store_true',
                        help='Disable soft blending (faster, hard edges)')
    parser.add_argument('--post-process', action='store_true',
                        help='Apply post-processing to clean the segmentation mask')
    parser.add_argument('--min-region-size', type=int, default=100,
                        help='Minimum region size for post-processing (default: 100)')
    parser.add_argument('--smooth-mask', action='store_true',
                        help='Smooth mask boundaries during post-processing')
    parser.add_argument('--smooth-sigma', type=float, default=1.0,
                        help='Sigma for mask boundary smoothing (default: 1.0)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print verbose progress')
    parser.add_argument('--demo', action='store_true',
                        help='Run demo with synthetic data')
    parser.add_argument('--show-stretches', action='store_true',
                        help='Show stretch function comparison')
    parser.add_argument('--list-classes', action='store_true',
                        help='List all class IDs and their stretch configurations')

    args = parser.parse_args()

    # Handle info commands
    if args.show_stretches:
        demo_stretches(verbose=True)
        return 0

    if args.list_classes:
        print("\nClass-to-Stretch Configuration:")
        print("=" * 70)
        print(f"{'ID':<4} {'Class Name':<25} {'Stretch':<10} {'Parameters'}")
        print("-" * 70)
        for i in range(NUM_CLASSES):
            func_name, params = STRETCH_CONFIG.get(i, ('mtf', {'midtone': 0.2}))
            param_str = ', '.join(f'{k}={v}' for k, v in params.items())
            print(f"{i:<4} {CLASS_NAMES[i]:<25} {func_name:<10} {param_str}")
        print("=" * 70)
        return 0

    # Demo mode
    if args.demo:
        print("Running demo with synthetic data...")
        image, mask = create_synthetic_test_data(512)

        if args.verbose:
            print(f"Created synthetic image: {image.shape}, range [{image.min():.3f}, {image.max():.3f}]")
            print(f"Created mask with classes: {np.unique(mask)}")

        # Compose
        if args.no_blend:
            composed = compose_segments_simple(image, mask)
        else:
            composed, metadata = compose_segments(
                image, mask,
                blend_sigma=args.blend_sigma,
                verbose=args.verbose
            )

        # Save outputs
        output_path = args.output or '/tmp/compose_demo_output.png'
        save_image(composed, output_path, args.bit_depth)
        print(f"Saved composed image to: {output_path}")

        # Save mask visualization
        mask_vis_path = Path(output_path).with_suffix('.mask.png')
        save_image(mask.astype(np.float32) / NUM_CLASSES, str(mask_vis_path), 8)
        print(f"Saved mask visualization to: {mask_vis_path}")

        # Save original for comparison
        orig_path = Path(output_path).with_suffix('.original.png')
        save_image(image, str(orig_path), args.bit_depth)
        print(f"Saved original (linear) to: {orig_path}")

        return 0

    # Normal mode - require input and mask
    if not args.input or not args.mask:
        parser.error("--input and --mask are required (or use --demo)")

    if not args.output:
        # Generate output name from input
        input_path = Path(args.input)
        args.output = str(input_path.with_suffix('.composed.tiff'))

    # Load images
    if args.verbose:
        print(f"Loading input: {args.input}")
    image = load_image(args.input)

    if args.verbose:
        print(f"Loading mask: {args.mask}")
    mask = load_mask(args.mask)

    # Apply post-processing to mask if requested
    if args.post_process:
        if not HAS_POSTPROCESS:
            print("Warning: mask_postprocess module not available, skipping post-processing")
        else:
            if args.verbose:
                print(f"Applying mask post-processing (min_region={args.min_region_size}, smooth={args.smooth_mask})...")
            mask = clean_mask(
                mask,
                morph=True,
                min_region=args.min_region_size,
                smooth=args.smooth_mask,
                sigma=args.smooth_sigma,
                verbose=args.verbose
            )

    # Validate shapes
    if image.shape[:2] != mask.shape[:2]:
        print(f"ERROR: Image shape {image.shape[:2]} doesn't match mask shape {mask.shape[:2]}")
        return 1

    if args.verbose:
        print(f"Image shape: {image.shape}, dtype: {image.dtype}")
        print(f"Image range: [{image.min():.4f}, {image.max():.4f}]")
        print(f"Mask shape: {mask.shape}, classes: {np.unique(mask)}")

    # Compose
    if args.no_blend:
        composed = compose_segments_simple(image, mask)
    else:
        composed, metadata = compose_segments(
            image, mask,
            blend_sigma=args.blend_sigma,
            verbose=args.verbose
        )

    # Save output
    if args.verbose:
        print(f"Saving output: {args.output}")
    save_image(composed, args.output, args.bit_depth)

    print(f"Done! Output saved to: {args.output}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
