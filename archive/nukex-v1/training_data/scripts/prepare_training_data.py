#!/usr/bin/env python3
"""
Prepare training data by creating a consolidated dataset manifest.
Also calculates class weights for balanced training.

DATA SOURCE PRIORITY (see DATA_SOURCES.md for details):
  Priority 1 (HIGHEST): QNAP mount data - user's own images
  Priority 2: High-quality external sources (HST, ESO, AstroBin)
  Priority 3 (LOWEST): General web sources
"""

import numpy as np
from pathlib import Path
from PIL import Image
from collections import defaultdict
from tqdm import tqdm
import json

# 21-class names
CLASS_NAMES = {
    0: 'background',
    1: 'star_bright',
    2: 'star_medium',
    3: 'star_faint',
    4: 'star_saturated',
    5: 'nebula_emission',
    6: 'nebula_reflection',
    7: 'nebula_dark',
    8: 'nebula_planetary',
    9: 'galaxy_spiral',
    10: 'galaxy_elliptical',
    11: 'galaxy_irregular',
    12: 'galaxy_core',
    13: 'dust_lane',
    14: 'star_cluster_open',
    15: 'star_cluster_globular',
    16: 'artifact_hot_pixel',
    17: 'artifact_satellite',
    18: 'artifact_diffraction',
    19: 'artifact_gradient',
    20: 'artifact_noise',
}

