#!/usr/bin/env python3
"""
Remap existing 21-class segmentation masks to the 7-class taxonomy.

Mapping table (old 21-class ID -> new 7-class ID):

    Old  Old Name               New  New Name
    ---  --------               ---  --------
     0   Background           ->  0  Background
     1   StarBright           ->  1  BrightCompact
     2   StarMedium           ->  2  FaintCompact
     3   StarFaint            ->  2  FaintCompact
     4   StarSaturated        ->  1  BrightCompact
     5   NebulaEmission       ->  3  BrightExtended
     6   NebulaReflection     ->  3  BrightExtended
     7   NebulaDark           ->  4  DarkExtended
     8   NebulaPlanetary      ->  3  BrightExtended
     9   GalaxySpiral         ->  3  BrightExtended
    10   GalaxyElliptical     ->  3  BrightExtended
    11   GalaxyIrregular      ->  3  BrightExtended
    12   GalaxyCore           ->  3  BrightExtended
    13   DustLane             ->  4  DarkExtended
    14   StarClusterOpen      ->  2  FaintCompact
    15   StarClusterGlobular  ->  2  FaintCompact
    16   ArtifactHotPixel     ->  5  Artifact
    17   ArtifactSatellite    ->  5  Artifact
    18   ArtifactDiffraction  ->  1  BrightCompact
    19   ArtifactGradient     ->  0  Background
    20   ArtifactNoise        ->  0  Background
    21   StarHalo             ->  6  StarHalo
    22   GalacticCirrus       ->  3  BrightExtended

Processes all mask_*.npy files in the given directory, overwriting in place.
Optionally backs up originals to a subdirectory.

Usage:
    python remap_labels_7class.py --mask-dir /path/to/masks
    python remap_labels_7class.py --mask-dir /path/to/masks --backup
    python remap_labels_7class.py --mask-dir /path/to/masks --dry-run
"""

import argparse
import logging
import shutil
import sys
from pathlib import Path

import numpy as np

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# 21-class -> 7-class mapping
# ---------------------------------------------------------------------------

# Old 21-class names (indices 0-22, with 21=StarHalo, 22=GalacticCirrus added later)
OLD_CLASS_NAMES = [
    "Background",          # 0
    "StarBright",          # 1
    "StarMedium",          # 2
    "StarFaint",           # 3
    "StarSaturated",       # 4
    "NebulaEmission",      # 5
    "NebulaReflection",    # 6
    "NebulaDark",          # 7
    "NebulaPlanetary",     # 8
    "GalaxySpiral",        # 9
    "GalaxyElliptical",    # 10
    "GalaxyIrregular",     # 11
    "GalaxyCore",          # 12
    "DustLane",            # 13
    "StarClusterOpen",     # 14
    "StarClusterGlobular", # 15
    "ArtifactHotPixel",    # 16
    "ArtifactSatellite",   # 17
    "ArtifactDiffraction", # 18
    "ArtifactGradient",    # 19
    "ArtifactNoise",       # 20
    "StarHalo",            # 21
    "GalacticCirrus",      # 22
]

NEW_CLASS_NAMES = [
    "Background",       # 0
    "BrightCompact",    # 1
    "FaintCompact",     # 2
    "BrightExtended",   # 3
    "DarkExtended",     # 4
    "Artifact",         # 5
    "StarHalo",         # 6
]

# Build the lookup table: old_id -> new_id
# Index in this array is the old class ID; value is the new class ID.
# Supports up to old class 22 (GalacticCirrus).
REMAP_TABLE = np.array([
    0,  # 0  Background           -> 0 Background
    1,  # 1  StarBright           -> 1 BrightCompact
    2,  # 2  StarMedium           -> 2 FaintCompact
    2,  # 3  StarFaint            -> 2 FaintCompact
    1,  # 4  StarSaturated        -> 1 BrightCompact
    3,  # 5  NebulaEmission       -> 3 BrightExtended
    3,  # 6  NebulaReflection     -> 3 BrightExtended
    4,  # 7  NebulaDark           -> 4 DarkExtended
    3,  # 8  NebulaPlanetary      -> 3 BrightExtended
    3,  # 9  GalaxySpiral         -> 3 BrightExtended
    3,  # 10 GalaxyElliptical     -> 3 BrightExtended
    3,  # 11 GalaxyIrregular      -> 3 BrightExtended
    3,  # 12 GalaxyCore           -> 3 BrightExtended
    4,  # 13 DustLane             -> 4 DarkExtended
    2,  # 14 StarClusterOpen      -> 2 FaintCompact
    2,  # 15 StarClusterGlobular  -> 2 FaintCompact
    5,  # 16 ArtifactHotPixel     -> 5 Artifact
    5,  # 17 ArtifactSatellite    -> 5 Artifact
    1,  # 18 ArtifactDiffraction  -> 1 BrightCompact
    0,  # 19 ArtifactGradient     -> 0 Background
    0,  # 20 ArtifactNoise        -> 0 Background
    6,  # 21 StarHalo             -> 6 StarHalo
    3,  # 22 GalacticCirrus       -> 3 BrightExtended
], dtype=np.int64)


