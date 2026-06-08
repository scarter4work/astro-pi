#!/usr/bin/env python3
"""
Comprehensive V6 Model Testing Suite

Tests the v6 segmentation model on:
1. Local RGB composites (QNAP_composites, Erellaz, M78, NOIRLab)
2. ESO FITS data
3. QNAP subframes (if mounted at /mnt/qnap/astro_data)

Outputs detailed statistics and optional visualization masks.
"""

import os
import sys
import json
import numpy as np
import torch
from PIL import Image
from pathlib import Path
from datetime import datetime
from collections import defaultdict

# For FITS handling
try:
    from astropy.io import fits
    HAS_ASTROPY = True
except ImportError:
    HAS_ASTROPY = False
    print("Warning: astropy not installed, FITS support disabled")

sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet

# Import unified palette
from segmentation_palette import (
    NUM_CLASSES,
    CLASS_NAMES,
    CLASS_COLORS_RGB,
    apply_colormap,
)

# Convert to dictionary format for backward compatibility
CLASS_COLORS = {i: color for i, color in enumerate(CLASS_COLORS_RGB)}

Image.MAX_IMAGE_PIXELS = None

# Data source paths
DATA_SOURCES = {
    'qnap_composites': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/QNAP_composites',
    'erellaz_composites': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/Erellaz_composites',
    'm78_composites': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/M78_composites',
    'noirlab': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/NOIRLab',
    'eso_emission': '/home/scarter4work/projects/NukeX/training_data/eso/bright_emission',
    'eso_planetary': '/home/scarter4work/projects/NukeX/training_data/eso/planetary_nebula',
    'eso_clusters': '/home/scarter4work/projects/NukeX/training_data/eso/star_clusters',
    'qnap_subs': '/mnt/qnap/astro_data',
}

# Object type hints from filenames
OBJECT_HINTS = {
    # Emission nebulae
    'm42': 'emission', 'orion': 'emission', 'm8': 'emission', 'lagoon': 'emission',
    'm20': 'emission', 'trifid': 'emission', 'm16': 'emission', 'eagle': 'emission',
    'm17': 'emission', 'omega': 'emission', 'ic443': 'emission', 'jellyfish': 'emission',
    'ngc7635': 'emission', 'bubble': 'emission', 'ngc6888': 'emission', 'crescent': 'emission',
    'm1': 'emission', 'crab': 'emission',
    # Reflection nebulae
    'm78': 'reflection', 'm45': 'reflection', 'pleiades': 'reflection',
    'ngc1333': 'reflection', 'ngc1999': 'reflection', 'ngc7023': 'reflection', 'iris': 'reflection',
    # Planetary nebulae
    'm57': 'planetary', 'ring': 'planetary', 'm27': 'planetary', 'dumbbell': 'planetary',
    'ngc7293': 'planetary', 'helix': 'planetary',
    # Galaxies
    'm31': 'galaxy', 'andromeda': 'galaxy', 'm33': 'galaxy', 'triangulum': 'galaxy',
    'm101': 'galaxy', 'pinwheel': 'galaxy', 'm51': 'galaxy', 'whirlpool': 'galaxy',
    'ngc6946': 'galaxy', 'fireworks': 'galaxy', 'm66': 'galaxy',
    # Star clusters
    'm13': 'cluster', 'm15': 'cluster', 'm39': 'cluster', 'm92': 'cluster',
}


def load_model(model_path, device):
    """Load the v6 model."""
    model = AstroUNet(in_channels=4, num_classes=NUM_CLASSES)
    checkpoint = torch.load(model_path, map_location=device, weights_only=True)
    if 'model_state_dict' in checkpoint:
        model.load_state_dict(checkpoint['model_state_dict'])
    else:
        model.load_state_dict(checkpoint)
    model = model.to(device)
    model.eval()
    return model


def normalize_image(img_array):
    """Normalize image to 0-1 range."""
    if img_array.dtype == np.uint16:
        img_array = img_array.astype(np.float32) / 65535.0
    elif img_array.dtype == np.uint8:
        img_array = img_array.astype(np.float32) / 255.0
    elif img_array.max() > 1.0:
        img_array = img_array.astype(np.float32)
        img_array = (img_array - img_array.min()) / (img_array.max() - img_array.min() + 1e-10)
    return img_array


