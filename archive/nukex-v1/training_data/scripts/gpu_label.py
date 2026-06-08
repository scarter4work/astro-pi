#!/usr/bin/env python3
"""
GPU-Accelerated Auto-Labeling for New Training Data

Uses the existing trained model to generate segmentation masks for new HST/ESO data.
Leverages RTX 5070 Ti for fast batch inference.

Usage:
    python gpu_label.py --input ../hst --output ../labeled --batch-size 8
    python gpu_label.py --input ../eso --output ../labeled --model /path/to/model.pth
"""

import argparse
import os
import sys
import numpy as np
import torch
import torch.nn.functional as F
from pathlib import Path
from PIL import Image
from tqdm import tqdm
from astropy.io import fits
import warnings

# Suppress warnings
warnings.filterwarnings('ignore')
Image.MAX_IMAGE_PIXELS = None

# Add NukeX2 training scripts to path for model import
NUKEX2_SCRIPTS = Path("/home/scarter4work/projects/NukeX2/training/scripts")
sys.path.insert(0, str(NUKEX2_SCRIPTS))

from model import AstroUNet

# GPU optimizations
torch.backends.cudnn.benchmark = True
torch.backends.cuda.matmul.allow_tf32 = True
torch.backends.cudnn.allow_tf32 = True

# 21 classes matching train.py
REGION_NAMES = [
    'background', 'star_bright', 'star_medium', 'star_faint', 'star_saturated',
    'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
    'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
    'dust_lane', 'star_cluster_open', 'star_cluster_globular',
    'artifact_hot_pixel', 'artifact_satellite', 'artifact_diffraction',
    'artifact_gradient', 'artifact_noise'
]
NUM_CLASSES = 21

# Colors for visualization masks
REGION_COLORS = [
    (0, 0, 0),        # background
    (255, 255, 255),  # star_bright
    (200, 200, 255),  # star_medium
    (150, 150, 200),  # star_faint
    (255, 255, 200),  # star_saturated
    (255, 100, 100),  # nebula_emission
    (100, 150, 255),  # nebula_reflection
    (50, 30, 30),     # nebula_dark
    (255, 100, 255),  # nebula_planetary
    (100, 200, 255),  # galaxy_spiral
    (200, 150, 100),  # galaxy_elliptical
    (150, 100, 200),  # galaxy_irregular
    (255, 200, 100),  # galaxy_core
    (80, 60, 40),     # dust_lane
    (255, 255, 100),  # star_cluster_open
    (200, 200, 100),  # star_cluster_globular
    (255, 0, 0),      # artifact_hot_pixel
    (0, 255, 255),    # artifact_satellite
    (255, 128, 0),    # artifact_diffraction
    (128, 0, 128),    # artifact_gradient
    (64, 64, 64),     # artifact_noise
]


def load_model(model_path, device='cuda'):
    """Load trained segmentation model."""
    print(f"Loading model from {model_path}...")

    # Detect input channels from checkpoint
    checkpoint = torch.load(model_path, map_location='cpu', weights_only=False)

    if 'model_state_dict' in checkpoint:
        state_dict = checkpoint['model_state_dict']
    else:
        state_dict = checkpoint

    # Find first conv layer (handles different naming conventions)
    first_conv_key = None
    for k in state_dict.keys():
        if 'weight' in k and state_dict[k].dim() == 4:
            first_conv_key = k
            break

    if first_conv_key is None:
        raise ValueError("Could not find first conv layer")

    in_channels = state_dict[first_conv_key].shape[1]
    print(f"Model expects {in_channels} input channels")

    # Find output layer (outc or final)
    out_key = None
    for k in state_dict.keys():
        if ('outc' in k or 'final' in k) and 'weight' in k:
            out_key = k

    if out_key is None:
        raise ValueError("Could not find output layer")

    num_classes = state_dict[out_key].shape[0]
    print(f"Model outputs {num_classes} classes")

    model = AstroUNet(in_channels=in_channels, num_classes=num_classes)
    model.load_state_dict(state_dict)
    model = model.to(device)
    model.eval()

    return model, in_channels, num_classes


