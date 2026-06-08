#!/usr/bin/env python3
"""
Check for corrupt QNAP composite images (B channel = 0) and create v7 manifest excluding them.
"""

import json
import numpy as np
from PIL import Image
from pathlib import Path
from tqdm import tqdm
import os

NUM_CLASSES = 21
CLASS_NAMES = [
    'background', 'star_bright', 'star_medium', 'star_faint', 'star_saturated',
    'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
    'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
    'dust_lane', 'star_cluster_open', 'star_cluster_globular',
    'artifact_hot_pixel', 'artifact_satellite', 'artifact_diffraction',
    'artifact_gradient', 'artifact_noise'
]

def check_corrupt_blue_channel(image_path):
    """Check if an image has a corrupt (all zero) blue channel."""
    try:
        img = Image.open(image_path)
        if img.mode != 'RGB':
            img = img.convert('RGB')
        arr = np.array(img)
        blue_channel = arr[:, :, 2]

        # Check if blue channel is all zeros or nearly all zeros
        blue_sum = blue_channel.sum()
        blue_max = blue_channel.max()

        return blue_sum == 0 or blue_max == 0
    except Exception as e:
        print(f"Error reading {image_path}: {e}")
        return True  # Consider unreadable files as corrupt


def calculate_class_weights(pairs, sample_size=1000):
    """Calculate class weights from masks."""
    print(f"Calculating class weights from {min(sample_size, len(pairs))} samples...")

    # Sample if dataset is large
    if len(pairs) > sample_size:
        indices = np.random.choice(len(pairs), sample_size, replace=False)
        sampled_pairs = [pairs[i] for i in indices]
    else:
        sampled_pairs = pairs

    class_counts = np.zeros(NUM_CLASSES, dtype=np.int64)

    for pair in tqdm(sampled_pairs, desc="Counting classes"):
        try:
            mask = np.array(Image.open(pair['mask']))
            for c in range(NUM_CLASSES):
                class_counts[c] += np.sum(mask == c)
        except Exception as e:
            print(f"Error reading {pair['mask']}: {e}")

    print("\nClass distribution:")
    total = class_counts.sum()
    for i, name in enumerate(CLASS_NAMES):
        pct = 100 * class_counts[i] / total if total > 0 else 0
        print(f"  {name}: {class_counts[i]:,} ({pct:.2f}%)")

    # Calculate weights - inverse frequency with smoothing
    weights = np.zeros(NUM_CLASSES)
    for c in range(NUM_CLASSES):
        if class_counts[c] > 0:
            weights[c] = total / (NUM_CLASSES * class_counts[c])
        else:
            weights[c] = 1.0

    # Cap extreme weights
    weights = np.clip(weights, 0.1, 10.0)

    # Normalize
    weights = weights / weights.mean()

    print("\nClass weights:")
    for i, name in enumerate(CLASS_NAMES):
        print(f"  {name}: {weights[i]:.4f}")

    return weights


def main():
    qnap_source_dir = Path('/home/scarter4work/projects/NukeX/training_data/rgb_sources/QNAP_composites')
    v6_manifest_path = '/home/scarter4work/projects/NukeX/training_data/unified_manifest_v6.json'
    v7_manifest_path = '/home/scarter4work/projects/NukeX/training_data/unified_manifest_v7.json'
    weights_path = '/home/scarter4work/projects/NukeX/training_data/class_weights_v7.npy'

    # Step 1: Check all QNAP composite source images for corrupt blue channels
    print("=" * 60)
    print("Step 1: Checking QNAP composite images for corrupt blue channels")
    print("=" * 60)

    corrupt_images = []
    good_images = []

    for img_path in sorted(qnap_source_dir.glob('*.png')):
        is_corrupt = check_corrupt_blue_channel(img_path)
        if is_corrupt:
            corrupt_images.append(img_path.name)
            print(f"  CORRUPT: {img_path.name}")
        else:
            good_images.append(img_path.name)

    print(f"\nFound {len(corrupt_images)} corrupt images, {len(good_images)} good images")

    if corrupt_images:
        print("\nCorrupt images (B=0):")
        for name in corrupt_images:
            print(f"  - {name}")

    # Step 2: Load v6 manifest and filter out corrupt QNAP pairs
    print("\n" + "=" * 60)
    print("Step 2: Creating v7 manifest excluding corrupt QNAP pairs")
    print("=" * 60)

    print(f"Loading v6 manifest from: {v6_manifest_path}")
    with open(v6_manifest_path) as f:
        v6_data = json.load(f)

    v6_pairs = v6_data['pairs']
    print(f"  V6 manifest has {len(v6_pairs)} pairs")

    # Filter out pairs from labeled_rgb/QNAP_composites that correspond to corrupt source images
    # The labeled pairs have paths like: .../labeled_rgb/QNAP_composites/<name>_img.png
    # We need to match the base name (before _img) to the corrupt source images

    v7_pairs = []
    excluded_count = 0

    for pair in v6_pairs:
        # Check if this pair is from QNAP_composites
        if 'QNAP_composites' in pair['image']:
            # Extract the base image name from the labeled path
            # e.g., ".../labeled_rgb/QNAP_composites/10_23_2025_M78_RGB_img.png"
            # -> "10_23_2025_M78_RGB.png"
            img_name = Path(pair['image']).name
            # Remove _img suffix to get base name
            if img_name.endswith('_img.png'):
                base_name = img_name.replace('_img.png', '.png')
            else:
                base_name = img_name

            if base_name in corrupt_images:
                excluded_count += 1
                print(f"  Excluding: {img_name} (source: {base_name})")
                continue  # Skip this pair

        v7_pairs.append(pair)

    print(f"  Excluded {excluded_count} pairs from corrupt QNAP sources")
    print(f"  V7 manifest will have {len(v7_pairs)} pairs")

    # Save v7 manifest
    v7_data = {'pairs': v7_pairs}
    with open(v7_manifest_path, 'w') as f:
        json.dump(v7_data, f, indent=2)
    print(f"  Saved v7 manifest to: {v7_manifest_path}")

    # Step 3: Calculate class weights for v7
    print("\n" + "=" * 60)
    print("Step 3: Calculating class weights for v7")
    print("=" * 60)

    weights = calculate_class_weights(v7_pairs)
    np.save(weights_path, weights)
    print(f"\nSaved class weights to: {weights_path}")

    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Corrupt QNAP images found: {len(corrupt_images)}")
    print(f"Pairs excluded: {excluded_count}")
    print(f"V6 pairs: {len(v6_pairs)}")
    print(f"V7 pairs: {len(v7_pairs)}")
    print(f"Manifest: {v7_manifest_path}")
    print(f"Weights: {weights_path}")


if __name__ == "__main__":
    main()