def remap_mask(mask: np.ndarray) -> np.ndarray:
    """
    Remap a mask from the 21-class (or 23-class) taxonomy to 7 classes.

    Values outside the known range are mapped to 0 (Background).

    Args:
        mask: integer array with old class IDs

    Returns:
        integer array with new 7-class IDs
    """
    mask = mask.astype(np.int64)
    # Clamp out-of-range values to 0 (Background)
    out_of_range = (mask < 0) | (mask >= len(REMAP_TABLE))
    mask[out_of_range] = 0
    return REMAP_TABLE[mask]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Remap 21-class segmentation masks to 7-class taxonomy"
    )
    parser.add_argument(
        "--mask-dir",
        type=str,
        required=True,
        help="Directory containing mask_*.npy files",
    )
    parser.add_argument(
        "--backup",
        action="store_true",
        help="Back up original masks to mask-dir/backup_21class/ before overwriting",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be done without modifying files",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default=None,
        help="Write remapped masks to a different directory instead of overwriting",
    )
    args = parser.parse_args()

    mask_dir = Path(args.mask_dir)
    if not mask_dir.is_dir():
        sys.exit(f"ERROR: Directory does not exist: {mask_dir}")

    mask_files = sorted(mask_dir.glob("mask_*.npy"))
    if not mask_files:
        sys.exit(f"ERROR: No mask_*.npy files found in {mask_dir}")

    logger.info("Found %d mask files in %s", len(mask_files), mask_dir)

    # Print mapping summary
    logger.info("Remapping table:")
    for old_id, new_id in enumerate(REMAP_TABLE):
        old_name = OLD_CLASS_NAMES[old_id] if old_id < len(OLD_CLASS_NAMES) else f"Class{old_id}"
        new_name = NEW_CLASS_NAMES[new_id]
        logger.info("  %2d %-22s -> %d %s", old_id, old_name, new_id, new_name)

    # Determine output directory
    if args.output_dir:
        out_dir = Path(args.output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
    else:
        out_dir = mask_dir

    # Backup if requested
    if args.backup and not args.dry_run and out_dir == mask_dir:
        backup_dir = mask_dir / "backup_21class"
        backup_dir.mkdir(exist_ok=True)
        logger.info("Backing up originals to %s", backup_dir)
        for mf in mask_files:
            shutil.copy2(mf, backup_dir / mf.name)

    # Process all masks
    stats = {name: 0 for name in NEW_CLASS_NAMES}
    total_pixels = 0
    processed = 0
    skipped = 0

    for mf in mask_files:
        mask = np.load(mf)

        # Validate: check if this mask looks like it has 21+ class IDs
        unique_vals = np.unique(mask)
        max_val = unique_vals.max()

        if max_val < 7 and max_val >= 0:
            # Already looks like 7-class (or fewer).  Skip unless forced.
            if max_val <= 6:
                logger.debug("  %s appears already remapped (max=%d), skipping", mf.name, max_val)
                skipped += 1
                continue

        new_mask = remap_mask(mask)

        # Accumulate statistics
        for cls_id in range(len(NEW_CLASS_NAMES)):
            stats[NEW_CLASS_NAMES[cls_id]] += (new_mask == cls_id).sum()
        total_pixels += new_mask.size

        if args.dry_run:
            old_unique = np.unique(mask)
            new_unique = np.unique(new_mask)
            logger.info(
                "  %s: old classes %s -> new classes %s",
                mf.name,
                old_unique.tolist(),
                new_unique.tolist(),
            )
        else:
            out_path = out_dir / mf.name
            np.save(out_path, new_mask.astype(np.int64))

        processed += 1

    # Summary
    logger.info("")
    logger.info("=" * 50)
    logger.info("Remapping complete")
    logger.info("  Processed: %d masks", processed)
    logger.info("  Skipped (already 7-class): %d masks", skipped)
    if total_pixels > 0:
        logger.info("  Class distribution:")
        for name in NEW_CLASS_NAMES:
            count = stats[name]
            pct = 100.0 * count / total_pixels
            logger.info("    %-18s %12d pixels (%.2f%%)", name, count, pct)
    if args.dry_run:
        logger.info("  (dry run - no files were modified)")


if __name__ == "__main__":
    main()