def arcsinh_stretch(data, scale=0.1):
    """
    Apply arcsinh stretch - standard for astronomical imaging.
    Preserves both faint nebulosity and bright star cores.

    Similar to PixInsight's STF or Photoshop's arcsinh stretch.
    scale parameter controls the stretch intensity (lower = more aggressive).
    """
    # Normalize to 0-1 first based on data range
    data = data.astype(np.float64)
    vmin = np.nanpercentile(data, 0.5)
    vmax = np.nanpercentile(data, 99.9)

    if vmax <= vmin:
        return np.zeros_like(data, dtype=np.float32)

    # Normalize
    normalized = (data - vmin) / (vmax - vmin)
    normalized = np.clip(normalized, 0, 1)

    # Apply arcsinh stretch
    # Formula: arcsinh(x / scale) / arcsinh(1 / scale)
    stretched = np.arcsinh(normalized / scale) / np.arcsinh(1.0 / scale)

    return stretched.astype(np.float32)


def midtone_transfer_function(data, midtone=0.25):
    """
    Apply Midtone Transfer Function (MTF) stretch.
    Common in PixInsight for initial stretches.

    midtone: target midtone value (0-1), lower = more aggressive stretch
    """
    data = data.astype(np.float64)

    # Normalize first
    vmin = np.nanpercentile(data, 0.5)
    vmax = np.nanpercentile(data, 99.9)

    if vmax <= vmin:
        return np.zeros_like(data, dtype=np.float32)

    normalized = (data - vmin) / (vmax - vmin)
    normalized = np.clip(normalized, 0, 1)

    # MTF formula: (m - 1) * x / ((2m - 1) * x - m)
    # where m = midtone, x = input
    m = midtone

    # Avoid division by zero
    denom = (2 * m - 1) * normalized - m
    denom = np.where(np.abs(denom) < 1e-10, 1e-10, denom)

    stretched = (m - 1) * normalized / denom
    stretched = np.clip(stretched, 0, 1)

    return stretched.astype(np.float32)


def load_fits_as_rgb(fits_path, target_size=512, stretch='arcsinh'):
    """
    Load FITS file, apply astronomical stretch, convert to RGB.

    stretch: 'arcsinh', 'mtf', 'linear', or 'none'
    """
    try:
        with fits.open(fits_path) as hdul:
            # Find the image data
            data = None
            for hdu in hdul:
                if hdu.data is not None and len(hdu.data.shape) >= 2:
                    data = hdu.data
                    break

            if data is None:
                return None

            # Handle different data shapes
            if len(data.shape) == 2:
                # Grayscale - replicate to RGB
                img = np.stack([data, data, data], axis=-1)
            elif len(data.shape) == 3:
                if data.shape[0] in [1, 3, 4]:
                    # Channel-first format
                    if data.shape[0] == 1:
                        img = np.stack([data[0], data[0], data[0]], axis=-1)
                    elif data.shape[0] == 3:
                        img = np.transpose(data, (1, 2, 0))
                    else:  # 4 channels - take first 3
                        img = np.transpose(data[:3], (1, 2, 0))
                else:
                    # Channel-last format
                    if data.shape[-1] == 1:
                        img = np.concatenate([data, data, data], axis=-1)
                    elif data.shape[-1] >= 3:
                        img = data[..., :3]
                    else:
                        return None
            else:
                return None

            # Handle NaN/Inf before stretching
            img = np.nan_to_num(img.astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0)

            # Apply astronomical stretch to each channel
            if stretch == 'arcsinh':
                for c in range(img.shape[-1]):
                    img[:,:,c] = arcsinh_stretch(img[:,:,c], scale=0.1)
            elif stretch == 'mtf':
                for c in range(img.shape[-1]):
                    img[:,:,c] = midtone_transfer_function(img[:,:,c], midtone=0.2)
            elif stretch == 'linear':
                # Simple linear stretch (percentile normalization)
                vmin, vmax = np.percentile(img[np.isfinite(img)], [1, 99])
                if vmax > vmin:
                    img = np.clip((img - vmin) / (vmax - vmin), 0, 1)
                else:
                    img = np.zeros_like(img)
            # 'none' = no stretch applied

            # Ensure 0-1 range
            img = np.clip(img, 0, 1)

            # Resize
            pil_img = Image.fromarray((img * 255).astype(np.uint8))
            pil_img = pil_img.resize((target_size, target_size), Image.BILINEAR)

            return np.array(pil_img).astype(np.float32) / 255.0

    except Exception as e:
        print(f"Error loading {fits_path}: {e}")
        return None


