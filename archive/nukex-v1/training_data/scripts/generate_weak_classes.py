#!/usr/bin/env python3
"""
Generate synthetic training data for weak classes:
- dust_lane (class 13)
- galaxy_irregular (class 11)
"""

import numpy as np
from pathlib import Path
from PIL import Image
from scipy import ndimage
from tqdm import tqdm

# Class indices
CLASS_BACKGROUND = 0
CLASS_GALAXY_SPIRAL = 9
CLASS_GALAXY_IRREGULAR = 11
CLASS_GALAXY_CORE = 12
CLASS_DUST_LANE = 13

OUTPUT_DIR = Path("/home/scarter4work/projects/NukeX/training_data/synthetic_weak")


def generate_dust_lane_image(size=512):
    """Generate image with prominent dust lane across a galaxy-like structure."""
    img = np.zeros((size, size, 3), dtype=np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # Create galaxy background (elliptical brightness distribution)
    y, x = np.ogrid[:size, :size]
    cy, cx = size // 2, size // 2

    # Random rotation
    angle = np.random.uniform(0, 2 * np.pi)

    # Elliptical distance (stretched along one axis)
    stretch = np.random.uniform(1.5, 3.0)
    x_rot = (x - cx) * np.cos(angle) - (y - cy) * np.sin(angle)
    y_rot = (x - cx) * np.sin(angle) + (y - cy) * np.cos(angle)
    dist = np.sqrt((x_rot / stretch)**2 + y_rot**2)

    # Galaxy brightness profile
    scale = size / 4
    galaxy = np.exp(-dist / scale) * np.random.uniform(0.6, 0.9)

    # Add slight color variation (edge-on galaxies are typically yellowish)
    r_factor = np.random.uniform(1.0, 1.1)
    b_factor = np.random.uniform(0.8, 0.95)

    img[:, :, 0] = galaxy * r_factor
    img[:, :, 1] = galaxy
    img[:, :, 2] = galaxy * b_factor

    # Create dust lane as dark band across the galaxy
    # Dust lanes are typically along the major axis of edge-on galaxies
    lane_thickness = np.random.uniform(0.03, 0.08) * size

    # Distance from the major axis line
    lane_dist = np.abs(y_rot)

    # Dust lane mask (absorption)
    dust_strength = np.exp(-lane_dist**2 / (2 * lane_thickness**2))
    dust_factor = np.random.uniform(0.3, 0.6)  # How dark the dust lane is

    # Only apply dust where galaxy is bright
    galaxy_mask = galaxy > 0.1
    dust_visible = dust_strength * galaxy_mask

    # Apply absorption to image
    img *= (1 - dust_visible[:, :, np.newaxis] * (1 - dust_factor))

    # Add some noise and clutter
    noise = np.random.normal(0, 0.02, img.shape)
    img = np.clip(img + noise, 0, 1)

    # Create mask
    # Galaxy core
    core_mask = (dist < size * 0.08) & (galaxy > 0.5)
    mask[core_mask] = CLASS_GALAXY_CORE

    # Galaxy structure (spiral)
    spiral_mask = (galaxy > 0.15) & ~core_mask
    mask[spiral_mask] = CLASS_GALAXY_SPIRAL

    # Dust lane (where absorption is significant)
    dust_lane_mask = (dust_visible > 0.3) & galaxy_mask
    mask[dust_lane_mask] = CLASS_DUST_LANE

    return (img * 255).astype(np.uint8), mask


def generate_irregular_galaxy(size=512):
    """Generate irregular galaxy with asymmetric structure."""
    img = np.zeros((size, size, 3), dtype=np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # Multiple off-center blobs for irregular structure
    num_blobs = np.random.randint(3, 7)

    y, x = np.ogrid[:size, :size]

    for i in range(num_blobs):
        # Random center, not perfectly centered
        cx = size // 2 + np.random.randint(-size // 4, size // 4)
        cy = size // 2 + np.random.randint(-size // 4, size // 4)

        # Random size and brightness
        blob_size = np.random.uniform(size / 15, size / 6)
        brightness = np.random.uniform(0.3, 0.8)

        # Irregular shape (varying scales in x and y)
        scale_x = np.random.uniform(0.7, 1.3)
        scale_y = np.random.uniform(0.7, 1.3)

        dist = np.sqrt(((x - cx) * scale_x)**2 + ((y - cy) * scale_y)**2)
        blob = np.exp(-dist / blob_size) * brightness

        # Random color (irregular galaxies can be bluish from star formation)
        r_factor = np.random.uniform(0.8, 1.0)
        b_factor = np.random.uniform(0.9, 1.2)

        img[:, :, 0] = np.maximum(img[:, :, 0], blob * r_factor)
        img[:, :, 1] = np.maximum(img[:, :, 1], blob)
        img[:, :, 2] = np.maximum(img[:, :, 2], blob * b_factor)

    # Add some patchy structure
    patchy = ndimage.gaussian_filter(np.random.random((size, size)), sigma=size/20) * 0.3
    img *= (1 + patchy[:, :, np.newaxis])

    # Add noise
    noise = np.random.normal(0, 0.02, img.shape)
    img = np.clip(img + noise, 0, 1)

    # Create mask - all galaxy pixels are irregular type
    galaxy_brightness = np.max(img, axis=2)
    galaxy_mask = galaxy_brightness > 0.1
    mask[galaxy_mask] = CLASS_GALAXY_IRREGULAR

    # Brightest region could be marked as core
    bright_threshold = np.percentile(galaxy_brightness[galaxy_mask], 90) if galaxy_mask.any() else 0.5
    core_mask = galaxy_brightness > bright_threshold
    mask[core_mask] = CLASS_GALAXY_CORE

    return (img * 255).astype(np.uint8), mask


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    dust_dir = OUTPUT_DIR / "dust_lanes"
    irregular_dir = OUTPUT_DIR / "irregular_galaxies"
    dust_dir.mkdir(exist_ok=True)
    irregular_dir.mkdir(exist_ok=True)

    print("=" * 60)
    print("Generating Synthetic Data for Weak Classes")
    print("=" * 60)

    # Generate dust lane images
    n_dust = 300
    print(f"\nGenerating {n_dust} dust lane images...")
    dust_lane_total = 0
    for i in tqdm(range(n_dust), desc="Dust lanes"):
        img, mask = generate_dust_lane_image()
        dust_lane_total += np.sum(mask == CLASS_DUST_LANE)

        Image.fromarray(img).save(dust_dir / f"dust_lane_{i:04d}_img.png")
        Image.fromarray(mask).save(dust_dir / f"dust_lane_{i:04d}_mask.png")

    print(f"  Generated {dust_lane_total:,} dust lane pixels")

    # Generate irregular galaxy images
    n_irregular = 300
    print(f"\nGenerating {n_irregular} irregular galaxy images...")
    irregular_total = 0
    for i in tqdm(range(n_irregular), desc="Irregular galaxies"):
        img, mask = generate_irregular_galaxy()
        irregular_total += np.sum(mask == CLASS_GALAXY_IRREGULAR)

        Image.fromarray(img).save(irregular_dir / f"irregular_{i:04d}_img.png")
        Image.fromarray(mask).save(irregular_dir / f"irregular_{i:04d}_mask.png")

    print(f"  Generated {irregular_total:,} irregular galaxy pixels")

    print(f"\nTotal files: {(n_dust + n_irregular) * 2}")
    print(f"Saved to: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
