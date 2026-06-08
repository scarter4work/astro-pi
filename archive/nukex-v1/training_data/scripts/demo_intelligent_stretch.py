#!/usr/bin/env python3
"""
Demo: Intelligent Stretching Workflow for NukeX

This script demonstrates the core value proposition of NukeX:
Using ML-based semantic segmentation to apply optimal stretches
to different regions of an astronomical image.

Workflow:
1. Load a linear (unstretched) FITS or TIFF image
2. Apply temporary arcsinh stretch for segmentation (model trained on stretched data)
3. Run 21-class segmentation model to get class masks
4. Apply optimal stretch to EACH detected class on the LINEAR data
5. Composite all stretched segments using soft blending
6. Output the final intelligently-stretched image

Key insight: Different astronomical features require different stretches:
- Stars: arcsinh to compress cores, preserve halos
- Emission nebulae: GHS with strong enhancement (D=0.3, b=2.5)
- Reflection nebulae: GHS with softer settings to preserve blue
- Background: MTF with low midtone for smooth appearance
- Dark nebulae: minimal stretch to preserve darkness

Usage:
    # Basic usage
    python demo_intelligent_stretch.py --input linear_image.fits --output stretched.tiff

    # With visualization of each segment
    python demo_intelligent_stretch.py --input linear.tiff --output out.tiff --visualize

    # Compare with traditional stretch
    python demo_intelligent_stretch.py --input linear.fits --output out.tiff --compare

    # Custom model and blend settings
    python demo_intelligent_stretch.py --input img.fits --model custom.pth --blend-sigma 5.0

Copyright (c) 2026 Scott Carter
"""

import argparse
import sys
from pathlib import Path
from typing import Dict, Tuple, Optional, List
import numpy as np
from datetime import datetime

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False
    print("ERROR: PyTorch is required. Install with: pip install torch")
    sys.exit(1)

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

try:
    from scipy.ndimage import gaussian_filter
    HAS_SCIPY = True
except ImportError:
    HAS_SCIPY = False

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False

# Add paths for NukeX modules
sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
sys.path.insert(0, '/home/scarter4work/projects/NukeX/training_data/scripts')

from model import AstroUNet
from segmentation_palette import (
    NUM_CLASSES, CLASS_NAMES, CLASS_DISPLAY_NAMES,
    CLASS_COLORS_RGB, apply_colormap
)

# Default model path (v8 test model)
DEFAULT_MODEL_PATH = '/home/scarter4work/projects/NukeX/training_data/models_v8_test/best_model.pth'


# =============================================================================
# STRETCH FUNCTIONS
# =============================================================================

def stretch_arcsinh(data: np.ndarray, scale: float = 0.1) -> np.ndarray:
    """
    Asinh stretch - excellent for stars.
    Compresses bright values while preserving faint detail.

    Args:
        data: Input array (0-1 normalized linear data)
        scale: Controls compression. Lower = more compression.

    Returns:
        Stretched array (0-1)
    """
    scale = max(scale, 1e-10)
    stretched = np.arcsinh(data / scale)
    max_val = np.arcsinh(1.0 / scale)
    if max_val > 0:
        stretched = stretched / max_val
    return np.clip(stretched, 0, 1)


