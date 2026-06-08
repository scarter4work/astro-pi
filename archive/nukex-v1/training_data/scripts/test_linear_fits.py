#!/usr/bin/env python3
"""
Test the trained 21-class segmentation model on linear FITS images.
Applies arcsinh stretch before segmentation (matching training workflow).
"""

import argparse
import numpy as np
import torch
from PIL import Image
from pathlib import Path
from astropy.io import fits
import sys

# Add model path
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

# Color map for visualization (RGB)
CLASS_COLORS = [
    (0, 0, 0),        # background - black
    (255, 255, 0),    # star_bright - yellow
    (255, 200, 0),    # star_medium - orange-yellow
    (255, 150, 0),    # star_faint - orange
    (255, 0, 255),    # star_saturated - magenta
    (255, 0, 0),      # nebula_emission - red
    (0, 0, 255),      # nebula_reflection - blue
    (64, 64, 64),     # nebula_dark - dark gray
    (0, 255, 255),    # nebula_planetary - cyan
    (0, 255, 0),      # galaxy_spiral - green
    (0, 200, 0),      # galaxy_elliptical - darker green
    (0, 150, 150),    # galaxy_irregular - teal
    (255, 128, 0),    # galaxy_core - orange
    (128, 64, 0),     # dust_lane - brown
    (200, 200, 255),  # star_cluster_open - light blue
    (255, 200, 200),  # star_cluster_globular - light pink
    (255, 0, 0),      # artifact_hot_pixel - red
    (255, 255, 255),  # artifact_satellite - white
    (128, 128, 255),  # artifact_diffraction - light purple
    (100, 100, 100),  # artifact_gradient - gray
    (50, 50, 50),     # artifact_noise - dark gray
]


def arcsinh_stretch(data, scale=0.1):
    """Apply arcsinh stretch to linear data."""
    # Normalize to 0-1 range first
    data = data.astype(np.float32)
    vmin, vmax = np.percentile(data[data > 0], [1, 99.5]) if np.any(data > 0) else (0, 1)
    data = np.clip((data - vmin) / (vmax - vmin + 1e-10), 0, 1)

    # Apply arcsinh stretch
    stretched = np.arcsinh(data / scale) / np.arcsinh(1.0 / scale)
    return np.clip(stretched, 0, 1)


def load_fits_image(fits_path):
    """Load FITS file and convert to stretched RGB."""
    with fits.open(fits_path) as hdul:
        data = hdul[0].data

        if data is None:
            # Try extension 1
            data = hdul[1].data

        print(f"  FITS shape: {data.shape}, dtype: {data.dtype}")

        # Handle different array shapes
        if len(data.shape) == 3:
            # Color image (could be RGB or channel-first)
            if data.shape[0] == 3:
                # Channel first: (3, H, W) -> (H, W, 3)
                data = np.transpose(data, (1, 2, 0))
            # Apply stretch to each channel
            stretched = np.zeros_like(data, dtype=np.float32)
            for i in range(3):
                stretched[:, :, i] = arcsinh_stretch(data[:, :, i])
        else:
            # Grayscale - stretch and convert to RGB
            stretched = arcsinh_stretch(data)
            stretched = np.stack([stretched, stretched, stretched], axis=-1)

        return stretched


def load_model(model_path, device):
    """Load trained model."""
    model = AstroUNet(in_channels=3, num_classes=NUM_CLASSES, base_features=32)
    checkpoint = torch.load(model_path, map_location=device)
    model.load_state_dict(checkpoint['model_state_dict'])
    model = model.to(device)
    model.eval()
    return model


def create_visualization(mask, stretched_img=None):
    """Create colored visualization of segmentation mask."""
    h, w = mask.shape
    vis = np.zeros((h, w, 3), dtype=np.uint8)

    for class_idx, color in enumerate(CLASS_COLORS):
        vis[mask == class_idx] = color

    if stretched_img is not None:
        # Blend with stretched image
        img_uint8 = (stretched_img * 255).astype(np.uint8)
        if img_uint8.shape[:2] != (h, w):
            img_uint8 = np.array(Image.fromarray(img_uint8).resize((w, h)))
        vis = (0.5 * vis + 0.5 * img_uint8).astype(np.uint8)

    return vis


def print_class_stats(mask):
    """Print statistics about detected classes."""
    unique, counts = np.unique(mask, return_counts=True)
    total = mask.size

    print("\nDetected classes:")
    print("-" * 50)
    for class_idx, count in sorted(zip(unique, counts), key=lambda x: -x[1]):
        pct = 100 * count / total
        if pct > 0.1:  # Only show classes with >0.1%
            print(f"  {CLASS_NAMES[class_idx]:25s}: {pct:6.2f}% ({count:,} pixels)")


def main():
    parser = argparse.ArgumentParser(description='Test segmentation on linear FITS')
    parser.add_argument('fits_file', help='Input FITS file path')
    parser.add_argument('--model', default='/home/scarter4work/projects/NukeX/training_data/models/best_model.pth',
                        help='Model path')
    parser.add_argument('--output', help='Output directory (default: current dir)')
    parser.add_argument('--size', type=int, default=512, help='Processing size')
    parser.add_argument('--no-blend', action='store_true', help='Don\'t blend with original')
    args = parser.parse_args()

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Device: {device}")

    # Load model
    print(f"Loading model: {args.model}")
    model = load_model(args.model, device)
    print("Model loaded")

    # Load FITS file
    print(f"\nLoading FITS: {args.fits_file}")
    stretched = load_fits_image(args.fits_file)
    print(f"  Stretched shape: {stretched.shape}")

    original_h, original_w = stretched.shape[:2]

    # Resize for model input
    img_pil = Image.fromarray((stretched * 255).astype(np.uint8))
    img_resized = img_pil.resize((args.size, args.size), Image.BILINEAR)
    img_np = np.array(img_resized).astype(np.float32) / 255.0
    img_tensor = torch.from_numpy(img_np).permute(2, 0, 1).unsqueeze(0).to(device)

    # Run inference
    print("Running segmentation...")
    with torch.no_grad():
        output = model(img_tensor)
        pred = output.argmax(dim=1).squeeze().cpu().numpy()

    # Print class statistics
    print_class_stats(pred)

    # Create visualizations
    blend_img = stretched if not args.no_blend else None
    vis = create_visualization(pred,
                               np.array(img_resized).astype(np.float32) / 255.0 if blend_img is not None else None)

    # Determine output paths
    fits_path = Path(args.fits_file)
    output_dir = Path(args.output) if args.output else Path('/home/scarter4work/projects/NukeX/training_data/test_results')
    output_dir.mkdir(parents=True, exist_ok=True)

    base_name = fits_path.stem

    # Save stretched image
    stretched_path = output_dir / f"{base_name}_stretched.png"
    Image.fromarray((stretched * 255).astype(np.uint8)).save(stretched_path)
    print(f"\nSaved stretched image: {stretched_path}")

    # Save segmentation visualization
    seg_path = output_dir / f"{base_name}_segmented.png"
    Image.fromarray(vis).save(seg_path)
    print(f"Saved segmentation: {seg_path}")

    # Save raw mask
    mask_path = output_dir / f"{base_name}_mask.png"
    Image.fromarray(pred.astype(np.uint8)).save(mask_path)
    print(f"Saved mask: {mask_path}")


if __name__ == '__main__':
    main()
