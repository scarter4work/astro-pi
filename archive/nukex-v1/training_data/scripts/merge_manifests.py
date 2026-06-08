#!/usr/bin/env python3
"""
Merge existing unified manifest with RGB manifest and recalculate class weights.
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
    # Load manifests
    unified_path = '/home/scarter4work/projects/NukeX/training_data/unified_manifest.json'
    rgb_path = '/home/scarter4work/projects/NukeX/training_data/labeled_rgb/rgb_manifest.json'
    output_path = '/home/scarter4work/projects/NukeX/training_data/unified_manifest_v6.json'
    weights_path = '/home/scarter4work/projects/NukeX/training_data/class_weights_v6.npy'

    print("Loading manifests...")

    with open(unified_path) as f:
        unified = json.load(f)
    print(f"  Unified manifest: {len(unified['pairs'])} pairs")

    with open(rgb_path) as f:
        rgb = json.load(f)
    print(f"  RGB manifest: {len(rgb['pairs'])} pairs")

    # Merge
    all_pairs = unified['pairs'] + rgb['pairs']
    print(f"\nMerged: {len(all_pairs)} total pairs")

    # Save merged manifest
    merged = {'pairs': all_pairs}
    with open(output_path, 'w') as f:
        json.dump(merged, f, indent=2)
    print(f"Saved merged manifest to: {output_path}")

    # Calculate and save class weights
    weights = calculate_class_weights(all_pairs)
    np.save(weights_path, weights)
    print(f"\nSaved class weights to: {weights_path}")


if __name__ == "__main__":
    main()