def load_fits_as_rgb(fits_path):
    """Load FITS file and convert to RGB array."""
    if not HAS_ASTROPY:
        return None

    try:
        with fits.open(fits_path) as hdul:
            data = None
            for hdu in hdul:
                if hdu.data is not None and len(hdu.data.shape) >= 2:
                    data = hdu.data
                    break

            if data is None:
                return None

            # Handle different data shapes
            if len(data.shape) == 2:
                # Grayscale - convert to RGB
                data = normalize_image(data)
                rgb = np.stack([data, data, data], axis=-1)
            elif len(data.shape) == 3:
                if data.shape[0] in [3, 4]:
                    # Channel-first (C, H, W)
                    data = np.transpose(data, (1, 2, 0))
                if data.shape[2] >= 3:
                    rgb = normalize_image(data[:, :, :3])
                else:
                    rgb = normalize_image(data[:, :, 0])
                    rgb = np.stack([rgb, rgb, rgb], axis=-1)
            else:
                return None

            return (rgb * 255).astype(np.uint8)
    except Exception as e:
        print(f"  Error loading FITS {fits_path}: {e}")
        return None


def segment_image(model, image, device, tile_size=512):
    """Segment an image using tiled inference."""
    img_array = np.array(image).astype(np.float32) / 255.0
    h, w = img_array.shape[:2]

    # Add color contrast channel (B-R for emission/reflection discrimination)
    blue = img_array[:, :, 2]
    red = img_array[:, :, 0]
    green = img_array[:, :, 1]

    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_mono = channel_std < 0.01

    if is_mono:
        color_contrast = np.zeros_like(blue)
    else:
        color_contrast = blue - red

    img_4ch = np.concatenate([img_array, color_contrast[:, :, np.newaxis]], axis=2)
    output_mask = np.zeros((h, w), dtype=np.uint8)

    stride = tile_size // 2

    for y in range(0, h, stride):
        for x in range(0, w, stride):
            y_end = min(y + tile_size, h)
            x_end = min(x + tile_size, w)
            y_start = max(0, y_end - tile_size)
            x_start = max(0, x_end - tile_size)

            tile = img_4ch[y_start:y_end, x_start:x_end]

            pad_h = tile_size - tile.shape[0]
            pad_w = tile_size - tile.shape[1]
            if pad_h > 0 or pad_w > 0:
                tile = np.pad(tile, ((0, pad_h), (0, pad_w), (0, 0)), mode='reflect')

            tile_tensor = torch.from_numpy(tile).permute(2, 0, 1).unsqueeze(0).float().to(device)

            with torch.no_grad():
                pred = model(tile_tensor)
                pred_mask = pred.argmax(dim=1).squeeze().cpu().numpy()

            if pad_h > 0:
                pred_mask = pred_mask[:-pad_h]
            if pad_w > 0:
                pred_mask = pred_mask[:, :-pad_w]

            margin = stride // 4
            copy_y_start = margin if y > 0 else 0
            copy_y_end = pred_mask.shape[0] - margin if y_end < h else pred_mask.shape[0]
            copy_x_start = margin if x > 0 else 0
            copy_x_end = pred_mask.shape[1] - margin if x_end < w else pred_mask.shape[1]

            out_y_start = y_start + copy_y_start
            out_y_end = y_start + copy_y_end
            out_x_start = x_start + copy_x_start
            out_x_end = x_start + copy_x_end

            output_mask[out_y_start:out_y_end, out_x_start:out_x_end] = \
                pred_mask[copy_y_start:copy_y_end, copy_x_start:copy_x_end]

    return output_mask


def create_visualization(image, mask, output_path):
    """Create side-by-side visualization of image and colored mask."""
    img_array = np.array(image)
    h, w = mask.shape

    # Create colored mask
    colored_mask = np.zeros((h, w, 3), dtype=np.uint8)
    for class_idx, color in CLASS_COLORS.items():
        colored_mask[mask == class_idx] = color

    # Resize if needed
    if img_array.shape[:2] != (h, w):
        from PIL import Image as PILImage
        img_resized = PILImage.fromarray(img_array).resize((w, h), PILImage.BILINEAR)
        img_array = np.array(img_resized)

    # Create side-by-side
    combined = np.concatenate([img_array, colored_mask], axis=1)

    # Save
    Image.fromarray(combined).save(output_path)


def infer_object_type(filename):
    """Infer expected object type from filename."""
    fname_lower = filename.lower()
    for key, obj_type in OBJECT_HINTS.items():
        if key in fname_lower:
            return obj_type
    return 'unknown'


