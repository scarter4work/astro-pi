#!/usr/bin/env python3
"""
Generate MASSIVE amounts of synthetic hot pixels and satellite trails.
Target: Get these classes to 0.2%+ of total pixels.
"""

import numpy as np
from pathlib import Path
from PIL import Image
import random
from tqdm import tqdm

CLASS_BACKGROUND = 0
CLASS_ARTIFACT_HOT_PIXEL = 16
CLASS_ARTIFACT_SATELLITE = 17


def generate_hot_pixel_dense(size=512):
    """Generate image with DENSE hot pixels (500-2000 per image)."""
    img = np.random.normal(0.03, 0.01, (size, size, 3)).astype(np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # MANY hot pixels
    num_pixels = random.randint(500, 2000)

    for _ in range(num_pixels):
        x = random.randint(0, size - 1)
        y = random.randint(0, size - 1)
        intensity = random.uniform(0.6, 1.0)
        img[y, x, :] = intensity
        mask[y, x] = CLASS_ARTIFACT_HOT_PIXEL

        # Hot pixel clusters (common in real sensors)
        if random.random() < 0.4:
            for dx, dy in [(1, 0), (0, 1), (-1, 0), (0, -1), (1, 1), (-1, -1)]:
                px, py = x + dx, y + dy
                if 0 <= px < size and 0 <= py < size and random.random() < 0.6:
                    img[py, px, :] = intensity * random.uniform(0.7, 1.0)
                    mask[py, px] = CLASS_ARTIFACT_HOT_PIXEL

    return np.clip(img, 0, 1), mask


def generate_satellite_multiple(size=512):
    """Generate image with MULTIPLE satellite trails (3-8 trails)."""
    img = np.random.normal(0.03, 0.01, (size, size, 3)).astype(np.float32)
    mask = np.zeros((size, size), dtype=np.uint8)

    # Add some stars
    for _ in range(random.randint(30, 80)):
        x, y = random.randint(0, size-1), random.randint(0, size-1)
        img[y, x, :] = random.uniform(0.15, 0.4)

    # Multiple trails
    num_trails = random.randint(3, 8)

    for _ in range(num_trails):
        x1 = random.randint(-size//4, size + size//4)
        y1 = random.randint(-size//4, size + size//4)
        angle = random.uniform(0, 2 * np.pi)
        length = random.randint(size, int(size * 1.5))

        x2 = int(x1 + length * np.cos(angle))
        y2 = int(y1 + length * np.sin(angle))

        trail_width = random.randint(1, 4)
        intensity = random.uniform(0.5, 1.0)

        dx = abs(x2 - x1)
        dy = abs(y2 - y1)
        steps = max(dx, dy, 1)

        for i in range(steps):
            t = i / steps
            x = int(x1 + t * (x2 - x1))
            y = int(y1 + t * (y2 - y1))

            for wx in range(-trail_width // 2, trail_width // 2 + 1):
                for wy in range(-trail_width // 2, trail_width // 2 + 1):
                    px, py = x + wx, y + wy
                    if 0 <= px < size and 0 <= py < size:
                        local_int = intensity * (0.85 + 0.15 * random.random())
                        img[py, px, :] = np.maximum(img[py, px, :], local_int)
                        mask[py, px] = CLASS_ARTIFACT_SATELLITE

    return np.clip(img, 0, 1), mask


def main():
    output_dir = Path("/home/scarter4work/projects/NukeX/training_data/synthetic_massive")
    output_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("Generating MASSIVE synthetic artifacts")
    print("=" * 60)

    # Generate 1000 dense hot pixel images
    print("\nGenerating 1000 dense hot pixel images...")
    for i in tqdm(range(1000), desc="Hot pixels"):
        img, mask = generate_hot_pixel_dense()
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_dir / f"hotpixel_dense_{i:05d}_img.png")
        Image.fromarray(mask).save(output_dir / f"hotpixel_dense_{i:05d}_mask.png")

    # Generate 500 multi-satellite images
    print("\nGenerating 500 multi-satellite images...")
    for i in tqdm(range(500), desc="Satellites"):
        img, mask = generate_satellite_multiple()
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_dir / f"satellite_multi_{i:05d}_img.png")
        Image.fromarray(mask).save(output_dir / f"satellite_multi_{i:05d}_mask.png")

    # Count pixels
    print("\nCounting pixels...")
    total_hot = 0
    total_sat = 0

    for mask_path in tqdm(list(output_dir.glob("*_mask.png")), desc="Counting"):
        mask = np.array(Image.open(mask_path))
        total_hot += np.sum(mask == CLASS_ARTIFACT_HOT_PIXEL)
        total_sat += np.sum(mask == CLASS_ARTIFACT_SATELLITE)

    print(f"\nHot pixel pixels: {total_hot:,}")
    print(f"Satellite pixels: {total_sat:,}")


if __name__ == "__main__":
    main()
