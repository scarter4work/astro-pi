#!/usr/bin/env python3
"""
Check class distribution across all training data.
"""

import numpy as np
from pathlib import Path
from PIL import Image
from collections import defaultdict
from tqdm import tqdm
import argparse

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

# Class groupings for summary
CLASS_GROUPS = {
    'Stars': [1, 2, 3, 4],
    'Nebulae': [5, 6, 7, 8],
    'Galaxies': [9, 10, 11, 12],
    'Structures': [13, 14, 15],
    'Artifacts': [16, 17, 18, 19, 20],
}


def count_classes_in_mask(mask_path):
    """Count pixels per class in a mask."""
    mask = np.array(Image.open(mask_path))
    unique, counts = np.unique(mask, return_counts=True)
    return dict(zip(unique.tolist(), counts.tolist()))


def main():
    parser = argparse.ArgumentParser(description='Check class distribution')
    parser.add_argument('--dirs', nargs='+',
                       default=[
                           '/home/scarter4work/projects/NukeX/training_data/labeled',
                           '/home/scarter4work/projects/NukeX/training_data/synthetic',
                           '/home/scarter4work/projects/NukeX/training_data/augmented',
                       ],
                       help='Directories to scan')
    args = parser.parse_args()

    total_counts = defaultdict(int)
    dir_counts = {}

    for data_dir in args.dirs:
        data_path = Path(data_dir)
        if not data_path.exists():
            print(f"Skipping {data_dir} (not found)")
            continue

        print(f"\nScanning {data_dir}...")
        mask_files = list(data_path.rglob("*_mask.png"))
        print(f"  Found {len(mask_files)} masks")

        dir_total = defaultdict(int)
        for mask_path in tqdm(mask_files, desc="  Processing"):
            counts = count_classes_in_mask(mask_path)
            for cls, count in counts.items():
                total_counts[cls] += count
                dir_total[cls] += count

        dir_counts[data_dir] = dict(dir_total)

    # Print summary
    print("\n" + "=" * 80)
    print("OVERALL CLASS DISTRIBUTION")
    print("=" * 80)

    total_pixels = sum(total_counts.values())
    print(f"\nTotal pixels: {total_pixels:,}")
    print(f"Total masks: {sum(len(list(Path(d).rglob('*_mask.png'))) for d in args.dirs if Path(d).exists())}")

    print("\n{:<5} {:<25} {:>15} {:>10}".format("ID", "Class", "Pixels", "Percent"))
    print("-" * 60)

    classes_present = 0
    classes_significant = 0  # > 0.1%

    for cls_id in sorted(CLASS_NAMES.keys()):
        count = total_counts.get(cls_id, 0)
        pct = (count / total_pixels * 100) if total_pixels > 0 else 0
        name = CLASS_NAMES[cls_id]

        # Color coding based on percentage
        if pct == 0:
            status = "MISSING"
        elif pct < 0.1:
            status = "minimal"
            classes_present += 1
        elif pct < 1.0:
            status = "low"
            classes_present += 1
            classes_significant += 1
        else:
            status = "GOOD"
            classes_present += 1
            classes_significant += 1

        print(f"{cls_id:<5} {name:<25} {count:>15,} {pct:>8.3f}% [{status}]")

    # Group summary
    print("\n" + "=" * 80)
    print("GROUP SUMMARY")
    print("=" * 80)

    for group_name, class_ids in CLASS_GROUPS.items():
        group_total = sum(total_counts.get(c, 0) for c in class_ids)
        group_pct = (group_total / total_pixels * 100) if total_pixels > 0 else 0
        classes_in_group = sum(1 for c in class_ids if total_counts.get(c, 0) > 0)
        print(f"{group_name:<15}: {group_total:>15,} pixels ({group_pct:>6.2f}%) - {classes_in_group}/{len(class_ids)} classes present")

    # Final summary
    print("\n" + "=" * 80)
    print("TRAINING READINESS")
    print("=" * 80)
    print(f"Classes present (>0 pixels):    {classes_present}/21")
    print(f"Classes significant (>0.1%):    {classes_significant}/21")

    missing = [CLASS_NAMES[c] for c in CLASS_NAMES if total_counts.get(c, 0) == 0]
    if missing:
        print(f"\nMISSING classes: {', '.join(missing)}")

    minimal = [CLASS_NAMES[c] for c in CLASS_NAMES if 0 < total_counts.get(c, 0) and (total_counts[c] / total_pixels * 100) < 0.1]
    if minimal:
        print(f"MINIMAL classes (<0.1%): {', '.join(minimal)}")

    low = [CLASS_NAMES[c] for c in CLASS_NAMES if 0.1 <= (total_counts.get(c, 0) / total_pixels * 100) < 1.0]
    if low:
        print(f"LOW classes (0.1-1.0%): {', '.join(low)}")


if __name__ == "__main__":
    main()
