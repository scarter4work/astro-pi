#!/usr/bin/env python3
"""
Test v8 segmentation model on FITS files.
"""

import torch
import torch.nn.functional as F
import numpy as np
from PIL import Image
from pathlib import Path
import sys

# Add parent to path for model import
sys.path.insert(0, str(Path(__file__).parent))
from model import AstroUNet

try:
    from astropy.io import fits
    HAS_ASTROPY = True
except ImportError:
    HAS_ASTROPY = False
    print("Warning: astropy not available, FITS support disabled")

# Class names and colors
CLASS_NAMES = [
    'background', 'star_bright', 'star_medium', 'star_faint', 'star_cluster_globular',
    'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
    'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
    'dust_lane', 'star_cluster_open', 'artifact_hot_pixel', 'artifact_cosmic_ray',
    'artifact_satellite', 'artifact_diffraction', 'artifact_gradient', 'transition'
]

# Color palette for visualization (RGB)
CLASS_COLORS = [
    (0, 0, 0),        # background - black
    (255, 255, 0),    # star_bright - yellow
    (255, 200, 0),    # star_medium - gold
    (255, 150, 0),    # star_faint - orange
    (0, 255, 255),    # star_cluster_globular - cyan
    (255, 0, 0),      # nebula_emission - red
    (0, 0, 255),      # nebula_reflection - blue
    (64, 64, 64),     # nebula_dark - dark gray
    (0, 255, 0),      # nebula_planetary - green
    (255, 0, 255),    # galaxy_spiral - magenta
    (200, 0, 200),    # galaxy_elliptical - purple
    (150, 0, 150),    # galaxy_irregular - dark purple
    (255, 128, 0),    # galaxy_core - orange
    (100, 50, 0),     # dust_lane - brown
    (128, 255, 255),  # star_cluster_open - light cyan
    (255, 0, 128),    # artifact_hot_pixel - pink
    (128, 0, 255),    # artifact_cosmic_ray - violet
    (255, 128, 128),  # artifact_satellite - light red
    (128, 128, 255),  # artifact_diffraction - light blue
    (50, 50, 50),     # artifact_gradient - very dark gray
    (128, 128, 128),  # transition - gray
]


def arcsinh_stretch(data: np.ndarray, scale: float = 5.0) -> np.ndarray:
    """Apply arcsinh stretch to linear data."""
    # Normalize to [0, 1] first
    data_min = np.percentile(data, 1)
    data_max = np.percentile(data, 99.5)

    if data_max > data_min:
        normalized = np.clip((data - data_min) / (data_max - data_min), 0, 1)
    else:
        normalized = np.zeros_like(data)

    # Apply arcsinh stretch
    stretched = np.arcsinh(normalized * scale) / np.arcsinh(scale)

    return np.clip(stretched, 0, 1)


def load_fits_image(fits_path: Path) -> np.ndarray:
    """Load and stretch a FITS file."""
    if not HAS_ASTROPY:
        raise ImportError("astropy required for FITS files")

    with fits.open(fits_path) as hdul:
        data = hdul[0].data.astype(np.float32)

    # Handle different FITS layouts
    if data.ndim == 3:
        # Could be (channels, H, W) or (H, W, channels)
        if data.shape[0] in [1, 3, 4]:
            data = np.transpose(data, (1, 2, 0))

    # If single channel, replicate to RGB (mono image)
    if data.ndim == 2:
        data = np.stack([data, data, data], axis=-1)
    elif data.shape[-1] == 1:
        data = np.repeat(data, 3, axis=-1)

    # Apply arcsinh stretch to each channel
    stretched = np.zeros_like(data)
    for c in range(data.shape[-1]):
        stretched[:, :, c] = arcsinh_stretch(data[:, :, c])

    return (stretched * 255).astype(np.uint8)


def preprocess_image(img_array: np.ndarray, target_size: int = 512) -> torch.Tensor:
    """Preprocess image array for model input."""
    # Resize
    img = Image.fromarray(img_array)
    img = img.resize((target_size, target_size), Image.LANCZOS)
    img_np = np.array(img).astype(np.float32) / 255.0

    # Normalize using percentiles
    for c in range(3):
        channel = img_np[:, :, c]
        p1, p99 = np.percentile(channel, [1, 99])
        if p99 > p1:
            img_np[:, :, c] = np.clip((channel - p1) / (p99 - p1), 0, 1)

    # Create 4-channel input (RGB + color contrast)
    r, g, b = img_np[:, :, 0], img_np[:, :, 1], img_np[:, :, 2]
    color_contrast = b - r
    color_contrast = (color_contrast + 1) / 2  # Normalize to [0, 1]

    # Stack to 4 channels
    input_4ch = np.stack([r, g, b, color_contrast], axis=0)
    tensor = torch.from_numpy(input_4ch).unsqueeze(0)

    return tensor