def load_image_as_rgb(image_path, target_size=512):
    """Load standard image file (JPG/PNG) and convert to RGB numpy array."""
    try:
        img = Image.open(image_path)

        # Convert to RGB if needed
        if img.mode != 'RGB':
            img = img.convert('RGB')

        # Resize
        img = img.resize((target_size, target_size), Image.BILINEAR)

        # Convert to numpy and normalize to 0-1
        img_array = np.array(img).astype(np.float32) / 255.0

        return img_array

    except Exception as e:
        print(f"Error loading {image_path}: {e}")
        return None


def preprocess_for_model(img_array, num_channels=3):
    """Convert RGB array to model input tensor."""
    r, g, b = img_array[:,:,0], img_array[:,:,1], img_array[:,:,2]

    if num_channels == 3:
        # Simple RGB
        tensor = np.stack([r, g, b], axis=0)
    elif num_channels == 11:
        # 11-channel multi-spectral
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        ha = np.clip(r * 1.2 - g * 0.1 - b * 0.1, 0, 1)
        oiii = np.clip(-r * 0.1 + g * 0.5 + b * 0.6, 0, 1)
        sii = np.clip(r * 0.8 - g * 0.2 + b * 0.0, 0, 1)
        r_g = (r - g + 1) / 2
        g_b = (g - b + 1) / 2
        b_r = (b - r + 1) / 2
        max_rgb = np.maximum(np.maximum(r, g), b)
        min_rgb = np.minimum(np.minimum(r, g), b)
        sat = np.where(max_rgb > 0, (max_rgb - min_rgb) / (max_rgb + 1e-6), 0)
        tensor = np.stack([r, g, b, lum, ha, oiii, sii, r_g, g_b, b_r, sat], axis=0)
    else:
        raise ValueError(f"Unsupported channel count: {num_channels}")

    return torch.from_numpy(tensor).float()


def create_color_mask(prediction, num_classes):
    """Convert class prediction to colored visualization."""
    h, w = prediction.shape
    color_mask = np.zeros((h, w, 3), dtype=np.uint8)

    for class_idx in range(min(num_classes, len(REGION_COLORS))):
        mask = prediction == class_idx
        color_mask[mask] = REGION_COLORS[class_idx]

    return color_mask


def process_batch(model, batch_tensors, device):
    """Run inference on a batch of images."""
    batch = torch.stack(batch_tensors).to(device)

    with torch.no_grad():
        with torch.cuda.amp.autocast():  # FP16 for speed
            outputs = model(batch)
            predictions = torch.argmax(outputs, dim=1)

    return predictions.cpu().numpy()


