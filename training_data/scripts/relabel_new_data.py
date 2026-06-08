#!/usr/bin/env python3
"""
Relabel newly downloaded and labeled data with correct class assignments.

Handles:
- labeled_reflection → nebula_reflection (class 6)
- labeled_clusters/globular → star_cluster_globular (class 15)
- labeled_clusters/open → star_cluster_open (class 14)
- labeled_dustlanes → dust_lane (class 13)
"""

import numpy as np
from pathlib import Path
from PIL import Image
from tqdm import tqdm
from scipy import ndimage

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


def relabel_reflection_nebula(mask, img):
    """
    Relabel for reflection nebulae: diffuse bluish regions = nebula_reflection.
    """
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Reflection nebulae are diffuse, moderate brightness
    # Not as bright as emission nebulae, but brighter than background
    min_bright = np.percentile(gray, 15)
    max_bright = np.percentile(gray, 95)

    # Extended diffuse regions
    diffuse_mask = (gray > min_bright) & (gray < max_bright)

    # Mark diffuse regions as reflection nebula
    # Don't overwrite stars
    star_classes = [CLASS_STAR_BRIGHT, CLASS_STAR_MEDIUM, CLASS_STAR_FAINT, CLASS_STAR_SATURATED]
    not_star = ~np.isin(mask, star_classes)

    new_mask[diffuse_mask & not_star] = CLASS_NEBULA_REFLECTION

    return new_mask


def relabel_globular_cluster(mask, img):
    """
    Relabel for globular clusters: dense central star regions.
    """
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

    # Create radial distance map
    y, x = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((x - center_x)**2 + (y - center_y)**2)
    max_dist = np.sqrt(center_x**2 + center_y**2)

    # Globular clusters are dense central concentrations
    central_mask = dist_from_center < max_dist * 0.6
    bright_mask = gray > np.percentile(gray, 30)

    cluster_region = central_mask & bright_mask

    # Mark as globular cluster (except keep bright/saturated stars)
    keep_star_classes = [CLASS_STAR_BRIGHT, CLASS_STAR_SATURATED]
    not_bright_star = ~np.isin(mask, keep_star_classes)

    new_mask[cluster_region & not_bright_star] = CLASS_STAR_CLUSTER_GLOBULAR

    return new_mask


def relabel_open_cluster(mask, img):
    """
    Relabel for open clusters: looser grouping of stars.
    """
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Open clusters are more spread out
    bright_mask = gray > np.percentile(gray, 40)

    # Mark fainter stars and background in bright regions as open cluster
    star_classes = [CLASS_STAR_FAINT, CLASS_STAR_MEDIUM]
    new_mask[bright_mask & np.isin(mask, star_classes)] = CLASS_STAR_CLUSTER_OPEN
    new_mask[bright_mask & (mask == CLASS_BACKGROUND)] = CLASS_STAR_CLUSTER_OPEN

    return new_mask


def relabel_dust_lane(mask, img):
    """
    Relabel for dust lanes in edge-on galaxies.
    """
    new_mask = mask.copy()

    if len(img.shape) == 3:
        gray = np.mean(img, axis=2)
    else:
        gray = img.copy()

    gray = gray.astype(np.float32)
    if gray.max() > 1:
        gray = gray / 255.0

    # Dust lanes are dark absorption features against brighter galaxy background
    local_mean = ndimage.uniform_filter(gray, size=20)
    contrast = local_mean - gray

    # Dust lanes: darker than local area, but not background
    dust_mask = (contrast > 0.03) & (gray > np.percentile(gray, 5)) & (gray < np.percentile(gray, 60))

    # Assign dust_lane class
    new_mask[dust_mask & ((mask == CLASS_BACKGROUND) | (mask == CLASS_NEBULA_DARK))] = CLASS_DUST_LANE

    # Bright regions = galaxy structure
    bright_mask = gray > np.percentile(gray, 65)
    new_mask[bright_mask & (mask == CLASS_BACKGROUND)] = CLASS_GALAXY_SPIRAL

    # Galaxy core for very bright central regions
    h, w = gray.shape
    center_y, center_x = h // 2, w // 2
    y, x = np.ogrid[:h, :w]
    dist_from_center = np.sqrt((x - center_x)**2 + (y - center_y)**2)
    max_dist = np.sqrt(center_x**2 + center_y**2)

    core_mask = (gray > np.percentile(gray, 90)) & (dist_from_center < max_dist * 0.3)
    new_mask[core_mask & (new_mask != CLASS_STAR_BRIGHT) & (new_mask != CLASS_STAR_SATURATED)] = CLASS_GALAXY_CORE

    return new_mask


def process_directory(dir_path, relabel_func, description):
    """Process all masks in a directory."""
    mask_files = list(dir_path.rglob("*_mask.png"))
    if not mask_files:
        print(f"  No masks found in {dir_path}")
        return 0

    print(f"  Processing {len(mask_files)} masks for {description}...")

    processed = 0
    for mask_path in tqdm(mask_files, desc=f"  {description}"):
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
    base_dir = Path("/home/scarter4work/projects/NukeX/training_data")

    print("=" * 60)
    print("Relabeling New Data")
    print("=" * 60)

    total = 0

    # Reflection nebulae
    reflection_dir = base_dir / "labeled_reflection"
    if reflection_dir.exists():
        count = process_directory(reflection_dir, relabel_reflection_nebula, "reflection nebulae")
        total += count
        print(f"  Relabeled {count} reflection nebula masks")

    # Globular clusters
    globular_dir = base_dir / "labeled_clusters" / "globular"
    if globular_dir.exists():
        count = process_directory(globular_dir, relabel_globular_cluster, "globular clusters")
        total += count
        print(f"  Relabeled {count} globular cluster masks")

    # Open clusters
    open_dir = base_dir / "labeled_clusters" / "open"
    if open_dir.exists():
        count = process_directory(open_dir, relabel_open_cluster, "open clusters")
        total += count
        print(f"  Relabeled {count} open cluster masks")

    # Dust lanes
    dustlanes_dir = base_dir / "labeled_dustlanes"
    if dustlanes_dir.exists():
        count = process_directory(dustlanes_dir, relabel_dust_lane, "dust lanes")
        total += count
        print(f"  Relabeled {count} dust lane masks")

    print(f"\nTotal masks relabeled: {total}")


if __name__ == "__main__":
    main()