def create_segmentation_overlay(original: np.ndarray, mask: np.ndarray, alpha: float = 0.5) -> np.ndarray:
    """Create visualization with segmentation overlay."""
    h, w = mask.shape
    orig_resized = np.array(Image.fromarray(original).resize((w, h), Image.LANCZOS))

    colored_mask = np.zeros((h, w, 3), dtype=np.uint8)
    for class_idx, color in enumerate(CLASS_COLORS):
        colored_mask[mask == class_idx] = color

    overlay = (orig_resized * (1 - alpha) + colored_mask * alpha).astype(np.uint8)
    return overlay


def run_inference(model_path: Path, image_path: Path, output_dir: Path):
    """Run inference on a FITS or image file."""
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # Load model
    print(f"Loading model: {model_path}")
    model = AstroUNet(in_channels=4, num_classes=21)
    checkpoint = torch.load(model_path, map_location=device, weights_only=False)
    model.load_state_dict(checkpoint['model_state_dict'])
    model = model.to(device)
    model.eval()

    # Load image
    print(f"Processing: {image_path}")
    suffix = image_path.suffix.lower()

    if suffix in ['.fits', '.fit', '.fts']:
        print("  Loading FITS file with arcsinh stretch...")
        img_array = load_fits_image(image_path)
    else:
        img = Image.open(image_path)
        if img.mode != 'RGB':
            img = img.convert('RGB')
        img_array = np.array(img)

    original = img_array.copy()

    # Preprocess
    input_tensor = preprocess_image(img_array)
    input_tensor = input_tensor.to(device)

    # Run inference
    with torch.no_grad():
        output = model(input_tensor)
        probs = F.softmax(output, dim=1)
        pred_mask = torch.argmax(probs, dim=1).squeeze().cpu().numpy()
        confidence = probs.max(dim=1)[0].squeeze().cpu().numpy()

    # Analyze results
    unique_classes, counts = np.unique(pred_mask, return_counts=True)
    total_pixels = pred_mask.size

    print("\n" + "="*60)
    print("SEGMENTATION RESULTS")
    print("="*60)
    print(f"\nDetected classes:")
    for cls_idx, count in sorted(zip(unique_classes, counts), key=lambda x: -x[1]):
        pct = count / total_pixels * 100
        if pct > 0.1:
            print(f"  {CLASS_NAMES[cls_idx]:25s}: {pct:5.1f}% ({count:,} pixels)")

    print(f"\nMean confidence: {confidence.mean():.3f}")
    print(f"Min confidence:  {confidence.min():.3f}")
    print(f"Max confidence:  {confidence.max():.3f}")

    # Save outputs
    output_dir.mkdir(parents=True, exist_ok=True)
    stem = image_path.stem

    # Save stretched original (for FITS)
    if suffix in ['.fits', '.fit', '.fts']:
        orig_path = output_dir / f"{stem}_stretched.png"
        Image.fromarray(original).save(orig_path)
        print(f"\nSaved stretched image: {orig_path}")

    # Save segmentation mask
    mask_colored = np.zeros((pred_mask.shape[0], pred_mask.shape[1], 3), dtype=np.uint8)
    for cls_idx, color in enumerate(CLASS_COLORS):
        mask_colored[pred_mask == cls_idx] = color

    mask_path = output_dir / f"{stem}_segmentation.png"
    Image.fromarray(mask_colored).save(mask_path)
    print(f"Saved segmentation mask: {mask_path}")

    # Save overlay
    overlay = create_segmentation_overlay(original, pred_mask, alpha=0.4)
    overlay_path = output_dir / f"{stem}_overlay.png"
    Image.fromarray(overlay).save(overlay_path)
    print(f"Saved overlay: {overlay_path}")

    # Save confidence map
    conf_uint8 = (confidence * 255).astype(np.uint8)
    conf_path = output_dir / f"{stem}_confidence.png"
    Image.fromarray(conf_uint8).save(conf_path)
    print(f"Saved confidence map: {conf_path}")

    return pred_mask, confidence


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Test segmentation model on FITS/images")
    parser.add_argument("--model", type=str, required=True, help="Path to model checkpoint")
    parser.add_argument("--image", type=str, required=True, help="Path to input image/FITS")
    parser.add_argument("--output", type=str, default="./test_output", help="Output directory")

    args = parser.parse_args()

    run_inference(
        model_path=Path(args.model),
        image_path=Path(args.image),
        output_dir=Path(args.output)
    )
