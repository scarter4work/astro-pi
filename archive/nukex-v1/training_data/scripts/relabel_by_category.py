#!/usr/bin/env python3
"""
Rule-based relabeling for specific astronomical categories.
Assigns correct class labels based on directory name and image characteristics.
"""

import numpy as np
from pathlib import Path
from PIL import Image
from tqdm import tqdm
import argparse

# Class indices from the 21-class model
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
CLASS_ARTIFACT_HOT_PIXEL = 16
CLASS_ARTIFACT_SATELLITE = 17
CLASS_ARTIFACT_DIFFRACTION = 18
CLASS_ARTIFACT_GRADIENT = 19
CLASS_ARTIFACT_NOISE = 20


def relabel_galaxy_core(mask, img):
    """
    For galaxy core images: bright central regions = galaxy_core (class 12).
    Uses intensity to identify core regions.
    """
    new_mask = mask.copy()

    # Convert image to grayscale if needed
    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    # Normalize
    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Find bright central region (galaxy core)
    h, w = gray.shape
    center_y, center_x = h // 2, w // 2

    # Create radial distance map
    y, x = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((x - center_x)**2 + (y - center_y)**2)
    max_dist = np.sqrt(center_x**2 + center_y**2)

    # Core is bright AND relatively central
    bright_threshold = np.percentile(gray, 80)
    core_mask = (gray > bright_threshold) & (dist_from_center < max_dist * 0.4)

    # Assign galaxy_core to bright central regions
    # Keep existing star classifications
    new_mask[(core_mask) & (mask == CLASS_BACKGROUND)] = CLASS_GALAXY_CORE
    new_mask[(core_mask) & (mask == CLASS_NEBULA_EMISSION)] = CLASS_GALAXY_CORE

    # Also mark slightly dimmer extended regions as galaxy spiral/structure
    extended_mask = (gray > np.percentile(gray, 50)) & (dist_from_center < max_dist * 0.7)
    new_mask[(extended_mask) & (new_mask == CLASS_BACKGROUND)] = CLASS_GALAXY_SPIRAL

    return new_mask


def relabel_dust_lane(mask, img):
    """
    For dust lane images (edge-on galaxies): dark absorption features = dust_lane (class 13).
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

    h, w = gray.shape

    # For edge-on galaxies, dust lanes are dark regions against brighter background
    # Find the median brightness
    median_val = np.median(gray)

    # Dark regions that are surrounded by brighter regions = dust lanes
    dark_threshold = np.percentile(gray, 30)

    # Use local contrast - dust lanes are darker than their surroundings
    from scipy import ndimage
    local_mean = ndimage.uniform_filter(gray, size=20)
    contrast = local_mean - gray

    # Dust lanes: significantly darker than local area AND not too dark (not background)
    dust_mask = (contrast > 0.05) & (gray > np.percentile(gray, 10)) & (gray < np.percentile(gray, 50))

    # Assign dust_lane class
    new_mask[(dust_mask) & ((mask == CLASS_BACKGROUND) | (mask == CLASS_NEBULA_DARK))] = CLASS_DUST_LANE

    # Bright regions in the galaxy
    bright_mask = gray > np.percentile(gray, 70)
    new_mask[(bright_mask) & (mask == CLASS_BACKGROUND)] = CLASS_GALAXY_SPIRAL

    return new_mask


def relabel_globular_cluster(mask, img):
    """
    For globular cluster images: dense central star region = star_cluster_globular (class 15).
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

    h, w = gray.shape
    center_y, center_x = h // 2, w // 2

    # Create radial distance map
    y, x = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((x - center_x)**2 + (y - center_y)**2)
    max_dist = np.sqrt(center_x**2 + center_y**2)

    # Globular clusters are dense central concentrations
    # Mark central bright region as globular cluster
    central_mask = dist_from_center < max_dist * 0.5
    bright_mask = gray > np.percentile(gray, 40)

    # Assign globular cluster to central bright region (except individual bright stars)
    cluster_region = central_mask & bright_mask

    # Keep individual bright stars, but mark the overall region
    new_mask[(cluster_region) & (mask == CLASS_BACKGROUND)] = CLASS_STAR_CLUSTER_GLOBULAR
    new_mask[(cluster_region) & (mask == CLASS_STAR_FAINT)] = CLASS_STAR_CLUSTER_GLOBULAR
    new_mask[(cluster_region) & (mask == CLASS_STAR_MEDIUM)] = CLASS_STAR_CLUSTER_GLOBULAR

    return new_mask


