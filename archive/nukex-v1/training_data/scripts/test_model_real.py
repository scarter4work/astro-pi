#!/usr/bin/env python3
"""
Test v8 segmentation model on real images.
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


def preprocess_image(img_path: Path, target_size: int = 512) -> tuple[torch.Tensor, np.ndarray]:
    """Load and preprocess image for model input."""
    img = Image.open(img_path)

    # Convert to RGB if needed
    if img.mode != 'RGB':
        img = img.convert('RGB')

    # Store original for visualization
    original = np.array(img)

    # Resize to target size
    img = img.resize((target_size, target_size), Image.LANCZOS)
    img_np = np.array(img).astype(np.float32) / 255.0

    # Normalize using percentiles (match training preprocessing)
    for c in range(3):
        channel = img_np[:, :, c]
        p1, p99 = np.percentile(channel, [1, 99])
        if p99 > p1:
            img_np[:, :, c] = np.clip((channel - p1) / (p99 - p1), 0, 1)

    # Create 4-channel input (RGB + color contrast)
    r, g, b = img_np[:, :, 0], img_np[:, :, 1], img_np[:, :, 2]
    color_contrast = b - r  # Blue minus Red
    color_contrast = (color_contrast + 1) / 2  # Normalize to [0, 1]

    # Stack to 4 channels: R, G, B, ColorContrast
    input_4ch = np.stack([r, g, b, color_contrast], axis=0)

    # Convert to tensor and add batch dimension
    tensor = torch.from_numpy(input_4ch).unsqueeze(0)

    return tensor, original


def create_segmentation_overlay(original: np.ndarray, mask: np.ndarray, alpha: float = 0.5) -> np.ndarray:
    """Create visualization with segmentation overlay."""
    # Resize original to match mask
    h, w = mask.shape
    orig_resized = np.array(Image.fromarray(original).resize((w, h), Image.LANCZOS))

    # Create colored mask
    colored_mask = np.zeros((h, w, 3), dtype=np.uint8)
    for class_idx, color in enumerate(CLASS_COLORS):
        colored_mask[mask == class_idx] = color

    # Blend
    overlay = (orig_resized * (1 - alpha) + colored_mask * alpha).astype(np.uint8)

    return overlay


def run_inference(model_path: Path, image_path: Path, output_dir: Path):
    """Run inference on a single image."""
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # Load model
    print(f"Loading model: {model_path}")
    model = AstroUNet(in_channels=4, num_classes=21)
    checkpoint = torch.load(model_path, map_location=device)
    model.load_state_dict(checkpoint['model_state_dict'])
    model = model.to(device)
    model.eval()

    # Preprocess image
    print(f"Processing image: {image_path}")
    input_tensor, original = preprocess_image(image_path)
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
        if pct > 0.1:  # Only show classes > 0.1%
            print(f"  {CLASS_NAMES[cls_idx]:25s}: {pct:5.1f}% ({count:,} pixels)")

    print(f"\nMean confidence: {confidence.mean():.3f}")
    print(f"Min confidence:  {confidence.min():.3f}")
    print(f"Max confidence:  {confidence.max():.3f}")

    # Save outputs
    output_dir.mkdir(parents=True, exist_ok=True)

    # Save segmentation mask
    mask_colored = np.zeros((pred_mask.shape[0], pred_mask.shape[1], 3), dtype=np.uint8)
    for cls_idx, color in enumerate(CLASS_COLORS):
        mask_colored[pred_mask == cls_idx] = color

    mask_path = output_dir / f"{image_path.stem}_segmentation.png"
    Image.fromarray(mask_colored).save(mask_path)
    print(f"\nSaved segmentation mask: {mask_path}")

    # Save overlay
    overlay = create_segmentation_overlay(original, pred_mask, alpha=0.4)
    overlay_path = output_dir / f"{image_path.stem}_overlay.png"
    Image.fromarray(overlay).save(overlay_path)
    print(f"Saved overlay: {overlay_path}")

    # Save confidence map
    conf_uint8 = (confidence * 255).astype(np.uint8)
    conf_path = output_dir / f"{image_path.stem}_confidence.png"
    Image.fromarray(conf_uint8).save(conf_path)
    print(f"Saved confidence map: {conf_path}")

    return pred_mask, confidence


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Test segmentation model on real images")
    parser.add_argument("--model", type=str, required=True, help="Path to model checkpoint")
    parser.add_argument("--image", type=str, required=True, help="Path to input image")
    parser.add_argument("--output", type=str, default="./test_output", help="Output directory")

    args = parser.parse_args()

    run_inference(
        model_path=Path(args.model),
        image_path=Path(args.image),
        output_dir=Path(args.output)
    )
