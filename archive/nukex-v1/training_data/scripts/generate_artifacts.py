#!/usr/bin/env python3
"""
Generate synthetic hot pixels and diffraction spikes for training data.

Hot pixels: Random single bright pixels (sensor defects)
Diffraction spikes: Cross patterns around bright stars (telescope optics)
"""

import numpy as np
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter
import argparse
from tqdm import tqdm
import random

# Class indices
CLASS_BACKGROUND = 0
CLASS_STAR_BRIGHT = 1
CLASS_ARTIFACT_HOT_PIXEL = 16
CLASS_ARTIFACT_DIFFRACTION = 18


def add_hot_pixels(img, mask, num_pixels=None, intensity_range=(0.8, 1.0)):
    """
    Add random hot pixels to an image.

    Hot pixels are single bright pixels caused by sensor defects.
    They appear at random locations with high intensity.
    """
    h, w = img.shape[:2]

    if num_pixels is None:
        # Random number of hot pixels (5-50 per image)
        num_pixels = random.randint(5, 50)

    img_out = img.copy()
    mask_out = mask.copy()

    for _ in range(num_pixels):
        x = random.randint(0, w - 1)
        y = random.randint(0, h - 1)

        # Random bright intensity
        intensity = random.uniform(*intensity_range)

        # Set pixel to bright value (affects all channels if RGB)
        if len(img_out.shape) == 3:
            img_out[y, x, :] = intensity
        else:
            img_out[y, x] = intensity

        # Mark in mask
        mask_out[y, x] = CLASS_ARTIFACT_HOT_PIXEL

    return img_out, mask_out