def relabel_planetary_nebula(mask, img):
    """
    For planetary nebula images: shell structure = nebula_planetary (class 8).
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

    h, w = gray.shape
    center_y, center_x = h // 2, w // 2

    # Create radial distance map
    y, x = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((x - center_x)**2 + (y - center_y)**2)
    max_dist = np.sqrt(center_x**2 + center_y**2)

    # Planetary nebulae have ring/shell structure
    # Any nebula emission within the central region
    nebula_region = dist_from_center < max_dist * 0.6
    bright_enough = gray > np.percentile(gray, 30)

    # Assign planetary_nebula
    pn_mask = nebula_region & bright_enough
    new_mask[(pn_mask) & ((mask == CLASS_BACKGROUND) | (mask == CLASS_NEBULA_EMISSION))] = CLASS_NEBULA_PLANETARY

    return new_mask


def relabel_star_cluster(mask, img):
    """
    For open star cluster images: loose grouping = star_cluster_open (class 14).
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

    # Open clusters are more spread out
    # Mark medium-bright stars as part of open cluster
    bright_mask = gray > np.percentile(gray, 50)

    # Stars in the region
    new_mask[(bright_mask) & ((mask == CLASS_STAR_FAINT) | (mask == CLASS_STAR_MEDIUM))] = CLASS_STAR_CLUSTER_OPEN
    new_mask[(bright_mask) & (mask == CLASS_BACKGROUND)] = CLASS_STAR_CLUSTER_OPEN

    return new_mask


def process_category(category_dir, relabel_func, description):
    """Process all images in a category directory with the given relabeling function."""

    mask_files = list(category_dir.rglob("*_mask.png"))
    if not mask_files:
        print(f"  No masks found in {category_dir.name}")
        return 0

    print(f"  Processing {len(mask_files)} masks for {description}...")

    processed = 0
    for mask_path in tqdm(mask_files, desc=f"  {category_dir.name}"):
        # Find corresponding image
        img_path = mask_path.with_name(mask_path.name.replace("_mask.png", "_img.png"))
        if not img_path.exists():
            continue

        # Load
        mask = np.array(Image.open(mask_path))
        img = np.array(Image.open(img_path))

        # Relabel
        new_mask = relabel_func(mask, img)

        # Save
        Image.fromarray(new_mask).save(mask_path)
        processed += 1

    return processed


def main():
    parser = argparse.ArgumentParser(description='Apply rule-based relabeling by category')
    parser.add_argument('--labeled-dir', default='/home/scarter4work/projects/NukeX/training_data/labeled',
                       help='Directory containing labeled data')
    args = parser.parse_args()

    labeled_dir = Path(args.labeled_dir)

    # Category to relabeling function mapping
    category_functions = {
        'galaxy_core': (relabel_galaxy_core, 'galaxy core regions'),
        'dust_lane': (relabel_dust_lane, 'dust lanes in edge-on galaxies'),
        'globular_cluster': (relabel_globular_cluster, 'globular cluster regions'),
        'planetary_nebula': (relabel_planetary_nebula, 'planetary nebula shells'),
        'star_clusters': (relabel_star_cluster, 'open star clusters'),
    }

    print("=" * 60)
    print("Rule-based Relabeling by Category")
    print("=" * 60)

    total_processed = 0
    for category, (func, desc) in category_functions.items():
        category_dir = labeled_dir / category
        if category_dir.exists():
            count = process_category(category_dir, func, desc)
            total_processed += count

    print(f"\nTotal masks relabeled: {total_processed}")


if __name__ == "__main__":
    main()
