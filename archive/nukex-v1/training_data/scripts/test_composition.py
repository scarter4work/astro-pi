#!/usr/bin/env python3
"""
Test the Segment Composition Pipeline for NukeX

This script tests the compose_segments.py pipeline by:
1. Loading a sample image (linear or RGB)
2. Either loading an existing mask or generating one from the v8 model
3. Applying the composition pipeline with per-segment stretching
4. Outputting results for visual inspection

Usage:
    # With existing mask
    python test_composition.py --image input.tiff --mask segmentation.png --output results/

    # Generate mask from model
    python test_composition.py --image input.tiff --model models/best_model.pth --output results/

    # Run demo with synthetic data
    python test_composition.py --demo --output results/

    # Compare soft vs hard blending
    python test_composition.py --image input.tiff --mask seg.png --compare-blending

Copyright (c) 2026 Scott Carter
"""

import argparse
import sys
from pathlib import Path
from typing import Tuple, Optional
import numpy as np

# Add scripts directory to path
SCRIPT_DIR = Path(__file__).parent
sys.path.insert(0, str(SCRIPT_DIR))

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    import torch
    HAS_TORCH = True
except ImportError:
    HAS_TORCH = False

# Import composition functions
from compose_segments import (
    compose_segments,
    compose_segments_simple,
    load_image,
    load_mask,
    save_image,
    create_synthetic_test_data,
    STRETCH_CONFIG,
    CLASS_NAMES,
    NUM_CLASSES,
)

# Import post-processing
try:
    from mask_postprocess import postprocess_mask, validate_mask
    HAS_POSTPROCESS = True
except ImportError:
    HAS_POSTPROCESS = False
    print("Warning: mask_postprocess not available")

# Import visualization palette
try:
    from segmentation_palette import (
        apply_colormap,
        create_legend_image,
        CLASS_DISPLAY_NAMES,
        CLASS_COLORS_RGB,
    )
    HAS_PALETTE = True
except ImportError:
    HAS_PALETTE = False
    print("Warning: segmentation_palette not available")


def load_model_for_inference(model_path: str, device: str = 'cuda'):
    """
    Load the trained segmentation model.

    Args:
        model_path: Path to the model checkpoint
        device: 'cuda' or 'cpu'

    Returns:
        Loaded model in eval mode
    """
    if not HAS_TORCH:
        raise ImportError("PyTorch is required for model inference")

    # Import model architecture
    sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
    from model import AstroUNet

    device = torch.device(device if torch.cuda.is_available() else 'cpu')
    model = AstroUNet(in_channels=4, num_classes=NUM_CLASSES, base_features=32)

    checkpoint = torch.load(model_path, map_location=device)
    model.load_state_dict(checkpoint['model_state_dict'])
    model = model.to(device)
    model.eval()

    return model, device


def generate_mask_from_model(
    image: np.ndarray,
    model,
    device,
    size: int = 512
) -> np.ndarray:
    """
    Generate segmentation mask using the trained model.

    Args:
        image: Input image (H, W, 3) RGB, values 0-1
        model: Loaded PyTorch model
        device: torch device
        size: Processing size

    Returns:
        Segmentation mask (H, W) with class IDs
    """
    import torch

    H, W = image.shape[:2]

    # Resize for inference
    img_pil = Image.fromarray((image * 255).astype(np.uint8))
    img_resized = img_pil.resize((size, size), Image.BILINEAR)
    img_np = np.array(img_resized).astype(np.float32) / 255.0

    # Create 4-channel input (RGB + color contrast)
    if img_np.ndim == 2:
        img_np = np.stack([img_np, img_np, img_np], axis=-1)

    blue = img_np[:, :, 2]
    red = img_np[:, :, 0]
    green = img_np[:, :, 1]

    # Detect narrowband/monochrome
    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_narrowband = channel_std < 0.01

    if is_narrowband:
        color_contrast = np.zeros_like(blue)[:, :, np.newaxis]
    else:
        color_contrast = (blue - red)[:, :, np.newaxis]

    img_4ch = np.concatenate([img_np, color_contrast], axis=2)
    img_tensor = torch.from_numpy(img_4ch).permute(2, 0, 1).unsqueeze(0).to(device)

    # Run inference
    with torch.no_grad():
        output = model(img_tensor)
        pred = output.argmax(dim=1).squeeze().cpu().numpy()

    # Resize back to original size
    pred_pil = Image.fromarray(pred.astype(np.uint8))
    pred_resized = pred_pil.resize((W, H), Image.NEAREST)

    return np.array(pred_resized).astype(np.int32)