def analyze_segmentation(mask, expected_type):
    """Analyze segmentation results and compare to expected type."""
    total = mask.size
    results = {
        'class_percentages': {},
        'expected_type': expected_type,
        'detected_type': 'unknown',
        'match': None,
    }

    # Calculate percentages for each class
    for i, name in enumerate(CLASS_NAMES):
        count = np.sum(mask == i)
        pct = 100.0 * count / total
        if pct > 0.01:  # Only record classes with >0.01%
            results['class_percentages'][name] = round(pct, 3)

    # Determine detected type based on segmentation
    emission_pct = results['class_percentages'].get('nebula_emission', 0)
    reflection_pct = results['class_percentages'].get('nebula_reflection', 0)
    planetary_pct = results['class_percentages'].get('nebula_planetary', 0)
    galaxy_pct = sum([
        results['class_percentages'].get('galaxy_spiral', 0),
        results['class_percentages'].get('galaxy_elliptical', 0),
        results['class_percentages'].get('galaxy_irregular', 0),
        results['class_percentages'].get('galaxy_core', 0),
    ])
    cluster_pct = sum([
        results['class_percentages'].get('star_cluster_open', 0),
        results['class_percentages'].get('star_cluster_globular', 0),
    ])

    # Determine dominant type
    type_scores = {
        'emission': emission_pct,
        'reflection': reflection_pct,
        'planetary': planetary_pct,
        'galaxy': galaxy_pct,
        'cluster': cluster_pct,
    }

    max_type = max(type_scores, key=type_scores.get)
    max_score = type_scores[max_type]

    if max_score > 1.0:  # At least 1% of image
        results['detected_type'] = max_type
    else:
        results['detected_type'] = 'mostly_stars_or_background'

    # Check match
    if expected_type == 'unknown':
        results['match'] = 'n/a'
    elif results['detected_type'] == expected_type:
        results['match'] = 'correct'
    elif results['detected_type'] == 'mostly_stars_or_background':
        results['match'] = 'ambiguous'
    else:
        results['match'] = 'incorrect'

    return results


def find_qnap_subframes(qnap_path, max_per_target=3):
    """Find representative subframes from QNAP astro_data."""
    subframes = []

    if not os.path.exists(qnap_path):
        return subframes

    # Look for common patterns
    target_dirs = defaultdict(list)

    for root, dirs, files in os.walk(qnap_path):
        for f in files:
            if f.lower().endswith(('.fits', '.fit', '.xisf')):
                # Try to identify target from path
                path_parts = root.lower()
                for hint in OBJECT_HINTS.keys():
                    if hint in path_parts or hint in f.lower():
                        target_dirs[hint].append(os.path.join(root, f))
                        break

    # Select a few from each target
    for target, files in target_dirs.items():
        for f in files[:max_per_target]:
            subframes.append((f, OBJECT_HINTS.get(target, 'unknown')))

    return subframes


def test_data_source(model, device, source_name, source_path, results, output_dir, max_images=10):
    """Test model on a specific data source."""
    print(f"\n{'='*60}")
    print(f"Testing: {source_name}")
    print(f"Path: {source_path}")
    print(f"{'='*60}")

    if not os.path.exists(source_path):
        print(f"  Source not found, skipping")
        return

    # Find images
    images = []
    for ext in ['*.png', '*.jpg', '*.jpeg', '*.tif', '*.tiff', '*.fits', '*.fit']:
        images.extend(Path(source_path).glob(ext))
        images.extend(Path(source_path).glob(ext.upper()))

    # Limit number of images
    if len(images) > max_images:
        # Sample evenly
        step = len(images) // max_images
        images = images[::step][:max_images]

    print(f"  Found {len(images)} images to test")

    source_results = []

    for img_path in images:
        print(f"\n  Processing: {img_path.name}")

        try:
            # Load image
            if img_path.suffix.lower() in ['.fits', '.fit']:
                img_array = load_fits_as_rgb(str(img_path))
                if img_array is None:
                    print(f"    Failed to load FITS")
                    continue
                img = Image.fromarray(img_array)
            else:
                img = Image.open(img_path).convert('RGB')

            # Resize if very large
            max_dim = 2048
            if max(img.size) > max_dim:
                ratio = max_dim / max(img.size)
                new_size = (int(img.size[0] * ratio), int(img.size[1] * ratio))
                img = img.resize(new_size, Image.BILINEAR)
                print(f"    Resized to {new_size}")

            # Infer expected type
            expected_type = infer_object_type(img_path.name)
            print(f"    Expected type: {expected_type}")

            # Segment
            mask = segment_image(model, img, device)

            # Analyze
            analysis = analyze_segmentation(mask, expected_type)
            analysis['filename'] = img_path.name
            analysis['source'] = source_name

            # Print key results
            print(f"    Detected type: {analysis['detected_type']}")
            print(f"    Match: {analysis['match']}")

            # Top 5 classes
            sorted_classes = sorted(
                analysis['class_percentages'].items(),
                key=lambda x: x[1],
                reverse=True
            )[:5]
            print(f"    Top classes: {', '.join(f'{n}:{p:.1f}%' for n, p in sorted_classes)}")

            # Save visualization
            if output_dir:
                viz_path = output_dir / f"{source_name}_{img_path.stem}_viz.jpg"
                create_visualization(img, mask, str(viz_path))

            source_results.append(analysis)

        except Exception as e:
            print(f"    Error: {e}")
            import traceback
            traceback.print_exc()

    results[source_name] = source_results


