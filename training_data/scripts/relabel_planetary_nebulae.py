#!/usr/bin/env python3
"""
Relabel planetary nebula images to assign correct nebula_planetary class (8).
Uses image characteristics to identify the shell/ring structure typical of PNe.
"""

import numpy as np
from pathlib import Path
from PIL import Image
from tqdm import tqdm
from scipy import ndimage

CLASS_BACKGROUND = 0
CLASS_STAR_BRIGHT = 1
CLASS_STAR_MEDIUM = 2
CLASS_STAR_FAINT = 3
CLASS_NEBULA_EMISSION = 5
CLASS_NEBULA_PLANETARY = 8


def relabel_planetary_nebula(mask, img):
    """
    Relabel as planetary nebula - shell/ring structure around central star.
    Planetary nebulae have:
    - Central star (often faint or obscured)
    - Shell/ring of emission
    - Often circular or elliptical symmetry
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

    # Find the nebula extent - where there's significant emission
    emission_thresh = np.percentile(gray, 60)
    bright_thresh = np.percentile(gray, 85)

    # Nebula shell: bright regions within central area
    nebula_region = dist_from_center < max_dist * 0.7
    emission_mask = (gray > emission_thresh) & nebula_region

    # Assign planetary_nebula to emission regions
    # Override nebula_emission and background classifications
    new_mask[(emission_mask) & ((new_mask == CLASS_BACKGROUND) |
                                 (new_mask == CLASS_NEBULA_EMISSION))] = CLASS_NEBULA_PLANETARY

    # Also include the bright core region as planetary nebula
    core_mask = (gray > bright_thresh) & (dist_from_center < max_dist * 0.5)
    new_mask[(core_mask) & (new_mask != CLASS_STAR_BRIGHT) &
             (new_mask != CLASS_STAR_MEDIUM)] = CLASS_NEBULA_PLANETARY

    # Keep bright point sources as stars but mark everything else as PN
    # Find local maxima (stars)
    local_max = ndimage.maximum_filter(gray, size=5)
    is_point_source = (gray == local_max) & (gray > np.percentile(gray, 95))

    # Don't relabel point sources that are likely the central star
    # They stay as their original star class

    return new_mask


def process_directory(data_dir):
    """Process all masks in a directory."""
    data_path = Path(data_dir)

    if not data_path.exists():
        print(f"Directory not found: {data_dir}")
        return 0

    mask_files = list(data_path.rglob("*_mask.png"))
    if not mask_files:
        print(f"No masks found in {data_dir}")
        return 0

    print(f"Processing {len(mask_files)} masks...")

    processed = 0
    for mask_path in tqdm(mask_files):
        img_path = mask_path.with_name(mask_path.name.replace("_mask.png", "_img.png"))
        if not img_path.exists():
            continue

        mask = np.array(Image.open(mask_path))
        img = np.array(Image.open(img_path))

        new_mask = relabel_planetary_nebula(mask, img)
        Image.fromarray(new_mask).save(mask_path)
        processed += 1

    return processed


def main():
    print("=" * 60)
    print("Planetary Nebula Relabeling")
    print("=" * 60)

    # Process both directories
    dirs = [
        '/home/scarter4work/projects/NukeX/training_data/labeled_pn',
        '/home/scarter4work/projects/NukeX/training_data/labeled_qnap_m27',
    ]

    total = 0
    for data_dir in dirs:
        print(f"\n{data_dir}")
        count = process_directory(data_dir)
        total += count
        print(f"  Relabeled: {count}")

    print(f"\nTotal relabeled: {total}")


if __name__ == "__main__":
    main()