def create_comparison_image(
    original: np.ndarray,
    mask: np.ndarray,
    composed_soft: np.ndarray,
    composed_hard: Optional[np.ndarray] = None
) -> np.ndarray:
    """
    Create a side-by-side comparison image.

    Args:
        original: Original input image
        mask: Segmentation mask
        composed_soft: Soft-blended composition
        composed_hard: Hard-blended composition (optional)

    Returns:
        Combined comparison image
    """
    # Ensure all images are uint8 RGB
    def to_rgb_uint8(img):
        if img.ndim == 2:
            img = np.stack([img, img, img], axis=-1)
        if img.max() <= 1.0:
            img = (img * 255).astype(np.uint8)
        return img.astype(np.uint8)

    original_rgb = to_rgb_uint8(original)
    composed_soft_rgb = to_rgb_uint8(composed_soft)

    # Colorize mask for visualization
    if HAS_PALETTE:
        mask_rgb = apply_colormap(mask)
    else:
        mask_rgb = (mask.astype(np.float32) / NUM_CLASSES * 255).astype(np.uint8)
        mask_rgb = np.stack([mask_rgb, mask_rgb, mask_rgb], axis=-1)

    H, W = original.shape[:2]

    if composed_hard is not None:
        # 4-panel comparison: Original | Mask | Hard | Soft
        composed_hard_rgb = to_rgb_uint8(composed_hard)
        comparison = np.zeros((H, W * 4, 3), dtype=np.uint8)
        comparison[:, 0:W] = original_rgb
        comparison[:, W:W*2] = mask_rgb
        comparison[:, W*2:W*3] = composed_hard_rgb
        comparison[:, W*3:W*4] = composed_soft_rgb
    else:
        # 3-panel comparison: Original | Mask | Composed
        comparison = np.zeros((H, W * 3, 3), dtype=np.uint8)
        comparison[:, 0:W] = original_rgb
        comparison[:, W:W*2] = mask_rgb
        comparison[:, W*2:W*3] = composed_soft_rgb

    return comparison


def add_labels_to_comparison(
    comparison: np.ndarray,
    labels: list,
    font_size: int = 20
) -> np.ndarray:
    """
    Add text labels to the comparison image.

    Args:
        comparison: Comparison image
        labels: List of labels for each panel
        font_size: Font size for labels

    Returns:
        Labeled comparison image
    """
    if not HAS_PIL:
        return comparison

    from PIL import ImageDraw, ImageFont

    H, W = comparison.shape[:2]
    panel_width = W // len(labels)

    img = Image.fromarray(comparison)
    draw = ImageDraw.Draw(img)

    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", font_size)
    except:
        font = ImageFont.load_default()

    for i, label in enumerate(labels):
        x = i * panel_width + 10
        y = 10
        # Draw shadow for visibility
        draw.text((x+2, y+2), label, fill=(0, 0, 0), font=font)
        draw.text((x, y), label, fill=(255, 255, 255), font=font)

    return np.array(img)


def print_composition_stats(metadata: dict, verbose: bool = True):
    """
    Print statistics about the composition process.

    Args:
        metadata: Metadata dict from compose_segments()
        verbose: Print detailed per-class information
    """
    print("\n" + "=" * 60)
    print("COMPOSITION STATISTICS")
    print("=" * 60)

    print(f"Image shape: {metadata['image_shape']}")
    print(f"Blend sigma: {metadata['blend_sigma']}")
    print(f"Classes found: {len(metadata['classes_found'])}")

    if verbose:
        print("\nPer-class breakdown:")
        print("-" * 60)
        print(f"{'Class':<4} {'Name':<25} {'Pixels':>10} {'Stretch':<10}")
        print("-" * 60)

        total_pixels = 0
        for cls_info in metadata['classes_stretched']:
            total_pixels += cls_info['pixel_count']

        for cls_info in sorted(metadata['classes_stretched'], key=lambda x: -x['pixel_count']):
            pct = cls_info['pixel_count'] / total_pixels * 100 if total_pixels > 0 else 0
            print(f"{cls_info['class_id']:<4} {cls_info['class_name']:<25} {cls_info['pixel_count']:>10,} ({pct:5.1f}%)  {cls_info['stretch_func']:<10}")

    print("=" * 60)