def print_summary(results):
    """Print summary statistics."""
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")

    total_correct = 0
    total_incorrect = 0
    total_ambiguous = 0
    total_na = 0

    for source, source_results in results.items():
        correct = sum(1 for r in source_results if r['match'] == 'correct')
        incorrect = sum(1 for r in source_results if r['match'] == 'incorrect')
        ambiguous = sum(1 for r in source_results if r['match'] == 'ambiguous')
        na = sum(1 for r in source_results if r['match'] == 'n/a')

        total_correct += correct
        total_incorrect += incorrect
        total_ambiguous += ambiguous
        total_na += na

        print(f"\n{source}:")
        print(f"  Tested: {len(source_results)}")
        print(f"  Correct: {correct}, Incorrect: {incorrect}, Ambiguous: {ambiguous}, N/A: {na}")

        if correct + incorrect > 0:
            acc = 100.0 * correct / (correct + incorrect)
            print(f"  Accuracy (excl. ambiguous): {acc:.1f}%")

    print(f"\n{'='*60}")
    print("OVERALL:")
    print(f"  Total tested: {total_correct + total_incorrect + total_ambiguous + total_na}")
    print(f"  Correct: {total_correct}")
    print(f"  Incorrect: {total_incorrect}")
    print(f"  Ambiguous: {total_ambiguous}")
    print(f"  Unknown expected type: {total_na}")

    if total_correct + total_incorrect > 0:
        overall_acc = 100.0 * total_correct / (total_correct + total_incorrect)
        print(f"  Overall accuracy (excl. ambiguous/na): {overall_acc:.1f}%")


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Comprehensive V6 Model Testing')
    parser.add_argument('--model', default='/home/scarter4work/projects/NukeX/training_data/models_v6/best_model.pth',
                        help='Path to model checkpoint')
    parser.add_argument('--output-dir', default='/tmp/v6_test_results',
                        help='Directory to save visualizations')
    parser.add_argument('--max-per-source', type=int, default=10,
                        help='Maximum images to test per source')
    parser.add_argument('--sources', nargs='+', default=None,
                        help='Specific sources to test (default: all)')
    parser.add_argument('--no-viz', action='store_true',
                        help='Skip saving visualizations')
    args = parser.parse_args()

    # Setup
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # Load model
    print(f"Loading model: {args.model}")
    model = load_model(args.model, device)

    # Create output directory
    output_dir = None
    if not args.no_viz:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        print(f"Saving visualizations to: {output_dir}")

    # Determine which sources to test
    sources_to_test = args.sources if args.sources else list(DATA_SOURCES.keys())

    # Run tests
    results = {}

    for source_name in sources_to_test:
        if source_name not in DATA_SOURCES:
            print(f"Unknown source: {source_name}")
            continue

        source_path = DATA_SOURCES[source_name]
        test_data_source(model, device, source_name, source_path, results, output_dir, args.max_per_source)

    # Print summary
    print_summary(results)

    # Save results JSON
    if output_dir:
        results_file = output_dir / f"test_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        with open(results_file, 'w') as f:
            json.dump(results, f, indent=2)
        print(f"\nResults saved to: {results_file}")


if __name__ == "__main__":
    main()