def main():
    parser = argparse.ArgumentParser(description='GPU-accelerated auto-labeling')
    parser.add_argument('--input', required=True, help='Input directory with FITS/images')
    parser.add_argument('--output', required=True, help='Output directory for masks')
    parser.add_argument('--model', default='/home/scarter4work/projects/NukeX2/training/models/best_model.pth',
                        help='Path to trained model')
    parser.add_argument('--batch-size', type=int, default=8, help='Batch size for inference')
    parser.add_argument('--size', type=int, default=512, help='Image size for inference')
    parser.add_argument('--save-viz', action='store_true', help='Save colored visualization masks')
    parser.add_argument('--stretch', choices=['arcsinh', 'mtf', 'linear', 'none'],
                        default='arcsinh', help='Stretch to apply to FITS data (default: arcsinh)')
    args = parser.parse_args()

    # Setup
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    if device.type == 'cuda':
        print(f"GPU: {torch.cuda.get_device_name()}")
        print(f"VRAM: {torch.cuda.get_device_properties(0).total_memory / 1e9:.1f} GB")

    # Load model
    model, in_channels, num_classes = load_model(args.model, device)

    # Find all image files (FITS and standard formats)
    input_path = Path(args.input)
    fits_files = list(input_path.rglob("*.fits")) + list(input_path.rglob("*.fits.gz"))
    jpg_files = list(input_path.rglob("*.jpg")) + list(input_path.rglob("*.jpeg"))
    png_files = list(input_path.rglob("*.png"))

    all_files = fits_files + jpg_files + png_files
    print(f"Found {len(fits_files)} FITS, {len(jpg_files)} JPG, {len(png_files)} PNG files")
    print(f"Total: {len(all_files)} files to process")

    if len(all_files) == 0:
        print("No image files found!")
        return

    # Create output directory
    output_path = Path(args.output)
    output_path.mkdir(parents=True, exist_ok=True)

    # Process in batches
    batch_tensors = []
    batch_files = []
    total_processed = 0

    def load_any_image(filepath, size, stretch='arcsinh'):
        """Load image based on file extension."""
        ext = filepath.suffix.lower()
        if ext in ['.fits', '.gz']:
            return load_fits_as_rgb(filepath, size, stretch=stretch)
        elif ext in ['.jpg', '.jpeg', '.png']:
            # JPG/PNG are already stretched display images
            return load_image_as_rgb(filepath, size)
        return None

    print(f"Using stretch: {args.stretch}")

    with tqdm(total=len(all_files), desc="Processing") as pbar:
        for image_file in all_files:
            # Load and preprocess
            img = load_any_image(image_file, args.size, stretch=args.stretch)
            if img is None:
                pbar.update(1)
                continue

            tensor = preprocess_for_model(img, in_channels)
            batch_tensors.append(tensor)
            batch_files.append((image_file, img))

            # Process batch when full
            if len(batch_tensors) >= args.batch_size:
                predictions = process_batch(model, batch_tensors, device)

                for i, (fpath, orig_img) in enumerate(batch_files):
                    pred = predictions[i]

                    # Determine output path preserving category structure
                    rel_path = fpath.relative_to(input_path)
                    mask_path = output_path / rel_path.parent / f"{fpath.stem}_mask.png"
                    mask_path.parent.mkdir(parents=True, exist_ok=True)

                    # Save class mask (grayscale, class indices)
                    mask_img = Image.fromarray(pred.astype(np.uint8))
                    mask_img.save(mask_path)

                    # Optionally save colored visualization
                    if args.save_viz:
                        viz_path = output_path / rel_path.parent / f"{fpath.stem}_viz.png"
                        color_mask = create_color_mask(pred, num_classes)
                        Image.fromarray(color_mask).save(viz_path)

                    # Also save the preprocessed image
                    img_path = output_path / rel_path.parent / f"{fpath.stem}_img.png"
                    Image.fromarray((orig_img * 255).astype(np.uint8)).save(img_path)

                    total_processed += 1

                batch_tensors = []
                batch_files = []
                pbar.update(args.batch_size)

        # Process remaining
        if batch_tensors:
            predictions = process_batch(model, batch_tensors, device)

            for i, (fpath, orig_img) in enumerate(batch_files):
                pred = predictions[i]

                rel_path = fpath.relative_to(input_path)
                mask_path = output_path / rel_path.parent / f"{fpath.stem}_mask.png"
                mask_path.parent.mkdir(parents=True, exist_ok=True)

                mask_img = Image.fromarray(pred.astype(np.uint8))
                mask_img.save(mask_path)

                if args.save_viz:
                    viz_path = output_path / rel_path.parent / f"{fpath.stem}_viz.png"
                    color_mask = create_color_mask(pred, num_classes)
                    Image.fromarray(color_mask).save(viz_path)

                img_path = output_path / rel_path.parent / f"{fpath.stem}_img.png"
                Image.fromarray((orig_img * 255).astype(np.uint8)).save(img_path)

                total_processed += 1

            pbar.update(len(batch_tensors))

    print(f"\nDone! Processed {total_processed} images")
    print(f"Masks saved to: {output_path}")

    # Summary by category
    print("\nBy category:")
    for cat_dir in sorted(output_path.iterdir()):
        if cat_dir.is_dir():
            masks = list(cat_dir.rglob("*_mask.png"))
            print(f"  {cat_dir.name}: {len(masks)} masks")


if __name__ == "__main__":
    main()