def run_demo(output_dir: Path, blend_sigma: float = 3.0, verbose: bool = True):
    """
    Run demo with synthetic test data.

    Args:
        output_dir: Directory to save outputs
        blend_sigma: Gaussian sigma for soft blending
        verbose: Print verbose output
    """
    print("\n" + "=" * 60)
    print("RUNNING DEMO WITH SYNTHETIC DATA")
    print("=" * 60)

    # Create synthetic data
    print("Creating synthetic test data...")
    image, mask = create_synthetic_test_data(512)
    print(f"  Image shape: {image.shape}, range: [{image.min():.3f}, {image.max():.3f}]")
    print(f"  Mask classes: {np.unique(mask)}")

    # Validate mask
    if HAS_POSTPROCESS:
        validation = validate_mask(mask)
        print(f"  Mask validation: {'PASS' if validation['valid'] else 'FAIL'}")

    # Apply post-processing to mask
    if HAS_POSTPROCESS:
        print("\nApplying mask post-processing...")
        mask_cleaned = postprocess_mask(
            mask,
            morph=True,
            min_region=50,
            smooth=False,
            verbose=verbose
        )
    else:
        mask_cleaned = mask

    # Compose with soft blending
    print(f"\nComposing with soft blending (sigma={blend_sigma})...")
    composed_soft, metadata = compose_segments(
        image, mask_cleaned,
        blend_sigma=blend_sigma,
        verbose=verbose
    )

    # Compose with hard blending for comparison
    print("\nComposing with hard blending...")
    composed_hard = compose_segments_simple(image, mask_cleaned)

    # Print stats
    print_composition_stats(metadata, verbose=verbose)

    # Create comparison image
    comparison = create_comparison_image(image, mask_cleaned, composed_soft, composed_hard)
    comparison = add_labels_to_comparison(
        comparison,
        ['Original (Linear)', 'Segmentation Mask', 'Hard Blending', 'Soft Blending']
    )

    # Save outputs
    output_dir.mkdir(parents=True, exist_ok=True)

    save_image(image, str(output_dir / 'demo_original.png'), bit_depth=8)
    save_image(composed_soft, str(output_dir / 'demo_composed_soft.png'), bit_depth=8)
    save_image(composed_hard, str(output_dir / 'demo_composed_hard.png'), bit_depth=8)
    Image.fromarray(comparison).save(str(output_dir / 'demo_comparison.png'))

    # Save mask visualization
    if HAS_PALETTE:
        mask_vis = apply_colormap(mask_cleaned)
        Image.fromarray(mask_vis).save(str(output_dir / 'demo_mask.png'))

        # Save legend
        legend = create_legend_image()
        Image.fromarray(legend).save(str(output_dir / 'demo_legend.png'))

    print(f"\n{'=' * 60}")
    print(f"OUTPUTS SAVED TO: {output_dir}")
    print(f"{'=' * 60}")
    print(f"  - demo_original.png      : Original linear image")
    print(f"  - demo_mask.png          : Segmentation mask (colorized)")
    print(f"  - demo_composed_soft.png : Soft-blended composition")
    print(f"  - demo_composed_hard.png : Hard-blended composition")
    print(f"  - demo_comparison.png    : Side-by-side comparison")
    if HAS_PALETTE:
        print(f"  - demo_legend.png        : Class color legend")

    return composed_soft, metadata


