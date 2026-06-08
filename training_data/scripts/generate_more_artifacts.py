#!/usr/bin/env python3
"""
Generate more synthetic artifacts focused on hot pixels, satellites, and noise.
"""

import numpy as np
from pathlib import Path
from PIL import Image
import random
from tqdm import tqdm

CLASS_BACKGROUND = 0
CLASS_ARTIFACT_HOT_PIXEL = 16
CLASS_ARTIFACT_SATELLITE = 17
CLASS_ARTIFACT_NOISE = 20


def generate_hot_pixel_heavy(size=512):
    """Generate image with MANY hot pixels."""
    # Dark background
    img = np.random.normal(0.05, 0.01, (size, size, 3)).astype(np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # Add MANY hot pixels (100-500 per image)
    num_pixels = random.randint(100, 500)

    for _ in range(num_pixels):
        x = random.randint(0, size - 1)
        y = random.randint(0, size - 1)
        intensity = random.uniform(0.7, 1.0)
        img[y, x, :] = intensity
        mask[y, x] = CLASS_ARTIFACT_HOT_PIXEL

        # Sometimes add small clusters (2-4 pixels)
        if random.random() < 0.3:
            for dx, dy in [(1, 0), (0, 1), (-1, 0), (0, -1)]:
                px, py = x + dx, y + dy
                if 0 <= px < size and 0 <= py < size and random.random() < 0.5:
                    img[py, px, :] = intensity * 0.8
                    mask[py, px] = CLASS_ARTIFACT_HOT_PIXEL

    return np.clip(img, 0, 1), mask


def generate_satellite_trail(size=512):
    """Generate image with satellite trails."""
    # Dark background with some stars
    img = np.random.normal(0.05, 0.01, (size, size, 3)).astype(np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # Add background stars
    num_stars = random.randint(20, 50)
    for _ in range(num_stars):
        x, y = random.randint(0, size-1), random.randint(0, size-1)
        img[y, x, :] = random.uniform(0.2, 0.5)

    # Add 1-3 satellite trails
    num_trails = random.randint(1, 3)

    for _ in range(num_trails):
        # Random line across image
        x1 = random.randint(0, size - 1)
        y1 = random.randint(0, size - 1)

        # Random angle
        angle = random.uniform(0, 2 * np.pi)
        length = random.randint(size // 2, size)

        x2 = int(x1 + length * np.cos(angle))
        y2 = int(y1 + length * np.sin(angle))

        # Draw line
        trail_width = random.randint(1, 3)
        intensity = random.uniform(0.6, 1.0)

        # Bresenham-like line drawing
        dx = abs(x2 - x1)
        dy = abs(y2 - y1)
        steps = max(dx, dy, 1)

        for i in range(steps):
            t = i / steps
            x = int(x1 + t * (x2 - x1))
            y = int(y1 + t * (y2 - y1))

            # Add width
            for wx in range(-trail_width // 2, trail_width // 2 + 1):
                for wy in range(-trail_width // 2, trail_width // 2 + 1):
                    px, py = x + wx, y + wy
                    if 0 <= px < size and 0 <= py < size:
                        # Slight intensity variation along trail
                        local_intensity = intensity * (0.8 + 0.2 * random.random())
                        img[py, px, :] = np.maximum(img[py, px, :], local_intensity)
                        mask[py, px] = CLASS_ARTIFACT_SATELLITE

    return np.clip(img, 0, 1), mask


def generate_noisy_image(size=512):
    """Generate image with noise patterns."""
    # Higher noise background
    img = np.random.normal(0.1, 0.05, (size, size, 3)).astype(np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # Add noise regions
    num_regions = random.randint(3, 8)

    for _ in range(num_regions):
        # Random elliptical region
        cx, cy = random.randint(0, size), random.randint(0, size)
        rx, ry = random.randint(30, 100), random.randint(30, 100)

        y, x = np.ogrid[:size, :size]
        region = ((x - cx)**2 / max(rx**2, 1) + (y - cy)**2 / max(ry**2, 1)) < 1

        # Add strong noise to this region
        noise_level = random.uniform(0.1, 0.2)
        img[region] += np.random.normal(0, noise_level, (np.sum(region), 3)).astype(np.float32)
        mask[region] = CLASS_ARTIFACT_NOISE

    return np.clip(img, 0, 1), mask


def main():
    output_dir = Path("/home/scarter4work/projects/NukeX/training_data/synthetic_artifacts")
    output_dir.mkdir(parents=True, exist_ok=True)

    print("Generating focused artifact images...")

    # Generate hot pixel images (500)
    print("\nGenerating hot pixel images...")
    for i in tqdm(range(500), desc="Hot pixels"):
        img, mask = generate_hot_pixel_heavy()
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_dir / f"hotpixel_{i:05d}_img.png")
        Image.fromarray(mask).save(output_dir / f"hotpixel_{i:05d}_mask.png")

    # Generate satellite trail images (300)
    print("\nGenerating satellite trail images...")
    for i in tqdm(range(300), desc="Satellites"):
        img, mask = generate_satellite_trail()
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_dir / f"satellite_{i:05d}_img.png")
        Image.fromarray(mask).save(output_dir / f"satellite_{i:05d}_mask.png")

    # Generate noisy images (200)
    print("\nGenerating noise images...")
    for i in tqdm(range(200), desc="Noise"):
        img, mask = generate_noisy_image()
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_dir / f"noise_{i:05d}_img.png")
        Image.fromarray(mask).save(output_dir / f"noise_{i:05d}_mask.png")

    # Count pixels
    print("\nCounting artifact pixels...")
    total_hot = 0
    total_sat = 0
    total_noise = 0

    for mask_path in output_dir.glob("*_mask.png"):
        mask = np.array(Image.open(mask_path))
        total_hot += np.sum(mask == CLASS_ARTIFACT_HOT_PIXEL)
        total_sat += np.sum(mask == CLASS_ARTIFACT_SATELLITE)
        total_noise += np.sum(mask == CLASS_ARTIFACT_NOISE)

    print(f"\nArtifact pixel counts:")
    print(f"  Hot pixels: {total_hot:,}")
    print(f"  Satellite trails: {total_sat:,}")
    print(f"  Noise: {total_noise:,}")


if __name__ == "__main__":
    main()
