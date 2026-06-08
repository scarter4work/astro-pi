#!/usr/bin/env python3
"""
Relabel targeted downloads with correct class assignments.
"""

import numpy as np
from pathlib import Path
from PIL import Image
from tqdm import tqdm
from scipy import ndimage

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


def relabel_elliptical_galaxy(mask, img):
    """Relabel as elliptical galaxy - smooth, featureless structure."""
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    h, w = gray.shape
    center_y, center_x = h // 2, w // 2

    y, x = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((x - center_x)**2 + (y - center_y)**2)
    max_dist = np.sqrt(center_x**2 + center_y**2)

    # Elliptical galaxies: smooth brightness falling from center
    bright_thresh = np.percentile(gray, 70)
    core_thresh = np.percentile(gray, 90)

    # Core region
    core_mask = (gray > core_thresh) & (dist_from_center < max_dist * 0.3)
    new_mask[core_mask] = CLASS_GALAXY_CORE

    # Extended halo
    halo_mask = (gray > bright_thresh) & ~core_mask & (dist_from_center < max_dist * 0.6)
    new_mask[halo_mask & (new_mask == CLASS_BACKGROUND)] = CLASS_GALAXY_ELLIPTICAL

    return new_mask


def relabel_irregular_galaxy(mask, img):
    """Relabel as irregular galaxy - patchy, asymmetric structure."""
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Irregular galaxies have patchy, asymmetric structure
    bright_thresh = np.percentile(gray, 60)

    # Any bright region that's not a point source
    bright_regions = gray > bright_thresh

    # Exclude point sources (stars) using local maxima detection
    local_max = ndimage.maximum_filter(gray, size=5)
    is_local_max = (gray == local_max)
    point_sources = is_local_max & (gray > np.percentile(gray, 95))

    # Irregular galaxy: bright regions that aren't stars
    galaxy_mask = bright_regions & ~point_sources

    new_mask[(galaxy_mask) & ((new_mask == CLASS_BACKGROUND) |
                               (new_mask == CLASS_NEBULA_EMISSION))] = CLASS_GALAXY_IRREGULAR

    return new_mask


def relabel_dust_lane(mask, img):
    """Relabel as dust lane - dark absorption in edge-on galaxies."""
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Dust lanes: dark regions surrounded by brighter material
    local_mean = ndimage.uniform_filter(gray, size=30)
    contrast = local_mean - gray

    # Dust lane: significantly darker than surroundings, not too dark (not background)
    dark_thresh = np.percentile(gray, 40)
    dust_mask = (contrast > 0.03) & (gray > np.percentile(gray, 15)) & (gray < dark_thresh)

    new_mask[(dust_mask) & ((new_mask == CLASS_BACKGROUND) |
                            (new_mask == CLASS_NEBULA_DARK))] = CLASS_DUST_LANE

    # Bright regions around dust lanes are the galaxy
    bright_mask = gray > np.percentile(gray, 65)
    new_mask[(bright_mask) & (new_mask == CLASS_BACKGROUND)] = CLASS_GALAXY_SPIRAL

    return new_mask


def relabel_star_field(mask, img):
    """Relabel star field by brightness levels."""
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Find stars (point sources)
    local_max = ndimage.maximum_filter(gray, size=3)
    is_star = (gray == local_max) & (gray > np.percentile(gray, 50))

    # Classify by brightness
    sat_thresh = np.percentile(gray, 99.5)
    bright_thresh = np.percentile(gray, 95)
    medium_thresh = np.percentile(gray, 80)
    faint_thresh = np.percentile(gray, 60)

    new_mask[is_star & (gray >= sat_thresh)] = CLASS_STAR_SATURATED
    new_mask[is_star & (gray >= bright_thresh) & (gray < sat_thresh)] = CLASS_STAR_BRIGHT
    new_mask[is_star & (gray >= medium_thresh) & (gray < bright_thresh)] = CLASS_STAR_MEDIUM
    new_mask[is_star & (gray >= faint_thresh) & (gray < medium_thresh)] = CLASS_STAR_FAINT

    return new_mask


def process_directory(data_dir, category, relabel_func):
    """Process all masks in a directory."""
    data_path = Path(data_dir) / category

    if not data_path.exists():
        print(f"  Directory not found: {data_path}")
        return 0

    mask_files = list(data_path.rglob("*_mask.png"))
    if not mask_files:
        print(f"  No masks found in {category}")
        return 0

    print(f"  Processing {len(mask_files)} masks for {category}...")

    processed = 0
    for mask_path in tqdm(mask_files, desc=f"    {category}"):
        img_path = mask_path.with_name(mask_path.name.replace("_mask.png", "_img.png"))
        if not img_path.exists():
            continue

        mask = np.array(Image.open(mask_path))
        img = np.array(Image.open(img_path))

        new_mask = relabel_func(mask, img)
        Image.fromarray(new_mask).save(mask_path)
        processed += 1

    return processed


def main():
    labeled_dir = Path("/home/scarter4work/projects/NukeX/training_data/labeled_targeted")

    print("=" * 60)
    print("Targeted Data Relabeling")
    print("=" * 60)

    # Category to relabeling function
    categories = {
        'galaxy_elliptical': relabel_elliptical_galaxy,
        'galaxy_irregular': relabel_irregular_galaxy,
        'dust_lane': relabel_dust_lane,
        'star_field': relabel_star_field,
    }

    total = 0
    for category, func in categories.items():
        count = process_directory(labeled_dir, category, func)
        total += count

    print(f"\nTotal masks relabeled: {total}")


if __name__ == "__main__":
    main()