def run_test(
    image_path: str,
    mask_path: Optional[str],
    model_path: Optional[str],
    output_dir: Path,
    blend_sigma: float = 3.0,
    post_process: bool = True,
    min_region: int = 100,
    compare_blending: bool = False,
    verbose: bool = True
):
    """
    Run composition test on a real image.

    Args:
        image_path: Path to input image
        mask_path: Path to segmentation mask (or None to generate)
        model_path: Path to model (for generating mask)
        output_dir: Directory to save outputs
        blend_sigma: Gaussian sigma for soft blending
        post_process: Apply mask post-processing
        min_region: Minimum region size for post-processing
        compare_blending: Compare soft vs hard blending
        verbose: Print verbose output
    """
    print("\n" + "=" * 60)
    print("RUNNING COMPOSITION TEST")
    print("=" * 60)

    # Load input image
    print(f"Loading image: {image_path}")
    image = load_image(image_path)
    print(f"  Shape: {image.shape}, range: [{image.min():.4f}, {image.max():.4f}]")

    # Get segmentation mask
    if mask_path:
        print(f"Loading mask: {mask_path}")
        mask = load_mask(mask_path)
    elif model_path:
        print(f"Generating mask from model: {model_path}")
        model, device = load_model_for_inference(model_path)
        mask = generate_mask_from_model(image, model, device)
    else:
        raise ValueError("Either --mask or --model must be provided")

    print(f"  Mask shape: {mask.shape}, classes: {np.unique(mask)}")

    # Validate mask
    if HAS_POSTPROCESS:
        validation = validate_mask(mask)
        if not validation['valid']:
            print(f"  WARNING: Mask validation failed: {validation['errors']}")
        if validation['warnings']:
            for w in validation['warnings']:
                print(f"  Warning: {w}")

    # Apply post-processing
    if post_process and HAS_POSTPROCESS:
        print(f"\nApplying mask post-processing (min_region={min_region})...")
        mask_cleaned = postprocess_mask(
            mask,
            morph=True,
            min_region=min_region,
            smooth=False,
            verbose=verbose
        )
    else:
        mask_cleaned = mask

    # Compose with soft blending
    print(f"\nComposing with soft blending (sigma={blend_sigma})...")
    composed_soft, metadata = compose_segments(
        image, mask_cleaned,
        blend_sigma=blend_sigma,
        verbose=verbose
    )

    # Optionally compose with hard blending
    composed_hard = None
    if compare_blending:
        print("\nComposing with hard blending...")
        composed_hard = compose_segments_simple(image, mask_cleaned)

    # Print stats
    print_composition_stats(metadata, verbose=verbose)

    # Save outputs
    output_dir.mkdir(parents=True, exist_ok=True)

    input_stem = Path(image_path).stem

    # Save main outputs
    save_image(composed_soft, str(output_dir / f'{input_stem}_composed.png'), bit_depth=16)
    save_image(composed_soft, str(output_dir / f'{input_stem}_composed.tiff'), bit_depth=16)

    # Create and save comparison
    if compare_blending:
        comparison = create_comparison_image(image, mask_cleaned, composed_soft, composed_hard)
        comparison = add_labels_to_comparison(
            comparison,
            ['Original', 'Segmentation', 'Hard Blend', 'Soft Blend']
        )
        save_image(composed_hard, str(output_dir / f'{input_stem}_composed_hard.png'), bit_depth=8)
    else:
        comparison = create_comparison_image(image, mask_cleaned, composed_soft)
        comparison = add_labels_to_comparison(
            comparison,
            ['Original', 'Segmentation', 'Composed']
        )

    Image.fromarray(comparison).save(str(output_dir / f'{input_stem}_comparison.png'))

    # Save mask visualization
    if HAS_PALETTE:
        mask_vis = apply_colormap(mask_cleaned)
        Image.fromarray(mask_vis).save(str(output_dir / f'{input_stem}_mask.png'))

    print(f"\n{'=' * 60}")
    print(f"OUTPUTS SAVED TO: {output_dir}")
    print(f"{'=' * 60}")
    print(f"  - {input_stem}_composed.png   : 16-bit PNG composition")
    print(f"  - {input_stem}_composed.tiff  : 16-bit TIFF composition")
    print(f"  - {input_stem}_comparison.png : Side-by-side comparison")
    print(f"  - {input_stem}_mask.png       : Colorized mask")

    return composed_soft, metadata


