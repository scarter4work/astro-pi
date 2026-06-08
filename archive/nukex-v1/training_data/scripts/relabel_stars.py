#!/usr/bin/env python3
"""
Relabel star pixels by brightness to fill in star_bright, star_medium, star_faint classes.
The model tends to classify all stars as one class - this splits them by intensity.
"""

import numpy as np
from pathlib import Path
from PIL import Image
from tqdm import tqdm
import argparse

# Class indices
CLASS_BACKGROUND = 0
CLASS_STAR_BRIGHT = 1
CLASS_STAR_MEDIUM = 2
CLASS_STAR_FAINT = 3
CLASS_STAR_SATURATED = 4


def relabel_stars_by_brightness(mask, img):
    """
    Relabel star pixels based on brightness levels.

    - Saturated (top 1%): class 4
    - Bright (top 10%): class 1
    - Medium (top 30%): class 2
    - Faint (top 60%): class 3
    """
    new_mask = mask.copy()

    # Convert to grayscale
    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Find point sources (stars) - small bright regions
    # Use intensity thresholds
    saturated_thresh = np.percentile(gray, 99.5)
    bright_thresh = np.percentile(gray, 95)
    medium_thresh = np.percentile(gray, 85)
    faint_thresh = np.percentile(gray, 70)

    # Find existing star pixels (classes 1-4) or identify new ones
    existing_stars = (mask >= CLASS_STAR_BRIGHT) & (mask <= CLASS_STAR_SATURATED)

    # Also identify potential stars that might be misclassified as background
    # Stars are typically small bright spots
    from scipy import ndimage

    # Simple approach: any bright isolated pixel is likely a star
    potential_stars = gray > faint_thresh

    # Create star mask combining existing and potential
    star_mask = existing_stars | potential_stars

    # Now classify by brightness
    new_mask[star_mask & (gray >= saturated_thresh)] = CLASS_STAR_SATURATED
    new_mask[star_mask & (gray >= bright_thresh) & (gray < saturated_thresh)] = CLASS_STAR_BRIGHT
    new_mask[star_mask & (gray >= medium_thresh) & (gray < bright_thresh)] = CLASS_STAR_MEDIUM
    new_mask[star_mask & (gray >= faint_thresh) & (gray < medium_thresh)] = CLASS_STAR_FAINT

    return new_mask


def process_directory(data_dir, subdirs=None):
    """Process all masks in a directory."""
    data_path = Path(data_dir)

    if subdirs:
        mask_files = []
        for subdir in subdirs:
            subpath = data_path / subdir
            if subpath.exists():
                mask_files.extend(list(subpath.rglob("*_mask.png")))
    else:
        mask_files = list(data_path.rglob("*_mask.png"))

    if not mask_files:
        print(f"No masks found in {data_dir}")
        return 0

    print(f"Processing {len(mask_files)} masks...")

    processed = 0
    for mask_path in tqdm(mask_files):
        # Find corresponding image
        img_path = mask_path.with_name(mask_path.name.replace("_mask.png", "_img.png"))
        if not img_path.exists():
            continue

        # Load
        mask = np.array(Image.open(mask_path))
        img = np.array(Image.open(img_path))

        # Relabel
        new_mask = relabel_stars_by_brightness(mask, img)

        # Save
        Image.fromarray(new_mask).save(mask_path)
        processed += 1

    return processed


def main():
    parser = argparse.ArgumentParser(description='Relabel stars by brightness')
    parser.add_argument('--labeled-dir', default='/home/scarter4work/projects/NukeX/training_data/labeled',
                       help='Labeled data directory')
    parser.add_argument('--subdirs', nargs='+',
                       default=['star_clusters', 'globular_cluster', 'bright_emission'],
                       help='Subdirectories to process')
    args = parser.parse_args()

    print("=" * 60)
    print("Star Brightness Relabeling")
    print("=" * 60)

    count = process_directory(args.labeled_dir, args.subdirs)
    print(f"\nRelabeled {count} masks")


if __name__ == "__main__":
    main()
