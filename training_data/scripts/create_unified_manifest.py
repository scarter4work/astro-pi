#!/usr/bin/env python3
"""
Create a unified training manifest from all labeled directories.
Combines original data with 5-point plan improvements.

DATA SOURCE PRIORITY (see DATA_SOURCES.md for details):
  Priority 1 (HIGHEST): QNAP mount data - user's own images
  Priority 2: High-quality external sources (HST, ESO, AstroBin)
  Priority 3 (LOWEST): General web sources
"""

import json
from pathlib import Path
from datetime import datetime

BASE_DIR = Path('/home/scarter4work/projects/NukeX/training_data')
OUTPUT_MANIFEST = BASE_DIR / 'unified_manifest.json'

# Data source priority configuration
# Priority 1: QNAP-sourced data (user's own images - HIGHEST PRIORITY)
# Priority 2: High-quality external archives (HST, ESO, AstroBin)
# Priority 3: General/mixed sources
LABELED_DIRS_WITH_PRIORITY = {
    # Priority 1: QNAP data (user's own images)
    'labeled_qnap_m27': {'priority': 1, 'source': 'qnap'},
    'labeled_emission_qnap': {'priority': 1, 'source': 'qnap'},
    'labeled_reflection_qnap': {'priority': 1, 'source': 'qnap'},
    'labeled_narrowband': {'priority': 1, 'source': 'qnap'},

    # Priority 2: High-quality external sources
    'labeled_pn': {'priority': 2, 'source': 'archive'},
    'labeled_clusters': {'priority': 2, 'source': 'archive'},
    'labeled_dustlanes': {'priority': 2, 'source': 'archive'},
    'labeled_dark_nebula': {'priority': 2, 'source': 'archive'},
    'labeled_reflection': {'priority': 2, 'source': 'archive'},

    # Priority 3: Mixed/general sources
    'labeled': {'priority': 3, 'source': 'mixed'},
    'labeled_targeted': {'priority': 3, 'source': 'mixed'},
    'labeled_stretched': {'priority': 3, 'source': 'mixed'},
    'labeled_linear_noise': {'priority': 3, 'source': 'synthetic'},
}

# Legacy list for backwards compatibility
LABELED_DIRS = list(LABELED_DIRS_WITH_PRIORITY.keys())

def find_training_pairs(directory):
    """Find all image-mask pairs in a directory with source priority tagging."""
    pairs = []
    dir_path = BASE_DIR / directory

    if not dir_path.exists():
        print(f"  Skipping {directory} - not found")
        return pairs

    # Get priority info for this directory
    priority_info = LABELED_DIRS_WITH_PRIORITY.get(directory, {'priority': 3, 'source': 'unknown'})

    # Find all *_img.png files
    for img_file in dir_path.rglob('*_img.png'):
        # Corresponding mask file
        mask_file = img_file.parent / img_file.name.replace('_img.png', '_mask.png')

        if mask_file.exists():
            pairs.append({
                'image': str(img_file),
                'mask': str(mask_file),
                'source_dir': directory,
                'source': priority_info['source'],
                'source_priority': priority_info['priority'],
            })

    return pairs


def main():
    print("Creating unified training manifest")
    print("=" * 60)

    all_pairs = []
    stats = {}

    for directory in LABELED_DIRS:
        print(f"Processing {directory}...")
        pairs = find_training_pairs(directory)
        all_pairs.extend(pairs)
        stats[directory] = len(pairs)
        print(f"  Found {len(pairs)} pairs")

    print()
    print("=" * 60)
    print("Summary:")
    print("=" * 60)

    for directory, count in sorted(stats.items(), key=lambda x: -x[1]):
        if count > 0:
            print(f"  {directory}: {count}")

    print(f"\nTOTAL: {len(all_pairs)} training pairs")

    # Calculate priority statistics
    priority_stats = {1: 0, 2: 0, 3: 0}
    for pair in all_pairs:
        priority_stats[pair.get('source_priority', 3)] += 1

    print(f"\nBy priority:")
    print(f"  Priority 1 (QNAP/User data): {priority_stats[1]}")
    print(f"  Priority 2 (Archives): {priority_stats[2]}")
    print(f"  Priority 3 (Mixed/Other): {priority_stats[3]}")

    # Create manifest
    manifest = {
        'created': datetime.now().isoformat(),
        'total_pairs': len(all_pairs),
        'sources': stats,
        'priority_stats': priority_stats,
        'priority_order': [
            '1 = QNAP/User data (HIGHEST)',
            '2 = High-quality archives (HST, ESO, AstroBin)',
            '3 = Mixed/general sources (LOWEST)'
        ],
        'pairs': all_pairs
    }

    # Save manifest
    with open(OUTPUT_MANIFEST, 'w') as f:
        json.dump(manifest, f, indent=2)

    print(f"\nManifest saved to: {OUTPUT_MANIFEST}")

    # Also create a simple file list for training
    train_list = BASE_DIR / 'training_pairs.txt'
    with open(train_list, 'w') as f:
        for pair in all_pairs:
            f.write(f"{pair['image']}\t{pair['mask']}\n")

    print(f"Training list saved to: {train_list}")


if __name__ == '__main__':
    main()