def verify_stretch_mappings():
    """Verify all 21 classes have appropriate stretch mappings."""
    print("\n" + "=" * 60)
    print("VERIFYING STRETCH MAPPINGS FOR ALL 21 CLASSES")
    print("=" * 60)

    missing = []
    for i in range(NUM_CLASSES):
        if i not in STRETCH_CONFIG:
            missing.append(i)

    if missing:
        print(f"WARNING: Missing stretch configs for classes: {missing}")
        return False

    print(f"All {NUM_CLASSES} classes have stretch mappings configured.")
    print("\nStretch mapping summary:")
    print("-" * 60)
    print(f"{'ID':<4} {'Class Name':<25} {'Stretch':<10} {'Key Params'}")
    print("-" * 60)

    stretch_counts = {}
    for i in range(NUM_CLASSES):
        func_name, params = STRETCH_CONFIG[i]
        stretch_counts[func_name] = stretch_counts.get(func_name, 0) + 1

        # Format key parameters
        key_params = []
        if func_name == 'arcsinh':
            key_params.append(f"scale={params.get('scale', 0.1)}")
        elif func_name == 'ghs':
            key_params.append(f"D={params.get('D', 1.0)}")
            if params.get('HP', 1.0) < 1.0:
                key_params.append(f"HP={params.get('HP')}")
        elif func_name == 'mtf':
            key_params.append(f"mid={params.get('midtone', 0.2)}")
        elif func_name == 'linear':
            if params.get('gamma', 1.0) != 1.0:
                key_params.append(f"gamma={params.get('gamma')}")
            if params.get('white_point', 1.0) < 1.0:
                key_params.append(f"wp={params.get('white_point')}")

        class_name = CLASS_NAMES[i] if i < len(CLASS_NAMES) else f"class_{i}"
        print(f"{i:<4} {class_name:<25} {func_name:<10} {', '.join(key_params)}")

    print("-" * 60)
    print("\nStretch function usage:")
    for func_name, count in sorted(stretch_counts.items(), key=lambda x: -x[1]):
        print(f"  {func_name}: {count} classes")

    return True


def main():
    parser = argparse.ArgumentParser(
        description='Test the segment composition pipeline',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run demo with synthetic data
  python test_composition.py --demo --output results/demo

  # Test with existing mask
  python test_composition.py --image image.tiff --mask mask.png --output results/

  # Generate mask from model and compose
  python test_composition.py --image image.tiff --model models/best_model.pth --output results/

  # Compare soft vs hard blending
  python test_composition.py --image image.tiff --mask mask.png --compare-blending

  # Verify stretch mappings
  python test_composition.py --verify-stretches
        """
    )

    parser.add_argument('--image', '-i', type=str, help='Input image path')
    parser.add_argument('--mask', '-m', type=str, help='Segmentation mask path')
    parser.add_argument('--model', type=str, help='Model path for generating masks')
    parser.add_argument('--output', '-o', type=str, default='./composition_test_results',
                        help='Output directory (default: ./composition_test_results)')
    parser.add_argument('--blend-sigma', type=float, default=3.0,
                        help='Gaussian sigma for soft blending (default: 3.0)')
    parser.add_argument('--no-post-process', action='store_true',
                        help='Skip mask post-processing')
    parser.add_argument('--min-region', type=int, default=100,
                        help='Minimum region size for post-processing (default: 100)')
    parser.add_argument('--compare-blending', action='store_true',
                        help='Compare soft vs hard blending')
    parser.add_argument('--demo', action='store_true',
                        help='Run demo with synthetic data')
    parser.add_argument('--verify-stretches', action='store_true',
                        help='Verify all classes have stretch mappings')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print verbose output')

    args = parser.parse_args()

    output_dir = Path(args.output)

    # Verify stretch mappings
    if args.verify_stretches:
        success = verify_stretch_mappings()
        return 0 if success else 1

    # Demo mode
    if args.demo:
        run_demo(output_dir, blend_sigma=args.blend_sigma, verbose=args.verbose)
        return 0

    # Real image mode
    if not args.image:
        parser.error("--image is required (or use --demo)")

    if not args.mask and not args.model:
        parser.error("Either --mask or --model is required")

    run_test(
        image_path=args.image,
        mask_path=args.mask,
        model_path=args.model,
        output_dir=output_dir,
        blend_sigma=args.blend_sigma,
        post_process=not args.no_post_process,
        min_region=args.min_region,
        compare_blending=args.compare_blending,
        verbose=args.verbose
    )

    return 0


if __name__ == '__main__':
    sys.exit(main())
