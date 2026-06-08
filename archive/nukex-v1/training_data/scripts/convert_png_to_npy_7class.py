#!/usr/bin/env python3
"""
Convert PNG img/mask training pairs to tile_*.npy / mask_*.npy format
with 7-class label remapping.

Reads *_img.png and *_mask.png pairs from one or more source directories,
applies the 21-class -> 7-class remap, optionally center-crops non-512x512
tiles, and writes sequential tile_NNNNNN.npy / mask_NNNNNN.npy files to
the output directory.

Output format matches what train_7class.py expects:
  - tile: float32 (512, 512, 3) in [0, 1]
  - mask: int64   (512, 512)    with values in [0, 6]

Usage:
    python convert_png_to_npy_7class.py \
        --src-dirs dir1 dir2 dir3 \
        --output-dir /path/to/tiles_7class \
        --start-index 105
"""

import argparse
import logging
import sys
from pathlib import Path

import numpy as np
from PIL import Image

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# 21-class -> 7-class remap table (same as remap_labels_7class.py)
# ---------------------------------------------------------------------------

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

NEW_CLASS_NAMES = [
    "Background",       # 0
    "BrightCompact",    # 1
    "FaintCompact",     # 2
    "BrightExtended",   # 3
    "DarkExtended",     # 4
    "Artifact",         # 5
    "StarHalo",         # 6
]

TARGET_SIZE = 512


def remap_mask(mask: np.ndarray) -> np.ndarray:
    """Remap 21-class mask to 7-class."""
    mask = mask.astype(np.int64)
    out_of_range = (mask < 0) | (mask >= len(REMAP_TABLE))
    mask[out_of_range] = 0
    return REMAP_TABLE[mask]


def center_crop(arr: np.ndarray, size: int) -> np.ndarray:
    """Center-crop a 2D or 3D array to (size, size, ...)."""
    h, w = arr.shape[:2]
    if h < size or w < size:
        raise ValueError(f"Array {arr.shape} is smaller than target {size}x{size}")
    y0 = (h - size) // 2
    x0 = (w - size) // 2
    return arr[y0:y0 + size, x0:x0 + size]


def find_png_pairs(src_dir: Path):
    """Find all *_img.png / *_mask.png pairs recursively."""
    pairs = []
    img_files = {}
    for f in sorted(src_dir.rglob("*_img.png")):
        stem = f.name.replace("_img.png", "")
        img_files[stem] = f

    for stem, img_path in sorted(img_files.items()):
        mask_path = img_path.parent / f"{stem}_mask.png"
        if mask_path.exists():
            pairs.append((img_path, mask_path))

    return pairs


def main():
    parser = argparse.ArgumentParser(
        description="Convert PNG img/mask pairs to npy with 7-class remapping"
    )
    parser.add_argument(
        "--src-dirs",
        type=str,
        nargs="+",
        required=True,
        help="Source directories containing *_img.png / *_mask.png pairs",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        required=True,
        help="Output directory for tile_*.npy / mask_*.npy files",
    )
    parser.add_argument(
        "--start-index",
        type=int,
        default=0,
        help="Starting index for output file numbering (to append to existing data)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be done without writing files",
    )
    args = parser.parse_args()

    out_dir = Path(args.output_dir)
    if not args.dry_run:
        out_dir.mkdir(parents=True, exist_ok=True)

    idx = args.start_index
    total_pairs = 0
    skipped = 0
    class_counts = np.zeros(len(NEW_CLASS_NAMES), dtype=np.int64)

    for src in args.src_dirs:
        src_dir = Path(src)
        if not src_dir.is_dir():
            logger.warning("Skipping non-existent directory: %s", src_dir)
            continue

        pairs = find_png_pairs(src_dir)
        logger.info("Found %d pairs in %s", len(pairs), src_dir)

        for img_path, mask_path in pairs:
            try:
                img = np.array(Image.open(img_path).convert("RGB")).astype(np.float32)
                mask = np.array(Image.open(mask_path))

                # Normalize image to [0, 1]
                if img.max() > 1.5:
                    img = img / 255.0

                # Handle size mismatch - center crop if larger than 512
                h, w = img.shape[:2]
                if h != TARGET_SIZE or w != TARGET_SIZE:
                    if h >= TARGET_SIZE and w >= TARGET_SIZE:
                        img = center_crop(img, TARGET_SIZE)
                        mask = center_crop(mask, TARGET_SIZE)
                    else:
                        logger.warning(
                            "  Skipping %s: size %dx%d is smaller than %d",
                            img_path.name, w, h, TARGET_SIZE,
                        )
                        skipped += 1
                        continue

                # Remap mask from 21-class to 7-class
                mask = remap_mask(mask)

                # Validate
                assert img.shape == (TARGET_SIZE, TARGET_SIZE, 3), f"Bad tile shape: {img.shape}"
                assert mask.shape == (TARGET_SIZE, TARGET_SIZE), f"Bad mask shape: {mask.shape}"
                assert mask.min() >= 0 and mask.max() <= 6, f"Bad mask range: {mask.min()}-{mask.max()}"

                if args.dry_run:
                    if idx < args.start_index + 3:
                        logger.info(
                            "  [%06d] %s -> tile shape=%s dtype=%s range=[%.4f, %.4f], mask unique=%s",
                            idx, img_path.name, img.shape, img.dtype,
                            img.min(), img.max(), np.unique(mask).tolist(),
                        )
                else:
                    np.save(out_dir / f"tile_{idx:06d}.npy", img)
                    np.save(out_dir / f"mask_{idx:06d}.npy", mask.astype(np.int64))

                # Accumulate class stats
                for c in range(len(NEW_CLASS_NAMES)):
                    class_counts[c] += (mask == c).sum()

                idx += 1
                total_pairs += 1

            except Exception as e:
                logger.error("  Error processing %s: %s", img_path.name, e)
                skipped += 1

    # Summary
    logger.info("")
    logger.info("=" * 60)
    logger.info("Conversion complete")
    logger.info("  Total pairs converted: %d", total_pairs)
    logger.info("  Skipped: %d", skipped)
    logger.info("  Output index range: %06d - %06d", args.start_index, idx - 1)
    logger.info("  Output directory: %s", out_dir)
    total_pixels = class_counts.sum()
    if total_pixels > 0:
        logger.info("  Class distribution:")
        for i, name in enumerate(NEW_CLASS_NAMES):
            pct = 100.0 * class_counts[i] / total_pixels
            logger.info("    %d %-18s %12d pixels (%.2f%%)", i, name, class_counts[i], pct)
    if args.dry_run:
        logger.info("  (dry run - no files were written)")


if __name__ == "__main__":
    main()