# Data directories with source priority
# Priority 1: QNAP/User data (HIGHEST - check these first)
# Priority 2: High-quality archives
# Priority 3: Mixed/synthetic sources
DATA_DIRS_WITH_PRIORITY = {
    # Priority 1: QNAP-sourced data (user's own images - HIGHEST PRIORITY)
    '/home/scarter4work/projects/NukeX/training_data/labeled_qnap_m27': {'priority': 1, 'source': 'qnap'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_emission_qnap': {'priority': 1, 'source': 'qnap'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_reflection_qnap': {'priority': 1, 'source': 'qnap'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_narrowband': {'priority': 1, 'source': 'qnap'},

    # Priority 2: High-quality archive data
    '/home/scarter4work/projects/NukeX/training_data/labeled_pn': {'priority': 2, 'source': 'archive'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_reflection': {'priority': 2, 'source': 'archive'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_clusters': {'priority': 2, 'source': 'archive'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_dustlanes': {'priority': 2, 'source': 'archive'},

    # Priority 3: Mixed/general sources
    '/home/scarter4work/projects/NukeX/training_data/labeled': {'priority': 3, 'source': 'mixed'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_targeted': {'priority': 3, 'source': 'mixed'},
    '/home/scarter4work/projects/NukeX/training_data/labeled_stretched': {'priority': 3, 'source': 'mixed'},
    '/home/scarter4work/projects/NukeX/training_data/augmented': {'priority': 3, 'source': 'augmented'},

    # Synthetic data (supplementary)
    '/home/scarter4work/projects/NukeX/training_data/synthetic': {'priority': 3, 'source': 'synthetic'},
    '/home/scarter4work/projects/NukeX/training_data/synthetic_extra': {'priority': 3, 'source': 'synthetic'},
    '/home/scarter4work/projects/NukeX/training_data/synthetic_artifacts': {'priority': 3, 'source': 'synthetic'},
    '/home/scarter4work/projects/NukeX/training_data/synthetic_massive': {'priority': 3, 'source': 'synthetic'},
    '/home/scarter4work/projects/NukeX/training_data/synthetic_weak': {'priority': 3, 'source': 'synthetic'},
}

# Sorted by priority (1 first, then 2, then 3)
DATA_DIRS = sorted(DATA_DIRS_WITH_PRIORITY.keys(),
                   key=lambda x: DATA_DIRS_WITH_PRIORITY[x]['priority'])


def main():
    output_dir = Path('/home/scarter4work/projects/NukeX/training_data')

    print("=" * 70)
    print("TRAINING DATA PREPARATION")
    print("=" * 70)

    # Collect all image-mask pairs
    all_pairs = []
    total_counts = defaultdict(int)

    priority_counts = {1: 0, 2: 0, 3: 0}

    for data_dir in DATA_DIRS:
        data_path = Path(data_dir)
        if not data_path.exists():
            continue

        priority_info = DATA_DIRS_WITH_PRIORITY.get(data_dir, {'priority': 3, 'source': 'unknown'})

        mask_files = list(data_path.rglob("*_mask.png"))
        for mask_path in tqdm(mask_files, desc=f"Scanning {data_path.name}"):
            img_path = mask_path.with_name(mask_path.name.replace("_mask.png", "_img.png"))
            if img_path.exists():
                all_pairs.append({
                    'image': str(img_path),
                    'mask': str(mask_path),
                    'source': priority_info['source'],
                    'source_priority': priority_info['priority'],
                })
                priority_counts[priority_info['priority']] += 1

                # Count classes
                mask = np.array(Image.open(mask_path))
                unique, counts = np.unique(mask, return_counts=True)
                for cls, cnt in zip(unique.tolist(), counts.tolist()):
                    total_counts[cls] += cnt

    print(f"\nTotal image-mask pairs: {len(all_pairs)}")
    print(f"\nBy source priority:")
    print(f"  Priority 1 (QNAP/User data): {priority_counts[1]}")
    print(f"  Priority 2 (Archives): {priority_counts[2]}")
    print(f"  Priority 3 (Mixed/Synthetic): {priority_counts[3]}")

    # Calculate class weights (inverse frequency)
    total_pixels = sum(total_counts.values())
    class_weights = {}
    class_freqs = {}

    for cls_id in range(21):
        count = total_counts.get(cls_id, 1)  # Avoid division by zero
        freq = count / total_pixels
        class_freqs[cls_id] = freq

        # Inverse frequency weight, capped to avoid extreme values
        weight = min(1.0 / (freq * 21 + 1e-6), 100.0)
        class_weights[cls_id] = weight

    # Normalize weights
    max_weight = max(class_weights.values())
    for cls_id in class_weights:
        class_weights[cls_id] /= max_weight

    # Print summary
    print("\n" + "=" * 70)
    print("CLASS DISTRIBUTION & WEIGHTS")
    print("=" * 70)
    print(f"\n{'ID':<4} {'Class':<25} {'Pixels':>15} {'Percent':>10} {'Weight':>10}")
    print("-" * 70)

    for cls_id in sorted(CLASS_NAMES.keys()):
        name = CLASS_NAMES[cls_id]
        count = total_counts.get(cls_id, 0)
        pct = (count / total_pixels * 100) if total_pixels > 0 else 0
        weight = class_weights.get(cls_id, 1.0)
        print(f"{cls_id:<4} {name:<25} {count:>15,} {pct:>9.3f}% {weight:>9.3f}")

    # Save manifest
    manifest = {
        'pairs': all_pairs,
        'class_weights': class_weights,
        'class_counts': dict(total_counts),
        'class_names': CLASS_NAMES,
        'total_pixels': total_pixels,
        'total_pairs': len(all_pairs),
        'priority_stats': priority_counts,
        'priority_order': [
            '1 = QNAP/User data (HIGHEST)',
            '2 = High-quality archives (HST, ESO, AstroBin)',
            '3 = Mixed/general/synthetic sources (LOWEST)'
        ],
    }

    manifest_path = output_dir / 'training_manifest.json'
    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)

    print(f"\nManifest saved to: {manifest_path}")

    # Save class weights for PyTorch
    weights_array = np.array([class_weights[i] for i in range(21)])
    weights_path = output_dir / 'class_weights.npy'
    np.save(weights_path, weights_array)
    print(f"Class weights saved to: {weights_path}")

    # Print training recommendations
    print("\n" + "=" * 70)
    print("TRAINING RECOMMENDATIONS")
    print("=" * 70)
    print("""
1. Use weighted cross-entropy loss with the provided class_weights.npy
2. Consider using Focal Loss for better handling of class imbalance
3. Use data augmentation (flips, rotations, brightness variations)
4. Train with batch size 32-64 to utilize RTX 5070 Ti (16GB VRAM)
5. Use mixed precision (FP16) for faster training

Sample PyTorch code:
```python
import torch
import torch.nn as nn
import numpy as np

# Load class weights
weights = np.load('class_weights.npy')
weights = torch.tensor(weights, dtype=torch.float32).to(device)

# Weighted cross entropy
criterion = nn.CrossEntropyLoss(weight=weights)

# Or use Focal Loss for better class imbalance handling
```
""")


if __name__ == "__main__":
    main()
