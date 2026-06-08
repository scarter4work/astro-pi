#!/usr/bin/env python3
"""
Calculate class weights from unified training manifest.
Weights are inverse frequency to handle class imbalance.
"""

import json
import numpy as np
from PIL import Image
from pathlib import Path
from tqdm import tqdm
from collections import Counter

NUM_CLASSES = 21
CLASS_NAMES = [
    'background', 'star_bright', 'star_medium', 'star_faint', 'star_saturated',
    'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
    'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
    'dust_lane', 'star_cluster_open', 'star_cluster_globular',
    'artifact_hot_pixel', 'artifact_satellite', 'artifact_diffraction',
    'artifact_gradient', 'artifact_noise'
]


def main():
    manifest_path = Path('/home/scarter4work/projects/NukeX/training_data/unified_manifest.json')
    output_path = Path('/home/scarter4work/projects/NukeX/training_data/class_weights_unified.npy')

    print(f"Loading manifest: {manifest_path}")
    with open(manifest_path) as f:
        manifest = json.load(f)

    pairs = manifest['pairs']
    print(f"Processing {len(pairs)} mask files...")

    # Count pixels per class
    class_counts = Counter()
    total_pixels = 0

    for pair in tqdm(pairs):
        mask = np.array(Image.open(pair['mask']))
        unique, counts = np.unique(mask, return_counts=True)
        for cls, cnt in zip(unique, counts):
            if cls < NUM_CLASSES:
                class_counts[cls] += cnt
                total_pixels += cnt

    print(f"\nTotal pixels: {total_pixels:,}")
    print("\nClass distribution:")
    print("-" * 60)

    for i in range(NUM_CLASSES):
        count = class_counts.get(i, 0)
        pct = 100 * count / total_pixels if total_pixels > 0 else 0
        print(f"  {i:2d}. {CLASS_NAMES[i]:25s}: {count:12,} ({pct:6.2f}%)")

    # Calculate weights (inverse frequency with smoothing)
    print("\nCalculating class weights...")

    weights = np.zeros(NUM_CLASSES, dtype=np.float32)
    min_count = 100  # Minimum count to avoid extreme weights

    for i in range(NUM_CLASSES):
        count = max(class_counts.get(i, 0), min_count)
        weights[i] = total_pixels / (NUM_CLASSES * count)

    # Normalize weights (median = 1.0)
    median_weight = np.median(weights)
    weights = weights / median_weight

    # Clip extreme weights
    weights = np.clip(weights, 0.1, 10.0)

    print("\nClass weights:")
    print("-" * 60)
    for i in range(NUM_CLASSES):
        print(f"  {i:2d}. {CLASS_NAMES[i]:25s}: {weights[i]:.4f}")

    # Save weights
    np.save(output_path, weights)
    print(f"\nWeights saved to: {output_path}")

    # Also save to the original location for compatibility
    original_path = Path('/home/scarter4work/projects/NukeX/training_data/class_weights.npy')
    np.save(original_path, weights)
    print(f"Also saved to: {original_path}")


if __name__ == '__main__':
    main()