def add_diffraction_spikes(img, mask, num_stars=None, spike_params=None):
    """
    Add diffraction spikes around bright point sources.

    Diffraction spikes are caused by spider vanes in reflecting telescopes.
    Typically 4 spikes (Newtonian) or 6 spikes (some designs).
    """
    h, w = img.shape[:2]

    if num_stars is None:
        num_stars = random.randint(2, 8)

    if spike_params is None:
        spike_params = {
            'num_spikes': random.choice([4, 6, 8]),  # Number of spikes
            'length_range': (20, 80),  # Spike length in pixels
            'width_range': (1, 3),  # Spike width
            'intensity_range': (0.6, 1.0),  # Core intensity
            'falloff': 0.7,  # How quickly spikes fade
        }

    img_out = img.copy().astype(np.float32)
    mask_out = mask.copy()

    for _ in range(num_stars):
        # Random position (avoid edges)
        margin = spike_params['length_range'][1]
        cx = random.randint(margin, w - margin)
        cy = random.randint(margin, h - margin)

        # Random parameters for this star
        num_spikes = spike_params['num_spikes']
        length = random.randint(*spike_params['length_range'])
        width = random.randint(*spike_params['width_range'])
        core_intensity = random.uniform(*spike_params['intensity_range'])
        falloff = spike_params['falloff']

        # Random rotation offset
        angle_offset = random.uniform(0, np.pi / num_spikes)

        # Draw spikes
        for i in range(num_spikes):
            angle = angle_offset + (2 * np.pi * i / num_spikes)

            # Draw spike line with falloff
            for dist in range(1, length):
                # Intensity falls off with distance
                intensity = core_intensity * (falloff ** (dist / 10))

                # Calculate position along spike
                dx = int(dist * np.cos(angle))
                dy = int(dist * np.sin(angle))

                x, y = cx + dx, cy + dy

                if 0 <= x < w and 0 <= y < h:
                    # Draw with width
                    for wx in range(-width//2, width//2 + 1):
                        for wy in range(-width//2, width//2 + 1):
                            px, py = x + wx, y + wy
                            if 0 <= px < w and 0 <= py < h:
                                # Additive blending
                                if len(img_out.shape) == 3:
                                    img_out[py, px, :] = np.minimum(1.0, img_out[py, px, :] + intensity * 0.3)
                                else:
                                    img_out[py, px] = min(1.0, img_out[py, px] + intensity * 0.3)

                                # Mark in mask (only if not already a brighter class)
                                if mask_out[py, px] == CLASS_BACKGROUND:
                                    mask_out[py, px] = CLASS_ARTIFACT_DIFFRACTION

        # Draw bright core
        core_radius = max(2, width)
        for dx in range(-core_radius, core_radius + 1):
            for dy in range(-core_radius, core_radius + 1):
                if dx*dx + dy*dy <= core_radius*core_radius:
                    px, py = cx + dx, cy + dy
                    if 0 <= px < w and 0 <= py < h:
                        if len(img_out.shape) == 3:
                            img_out[py, px, :] = core_intensity
                        else:
                            img_out[py, px] = core_intensity
                        mask_out[py, px] = CLASS_STAR_BRIGHT

    return np.clip(img_out, 0, 1), mask_out


def generate_base_image(size=512):
    """Generate a realistic-looking astronomical background."""
    # Start with dark background with slight gradient
    img = np.random.normal(0.05, 0.02, (size, size, 3)).astype(np.float32)

    # Add some nebulosity (random gaussian blobs)
    for _ in range(random.randint(0, 3)):
        cx, cy = random.randint(0, size), random.randint(0, size)
        sigma = random.randint(50, 150)
        intensity = random.uniform(0.05, 0.15)

        y, x = np.ogrid[:size, :size]
        blob = np.exp(-((x - cx)**2 + (y - cy)**2) / (2 * sigma**2))

        # Random color tint
        color = np.array([random.uniform(0.8, 1.2),
                         random.uniform(0.8, 1.2),
                         random.uniform(0.8, 1.2)])

        for c in range(3):
            img[:, :, c] += blob * intensity * color[c]

    # Add some faint background stars
    num_bg_stars = random.randint(50, 200)
    for _ in range(num_bg_stars):
        x, y = random.randint(0, size-1), random.randint(0, size-1)
        brightness = random.uniform(0.1, 0.4)
        img[y, x, :] = brightness

    return np.clip(img, 0, 1)


def generate_synthetic_artifacts(output_dir, num_images=500, size=512):
    """Generate synthetic images with hot pixels and diffraction spikes."""

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    print(f"Generating {num_images} synthetic artifact images...")

    for i in tqdm(range(num_images)):
        # Generate base image
        img = generate_base_image(size)
        mask = np.zeros((size, size), dtype=np.uint8)

        # Randomly decide what artifacts to add
        add_hot = random.random() < 0.7  # 70% have hot pixels
        add_diff = random.random() < 0.7  # 70% have diffraction spikes

        if add_hot:
            img, mask = add_hot_pixels(img, mask)

        if add_diff:
            img, mask = add_diffraction_spikes(img, mask)

        # Save image and mask
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_path / f"synthetic_{i:05d}_img.png")
        Image.fromarray(mask).save(output_path / f"synthetic_{i:05d}_mask.png")

    print(f"Generated {num_images} images in {output_path}")

    # Count artifact pixels
    total_hot = 0
    total_diff = 0

    for mask_path in output_path.glob("*_mask.png"):
        mask = np.array(Image.open(mask_path))
        total_hot += np.sum(mask == CLASS_ARTIFACT_HOT_PIXEL)
        total_diff += np.sum(mask == CLASS_ARTIFACT_DIFFRACTION)

    print(f"\nArtifact pixel counts:")
    print(f"  Hot pixels: {total_hot:,}")
    print(f"  Diffraction spikes: {total_diff:,}")


def augment_existing_images(input_dir, output_dir, max_images=200):
    """Add artifacts to existing training images."""

    input_path = Path(input_dir)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    # Find existing image-mask pairs
    img_files = list(input_path.rglob("*_img.png"))[:max_images]

    print(f"Augmenting {len(img_files)} existing images with artifacts...")

    for img_path in tqdm(img_files):
        mask_path = img_path.with_name(img_path.name.replace("_img.png", "_mask.png"))

        if not mask_path.exists():
            continue

        # Load
        img = np.array(Image.open(img_path)).astype(np.float32) / 255.0
        mask = np.array(Image.open(mask_path))

        # Add artifacts
        if random.random() < 0.5:
            img, mask = add_hot_pixels(img, mask, num_pixels=random.randint(10, 30))

        if random.random() < 0.5:
            img, mask = add_diffraction_spikes(img, mask, num_stars=random.randint(1, 4))

        # Save
        out_name = img_path.stem.replace("_img", "")
        img_uint8 = (img * 255).astype(np.uint8)
        Image.fromarray(img_uint8).save(output_path / f"{out_name}_augmented_img.png")
        Image.fromarray(mask).save(output_path / f"{out_name}_augmented_mask.png")

    print(f"Augmented images saved to {output_path}")


def main():
    parser = argparse.ArgumentParser(description='Generate synthetic artifacts')
    parser.add_argument('--output', required=True, help='Output directory')
    parser.add_argument('--num-images', type=int, default=500, help='Number of synthetic images')
    parser.add_argument('--size', type=int, default=512, help='Image size')
    parser.add_argument('--augment-from', help='Augment existing images from this directory')
    parser.add_argument('--augment-max', type=int, default=200, help='Max images to augment')
    args = parser.parse_args()

    if args.augment_from:
        augment_existing_images(args.augment_from, args.output, args.augment_max)
    else:
        generate_synthetic_artifacts(args.output, args.num_images, args.size)


if __name__ == "__main__":
    main()