def stretch_ghs(data: np.ndarray, D: float = 1.0, b: float = 0.0,
                SP: float = 0.0, HP: float = 1.0, LP: float = 0.0) -> np.ndarray:
    """
    Generalized Hyperbolic Stretch (GHS) - excellent for nebulae.
    Based on the PixInsight GHS process.

    Args:
        data: Input array (0-1 normalized linear data)
        D: Stretch factor (D > 1 = more stretch)
        b: Local intensity balance (higher = emphasize brighter features)
        SP: Symmetry point (0-1)
        HP: Highlight protection (0-1)
        LP: Shadow protection (0-1)

    Returns:
        Stretched array (0-1)
    """
    D = max(D, 0.01)
    HP = np.clip(HP, 0.01, 1.0)
    LP = np.clip(LP, 0.0, 0.99)

    protected = np.clip(data, LP, HP)

    if HP > LP:
        normalized = (protected - LP) / (HP - LP)
    else:
        normalized = protected

    # GHS transformation: y = (1 + D) * x / (1 + D * x + b * (1 - x))
    numerator = (1 + D) * normalized
    denominator = 1 + D * normalized + b * (1 - normalized)
    denominator = np.maximum(denominator, 1e-10)
    stretched = numerator / denominator

    if SP > 0:
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
    Maps the midtone value to 0.5 in the output.

    Args:
        data: Input array (0-1 normalized linear data)
        midtone: Input value that maps to 0.5 in output (typical: 0.1-0.3)
        black_point: Input value to clip to black
        white_point: Input value to clip to white

    Returns:
        Stretched array (0-1)
    """
    midtone = np.clip(midtone, 0.001, 0.999)
    clipped = np.clip(data, black_point, white_point)

    if white_point > black_point:
        normalized = (clipped - black_point) / (white_point - black_point)
    else:
        normalized = clipped

    m = midtone
    numerator = normalized * (m - 1)
    denominator = normalized * (2 * m - 1) - m

    stretched = np.where(
        np.abs(denominator) < 1e-10,
        normalized,
        numerator / denominator
    )

    return np.clip(stretched, 0, 1)


def stretch_linear(data: np.ndarray, black_point: float = 0.0,
                   white_point: float = 1.0, gamma: float = 1.0) -> np.ndarray:
    """
    Linear stretch with optional gamma - for dark features.

    Args:
        data: Input array (0-1 normalized linear data)
        black_point: Input value to map to 0
        white_point: Input value to map to 1
        gamma: Gamma correction (1.0 = no correction)

    Returns:
        Stretched array (0-1)
    """
    if white_point > black_point:
        stretched = (data - black_point) / (white_point - black_point)
    else:
        stretched = data

    stretched = np.clip(stretched, 0, 1)

    if gamma != 1.0 and gamma > 0:
        stretched = np.power(stretched, 1.0 / gamma)

    return stretched


# =============================================================================
# INTELLIGENT STRETCH CONFIGURATION
# =============================================================================

# Class-to-stretch mapping with astronomical rationale
# Format: class_id -> (stretch_func, params, description)
INTELLIGENT_STRETCH_CONFIG = {
    # Background (class 0): Smooth, even appearance
    0: (stretch_mtf, {'midtone': 0.15}, "MTF with low midtone for smooth background"),

    # Stars - arcsinh to compress cores, preserve halos
    1: (stretch_arcsinh, {'scale': 0.05}, "Aggressive arcsinh for bright stars"),
    2: (stretch_arcsinh, {'scale': 0.10}, "Moderate arcsinh for medium stars"),
    3: (stretch_arcsinh, {'scale': 0.15}, "Mild arcsinh for faint stars"),
    4: (stretch_arcsinh, {'scale': 0.02}, "Very aggressive for saturated stars"),

    # Nebulae - GHS for fine control
    5: (stretch_ghs, {'D': 0.3, 'b': 2.5, 'HP': 0.85}, "GHS for emission nebula - strong enhancement"),
    6: (stretch_ghs, {'D': 0.8, 'b': 0.05, 'HP': 0.9}, "Soft GHS for reflection nebula - preserve blue"),
    7: (stretch_linear, {'black_point': 0.0, 'gamma': 1.0}, "Linear to PRESERVE dark nebula darkness"),
    8: (stretch_ghs, {'D': 1.0, 'b': 0.15, 'HP': 0.9}, "Balanced GHS for planetary nebula"),

    # Galaxies - varying by morphology
    9: (stretch_ghs, {'D': 1.1, 'b': 0.1, 'HP': 0.88}, "GHS to show spiral arms"),
    10: (stretch_ghs, {'D': 0.9, 'b': 0.05, 'HP': 0.92}, "Smooth GHS for elliptical galaxy"),
    11: (stretch_ghs, {'D': 1.0, 'b': 0.08, 'HP': 0.9}, "Moderate GHS for irregular galaxy"),
    12: (stretch_arcsinh, {'scale': 0.08}, "Arcsinh to compress galaxy core"),

    # Dust features - preserve dark structures
    13: (stretch_linear, {'black_point': 0.0, 'white_point': 0.8, 'gamma': 1.0},
         "Linear stretch to preserve dust lane darkness"),

    # Star clusters
    14: (stretch_arcsinh, {'scale': 0.12}, "Arcsinh for open cluster - show individual stars"),
    15: (stretch_arcsinh, {'scale': 0.08}, "Arcsinh for globular cluster - compressed core"),

    # Artifacts - minimize or preserve for removal
    16: (stretch_linear, {'black_point': 0.0, 'white_point': 0.5}, "Suppress hot pixels"),
    17: (stretch_linear, {'black_point': 0.0, 'white_point': 0.3}, "Suppress satellite trails"),
    18: (stretch_arcsinh, {'scale': 0.15}, "Reduce diffraction spikes but keep natural"),
    19: (stretch_mtf, {'midtone': 0.3}, "Smooth out gradients"),
    20: (stretch_mtf, {'midtone': 0.25}, "Reduce noise contrast"),
}


# =============================================================================
# SEGMENTATION MODEL
# =============================================================================

def load_segmentation_model(model_path: str, device: torch.device) -> torch.nn.Module:
    """
    Load the trained 21-class segmentation model.

    Args:
        model_path: Path to .pth checkpoint
        device: torch device

    Returns:
        Loaded model in eval mode
    """
    model = AstroUNet(in_channels=4, num_classes=NUM_CLASSES, base_features=32)

    checkpoint = torch.load(model_path, map_location=device, weights_only=False)

    if 'model_state_dict' in checkpoint:
        model.load_state_dict(checkpoint['model_state_dict'])
        epoch = checkpoint.get('epoch', 'unknown')
        val_acc = checkpoint.get('val_acc', 'N/A')
        print(f"  Loaded checkpoint from epoch {epoch}")
        if isinstance(val_acc, float):
            print(f"  Validation accuracy: {val_acc:.4f}")
    else:
        model.load_state_dict(checkpoint)

    model = model.to(device)
    model.eval()
    return model


def preprocess_for_segmentation(linear_image: np.ndarray, size: int = 512) -> Tuple[torch.Tensor, np.ndarray]:
    """
    Preprocess linear image for segmentation model.

    The model was trained on arcsinh-stretched images, so we need to:
    1. Apply arcsinh stretch (temporary, for segmentation only)
    2. Percentile normalize
    3. Add color contrast channel (B-R)

    Args:
        linear_image: Linear image (H, W) or (H, W, 3), values 0-1
        size: Target size for model input

    Returns:
        img_tensor: Preprocessed tensor (1, 4, H, W)
        stretched_preview: The stretched image used for segmentation (H, W, 3)
    """
    # Ensure 3 channels
    if linear_image.ndim == 2:
        img = np.stack([linear_image, linear_image, linear_image], axis=-1)
    else:
        img = linear_image.copy()

    # Step 1: Apply arcsinh stretch (what the model was trained on)
    stretched = stretch_arcsinh(img, scale=0.1)

    # Step 2: Resize to model input size
    h, w = stretched.shape[:2]
    if h != size or w != size:
        # Use PIL for high-quality resize
        pil_img = Image.fromarray((stretched * 255).astype(np.uint8))
        pil_img = pil_img.resize((size, size), Image.BILINEAR)
        stretched_resized = np.array(pil_img).astype(np.float32) / 255.0
    else:
        stretched_resized = stretched.astype(np.float32)

    # Step 3: Percentile normalization (matches training)
    for c in range(3):
        p1, p99 = np.percentile(stretched_resized[:, :, c], [1, 99])
        if p99 > p1:
            stretched_resized[:, :, c] = np.clip(
                (stretched_resized[:, :, c] - p1) / (p99 - p1), 0, 1
            )

    # Step 4: Add color contrast channel (B-R)
    blue = stretched_resized[:, :, 2]
    red = stretched_resized[:, :, 0]
    green = stretched_resized[:, :, 1]

    # Detect narrowband/monochrome
    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_narrowband = channel_std < 0.01

    if is_narrowband:
        color_contrast = np.zeros_like(blue)[:, :, np.newaxis]
    else:
        color_contrast = (blue - red)[:, :, np.newaxis]

    # Stack as 4 channels
    img_4ch = np.concatenate([stretched_resized, color_contrast], axis=2)
    img_tensor = torch.from_numpy(img_4ch).permute(2, 0, 1).unsqueeze(0).float()

    return img_tensor, stretched


def run_segmentation(model: torch.nn.Module, img_tensor: torch.Tensor,
                     device: torch.device, original_size: Tuple[int, int]) -> np.ndarray:
    """
    Run segmentation model and resize output to original image size.

    Args:
        model: Loaded segmentation model
        img_tensor: Preprocessed tensor (1, 4, H, W)
        device: torch device
        original_size: (height, width) of original image

    Returns:
        Segmentation mask (H, W) matching original_size
    """
    img_tensor = img_tensor.to(device)

    with torch.no_grad():
        output = model(img_tensor)
        pred = output.argmax(dim=1).squeeze().cpu().numpy()

    # Resize mask to original size using nearest neighbor (preserve class labels)
    if pred.shape != original_size:
        mask_pil = Image.fromarray(pred.astype(np.uint8))
        mask_pil = mask_pil.resize((original_size[1], original_size[0]), Image.NEAREST)
        pred = np.array(mask_pil)

    return pred


# =============================================================================
# INTELLIGENT COMPOSITION
# =============================================================================

def create_soft_mask(hard_mask: np.ndarray, sigma: float = 3.0) -> np.ndarray:
    """
    Create soft (blurred) mask from hard binary mask for smooth blending.

    Args:
        hard_mask: Binary mask (0 or 1) of shape (H, W)
        sigma: Gaussian blur sigma

    Returns:
        Soft mask with values 0-1
    """
    if HAS_SCIPY and sigma > 0:
        return gaussian_filter(hard_mask.astype(np.float32), sigma=sigma)
    else:
        return hard_mask.astype(np.float32)


def intelligent_stretch(
    linear_image: np.ndarray,
    segmentation_mask: np.ndarray,
    blend_sigma: float = 3.0,
    verbose: bool = True
) -> Tuple[np.ndarray, Dict]:
    """
    Apply intelligent per-class stretching and blend results.

    This is the core of the intelligent stretching workflow:
    1. For each detected class, create a soft mask
    2. Apply the optimal stretch for that class to the LINEAR data
    3. Blend using soft masks with weight normalization

    Args:
        linear_image: Linear image (H, W) or (H, W, 3), values 0-1
        segmentation_mask: Class labels (H, W) with values 0-20
        blend_sigma: Sigma for Gaussian blur on masks
        verbose: Print progress

    Returns:
        composed: Intelligently stretched image
        metadata: Dictionary with processing details
    """
    # Ensure 3 channels
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

    # Find which classes are present
    unique_classes = np.unique(segmentation_mask)

    metadata = {
        'classes_detected': [],
        'blend_sigma': blend_sigma,
        'image_size': (H, W),
    }

    if verbose:
        print(f"\n  Processing {len(unique_classes)} detected classes:")

    for class_id in unique_classes:
        class_id = int(class_id)

        # Get class mask
        hard_mask = (segmentation_mask == class_id)
        pixel_count = np.sum(hard_mask)

        if pixel_count == 0:
            continue

        # Create soft mask
        soft_mask = create_soft_mask(hard_mask, sigma=blend_sigma)
        soft_mask = soft_mask[:, :, np.newaxis]

        # Get stretch function and parameters
        if class_id in INTELLIGENT_STRETCH_CONFIG:
            stretch_func, params, description = INTELLIGENT_STRETCH_CONFIG[class_id]
        else:
            stretch_func, params, description = stretch_mtf, {'midtone': 0.2}, "Default MTF"

        class_name = CLASS_NAMES[class_id] if class_id < len(CLASS_NAMES) else f"class_{class_id}"
        display_name = CLASS_DISPLAY_NAMES[class_id] if class_id < len(CLASS_DISPLAY_NAMES) else class_name

        if verbose:
            pct = 100.0 * pixel_count / (H * W)
            print(f"    [{class_id:2d}] {display_name:25s}: {pct:5.1f}% - {description}")

        # Apply stretch to the linear image
        stretched = np.zeros_like(image)
        for c in range(C):
            stretched[:, :, c] = stretch_func(image[:, :, c], **params)

        # Accumulate weighted contribution
        composed += stretched * soft_mask
        weight_sum += soft_mask

        metadata['classes_detected'].append({
            'class_id': class_id,
            'class_name': class_name,
            'display_name': display_name,
            'pixel_count': int(pixel_count),
            'percentage': float(100.0 * pixel_count / (H * W)),
            'stretch_description': description,
        })

    # Normalize by weight sum
    weight_sum = np.maximum(weight_sum, 1e-10)
    composed = composed / weight_sum
    composed = np.clip(composed, 0, 1)

    if not is_rgb:
        composed = composed[:, :, 0]

    return composed, metadata


def traditional_stretch(linear_image: np.ndarray, method: str = 'arcsinh') -> np.ndarray:
    """
    Apply a single traditional stretch to the entire image (for comparison).

    Args:
        linear_image: Linear image (H, W) or (H, W, 3), values 0-1
        method: 'arcsinh', 'mtf', or 'ghs'

    Returns:
        Stretched image
    """
    if method == 'arcsinh':
        return stretch_arcsinh(linear_image, scale=0.1)
    elif method == 'mtf':
        return stretch_mtf(linear_image, midtone=0.2)
    elif method == 'ghs':
        return stretch_ghs(linear_image, D=1.0, b=0.1)
    else:
        return stretch_arcsinh(linear_image, scale=0.1)


# =============================================================================
# I/O FUNCTIONS
# =============================================================================

def load_linear_image(filepath: str) -> Tuple[np.ndarray, dict]:
    """
    Load a linear (unstretched) image from FITS, TIFF, or other format.

    Args:
        filepath: Path to image file

    Returns:
        image: Float32 array normalized to 0-1
        header: Dictionary with metadata
    """
    path = Path(filepath)
    suffix = path.suffix.lower()
    header = {'filename': path.name}

    if suffix in ['.fits', '.fit', '.fts']:
        if not HAS_ASTROPY:
            raise ImportError("astropy required for FITS: pip install astropy")
        with fits.open(filepath) as hdul:
            data = hdul[0].data.astype(np.float32)
            # Get FITS header info
            hdr = hdul[0].header
            header['object'] = hdr.get('OBJECT', 'Unknown')
            header['exposure'] = hdr.get('EXPTIME', 0)
            header['filter'] = hdr.get('FILTER', 'Unknown')

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
            raise ImportError("tifffile or PIL required for TIFF")

    elif suffix in ['.png', '.jpg', '.jpeg']:
        if not HAS_PIL:
            raise ImportError("PIL required: pip install pillow")
        img = Image.open(filepath)
        data = np.array(img).astype(np.float32)

    else:
        raise ValueError(f"Unsupported format: {suffix}")

    # Normalize to 0-1
    if data.max() > 1.0:
        if data.max() > 255:
            data = data / 65535.0  # 16-bit
        else:
            data = data / 255.0  # 8-bit

    header['shape'] = data.shape
    header['dtype'] = str(data.dtype)

    return data, header


def save_image(image: np.ndarray, filepath: str, bit_depth: int = 16):
    """
    Save image to TIFF, PNG, or FITS.

    Args:
        image: Image array (H, W) or (H, W, 3), values 0-1
        filepath: Output path
        bit_depth: 8 or 16 for output
    """
    path = Path(filepath)
    path.parent.mkdir(parents=True, exist_ok=True)
    suffix = path.suffix.lower()

    if bit_depth == 16:
        output = (np.clip(image, 0, 1) * 65535).astype(np.uint16)
    else:
        output = (np.clip(image, 0, 1) * 255).astype(np.uint8)

    if suffix in ['.tif', '.tiff']:
        if HAS_TIFFFILE:
            tifffile.imwrite(filepath, output)
        elif HAS_PIL:
            Image.fromarray(output).save(filepath)
        else:
            raise ImportError("tifffile or PIL required")

    elif suffix in ['.png', '.jpg', '.jpeg']:
        output = (np.clip(image, 0, 1) * 255).astype(np.uint8)
        if output.ndim == 2:
            output = np.stack([output, output, output], axis=-1)
        Image.fromarray(output).save(filepath)

    elif suffix in ['.fits', '.fit']:
        if not HAS_ASTROPY:
            raise ImportError("astropy required for FITS")
        if output.ndim == 3:
            output = np.transpose(output, (2, 0, 1))
        hdu = fits.PrimaryHDU(output.astype(np.float32))
        hdu.writeto(filepath, overwrite=True)

    else:
        raise ValueError(f"Unsupported output format: {suffix}")


# =============================================================================
# VISUALIZATION
# =============================================================================

def create_comparison_figure(
    linear_img: np.ndarray,
    traditional_stretched: np.ndarray,
    intelligent_stretched: np.ndarray,
    segmentation_mask: np.ndarray,
    output_path: str,
    title: str = "Intelligent Stretch Comparison"
):
    """
    Create a side-by-side comparison figure.

    Shows:
    - Linear input (auto-stretched for visibility)
    - Traditional arcsinh stretch
    - Segmentation mask (colorized)
    - Intelligent stretch result
    """
    if not HAS_MATPLOTLIB:
        print("  Warning: matplotlib not available, skipping comparison figure")
        return

    fig, axes = plt.subplots(2, 2, figsize=(14, 14))

    # Ensure RGB for display
    def to_rgb(img):
        if img.ndim == 2:
            return np.stack([img, img, img], axis=-1)
        return img

    # Linear input (stretch for visibility)
    linear_display = stretch_arcsinh(linear_img, scale=0.2)
    axes[0, 0].imshow(to_rgb(linear_display))
    axes[0, 0].set_title('Linear Input (stretched for display)', fontsize=12)
    axes[0, 0].axis('off')

    # Traditional stretch
    axes[0, 1].imshow(to_rgb(traditional_stretched))
    axes[0, 1].set_title('Traditional Arcsinh Stretch', fontsize=12)
    axes[0, 1].axis('off')

    # Segmentation mask (colorized)
    seg_colored = apply_colormap(segmentation_mask)
    axes[1, 0].imshow(seg_colored)
    axes[1, 0].set_title('Segmentation Mask (21 classes)', fontsize=12)
    axes[1, 0].axis('off')

    # Intelligent stretch
    axes[1, 1].imshow(to_rgb(intelligent_stretched))
    axes[1, 1].set_title('Intelligent Stretch (per-class)', fontsize=12)
    axes[1, 1].axis('off')

    plt.suptitle(title, fontsize=14, fontweight='bold')
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()


def create_segment_visualization(
    linear_image: np.ndarray,
    segmentation_mask: np.ndarray,
    output_dir: str,
    verbose: bool = True
):
    """
    Create individual visualizations for each detected class segment.

    Saves images showing:
    - The segment mask
    - The linear data in that segment
    - The stretched data in that segment
    """
    if not HAS_MATPLOTLIB:
        print("  Warning: matplotlib not available, skipping segment visualization")
        return

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    unique_classes = np.unique(segmentation_mask)
    H, W = segmentation_mask.shape

    if verbose:
        print(f"\n  Saving segment visualizations to: {output_path}")

    for class_id in unique_classes:
        class_id = int(class_id)
        mask = (segmentation_mask == class_id)

        if np.sum(mask) == 0:
            continue

        class_name = CLASS_NAMES[class_id] if class_id < len(CLASS_NAMES) else f"class_{class_id}"
        display_name = CLASS_DISPLAY_NAMES[class_id] if class_id < len(CLASS_DISPLAY_NAMES) else class_name

        # Get stretch config
        if class_id in INTELLIGENT_STRETCH_CONFIG:
            stretch_func, params, description = INTELLIGENT_STRETCH_CONFIG[class_id]
        else:
            stretch_func, params, description = stretch_mtf, {'midtone': 0.2}, "Default"

        # Apply stretch
        if linear_image.ndim == 2:
            stretched = stretch_func(linear_image, **params)
        else:
            stretched = np.zeros_like(linear_image)
            for c in range(linear_image.shape[2]):
                stretched[:, :, c] = stretch_func(linear_image[:, :, c], **params)

        # Create figure
        fig, axes = plt.subplots(1, 3, figsize=(15, 5))

        # Mask
        mask_colored = np.zeros((H, W, 3), dtype=np.uint8)
        mask_colored[mask] = CLASS_COLORS_RGB[class_id]
        axes[0].imshow(mask_colored)
        axes[0].set_title(f'Mask: {display_name}')
        axes[0].axis('off')

        # Linear data (isolated)
        linear_isolated = np.zeros_like(linear_image)
        linear_isolated[mask] = linear_image[mask]
        linear_display = stretch_arcsinh(linear_isolated, scale=0.1)
        if linear_display.ndim == 2:
            linear_display = np.stack([linear_display]*3, axis=-1)
        axes[1].imshow(linear_display)
        axes[1].set_title('Linear Data (isolated)')
        axes[1].axis('off')

        # Stretched data (isolated)
        stretched_isolated = np.zeros_like(stretched)
        stretched_isolated[mask] = stretched[mask]
        if stretched_isolated.ndim == 2:
            stretched_isolated = np.stack([stretched_isolated]*3, axis=-1)
        axes[2].imshow(stretched_isolated)
        axes[2].set_title(f'Stretched: {description}')
        axes[2].axis('off')

        plt.suptitle(f'Class {class_id}: {display_name}', fontsize=14)
        plt.tight_layout()

        fig_path = output_path / f'segment_{class_id:02d}_{class_name}.png'
        plt.savefig(fig_path, dpi=100, bbox_inches='tight')
        plt.close()

        if verbose:
            print(f"    Saved: {fig_path.name}")


# =============================================================================
# MAIN DEMO
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='Demo: Intelligent Stretching Workflow for NukeX',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic usage
  python demo_intelligent_stretch.py --input linear.fits --output stretched.tiff

  # With comparison to traditional stretch
  python demo_intelligent_stretch.py --input linear.fits --output out.tiff --compare

  # With per-segment visualization
  python demo_intelligent_stretch.py --input linear.tiff --output out.tiff --visualize

  # Custom model
  python demo_intelligent_stretch.py --input img.fits --model custom.pth --output out.tiff
        """
    )

    parser.add_argument('--input', '-i', required=True,
                        help='Input linear (unstretched) image (FITS, TIFF, PNG)')
    parser.add_argument('--output', '-o', required=True,
                        help='Output intelligently stretched image')
    parser.add_argument('--model', '-m', default=DEFAULT_MODEL_PATH,
                        help=f'Segmentation model path (default: {DEFAULT_MODEL_PATH})')
    parser.add_argument('--blend-sigma', type=float, default=3.0,
                        help='Gaussian sigma for soft mask blending (default: 3.0)')
    parser.add_argument('--size', type=int, default=512,
                        help='Model input size (default: 512)')
    parser.add_argument('--bit-depth', type=int, choices=[8, 16], default=16,
                        help='Output bit depth (default: 16)')
    parser.add_argument('--compare', action='store_true',
                        help='Generate comparison with traditional stretch')
    parser.add_argument('--visualize', action='store_true',
                        help='Generate per-segment visualizations')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress progress output')

    args = parser.parse_args()
    verbose = not args.quiet

    # Print header
    if verbose:
        print("=" * 70)
        print("NukeX Intelligent Stretching Demo")
        print("=" * 70)
        print(f"Input:  {args.input}")
        print(f"Output: {args.output}")
        print(f"Model:  {args.model}")
        print()

    # Check dependencies
    if not HAS_PIL:
        print("ERROR: PIL (Pillow) is required. Install with: pip install pillow")
        return 1

    # Setup device
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    if verbose:
        print(f"Device: {device}")
        if device.type == 'cuda':
            print(f"  GPU: {torch.cuda.get_device_name()}")

    # Step 1: Load linear image
    if verbose:
        print("\n[Step 1/5] Loading linear image...")
    try:
        linear_image, header = load_linear_image(args.input)
        if verbose:
            print(f"  Shape: {linear_image.shape}")
            print(f"  Range: [{linear_image.min():.4f}, {linear_image.max():.4f}]")
            if 'object' in header:
                print(f"  Object: {header.get('object', 'Unknown')}")
    except Exception as e:
        print(f"ERROR loading image: {e}")
        return 1

    # Step 2: Load segmentation model
    if verbose:
        print("\n[Step 2/5] Loading segmentation model...")
    try:
        model = load_segmentation_model(args.model, device)
        if verbose:
            print("  Model loaded successfully")
    except Exception as e:
        print(f"ERROR loading model: {e}")
        return 1

    # Step 3: Apply temporary stretch and run segmentation
    if verbose:
        print("\n[Step 3/5] Running segmentation on temporarily stretched image...")

    original_size = linear_image.shape[:2]
    img_tensor, stretched_preview = preprocess_for_segmentation(linear_image, args.size)
    segmentation_mask = run_segmentation(model, img_tensor, device, original_size)

    if verbose:
        unique_classes = np.unique(segmentation_mask)
        print(f"  Detected {len(unique_classes)} classes: {unique_classes.tolist()}")

    # Step 4: Apply intelligent stretch to LINEAR data
    if verbose:
        print("\n[Step 4/5] Applying intelligent per-class stretch to linear data...")

    intelligent_result, metadata = intelligent_stretch(
        linear_image, segmentation_mask,
        blend_sigma=args.blend_sigma,
        verbose=verbose
    )

    # Step 5: Save output
    if verbose:
        print(f"\n[Step 5/5] Saving output to: {args.output}")

    save_image(intelligent_result, args.output, args.bit_depth)

    if verbose:
        print(f"  Saved {args.bit_depth}-bit image")

    # Optional: Generate comparison
    if args.compare:
        if verbose:
            print("\n[Bonus] Generating comparison with traditional stretch...")

        traditional_result = traditional_stretch(linear_image, method='arcsinh')

        # Save traditional stretch
        output_path = Path(args.output)
        traditional_path = output_path.with_stem(output_path.stem + '_traditional')
        save_image(traditional_result, str(traditional_path), args.bit_depth)
        if verbose:
            print(f"  Saved traditional stretch: {traditional_path}")

        # Save comparison figure
        comparison_path = output_path.with_stem(output_path.stem + '_comparison').with_suffix('.png')
        create_comparison_figure(
            linear_image, traditional_result, intelligent_result,
            segmentation_mask, str(comparison_path),
            title=f"Intelligent Stretch: {Path(args.input).name}"
        )
        if verbose:
            print(f"  Saved comparison figure: {comparison_path}")

        # Save segmentation mask
        mask_path = output_path.with_stem(output_path.stem + '_segmentation').with_suffix('.png')
        seg_colored = apply_colormap(segmentation_mask)
        Image.fromarray(seg_colored).save(str(mask_path))
        if verbose:
            print(f"  Saved segmentation mask: {mask_path}")

    # Optional: Generate per-segment visualization
    if args.visualize:
        if verbose:
            print("\n[Bonus] Generating per-segment visualizations...")

        output_path = Path(args.output)
        vis_dir = output_path.parent / (output_path.stem + '_segments')
        create_segment_visualization(linear_image, segmentation_mask, str(vis_dir), verbose)

    # Summary
    if verbose:
        print("\n" + "=" * 70)
        print("PROCESSING COMPLETE")
        print("=" * 70)
        print(f"Input:  {args.input}")
        print(f"Output: {args.output}")
        print(f"Classes detected: {len(metadata['classes_detected'])}")
        print()
        print("Detected class summary:")
        for cls in sorted(metadata['classes_detected'], key=lambda x: -x['percentage']):
            if cls['percentage'] > 0.1:
                print(f"  {cls['display_name']:25s}: {cls['percentage']:5.1f}%")
        print()
        print("This demonstrates the core value of NukeX:")
        print("  - Stars compressed with arcsinh (preserve cores, show halos)")
        print("  - Nebulae enhanced with GHS (strong detail)")
        print("  - Dark features preserved (minimal stretch)")
        print("  - Background smoothed with MTF (clean appearance)")
        print()

    return 0


if __name__ == '__main__':
    sys.exit(main())
