#!/usr/bin/env python3
"""
Process raw Bayer/CFA FITS files into training tiles for the 7-class model.

Workflow per FITS file:
  1. Load mono CFA data and read BAYERPAT keyword
  2. Bilinear demosaic to RGB (matching C++ BayerDemosaic.cpp)
  3. Per-channel arcsinh stretch to [0, 1]
  4. Extract 512x512 tiles with 64 px overlap
  5. Rule-based labeling for 7 classes
  6. Save as tile_XXXXXX.npy / mask_XXXXXX.npy

Supported Bayer patterns: RGGB, GRBG, GBRG, BGGR (auto-detected from FITS header).

Usage:
    python process_bayer_fits.py --input-dir /mnt/qnap/astro/M42 --output-dir tiles/
"""

import argparse
import logging
import os
import sys
from pathlib import Path

import numpy as np

try:
    from astropy.io import fits as pyfits
except ImportError:
    sys.exit("ERROR: astropy is required. Install with: pip install astropy")

try:
    from scipy.ndimage import binary_dilation, label, generate_binary_structure
except ImportError:
    sys.exit("ERROR: scipy is required. Install with: pip install scipy")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# 7-class taxonomy
# ---------------------------------------------------------------------------

CLASS_BACKGROUND = 0
CLASS_BRIGHT_COMPACT = 1
CLASS_FAINT_COMPACT = 2
CLASS_BRIGHT_EXTENDED = 3
CLASS_DARK_EXTENDED = 4
CLASS_ARTIFACT = 5
CLASS_STAR_HALO = 6

CLASS_NAMES = [
    "Background",
    "BrightCompact",
    "FaintCompact",
    "BrightExtended",
    "DarkExtended",
    "Artifact",
    "StarHalo",
]


# ---------------------------------------------------------------------------
# Bayer pattern helpers (mirrors C++ GetBayerOffsets)
# ---------------------------------------------------------------------------

BAYER_OFFSETS = {
    "RGGB": {"R": (0, 0), "G1": (1, 0), "G2": (0, 1), "B": (1, 1)},
    "BGGR": {"B": (0, 0), "G1": (1, 0), "G2": (0, 1), "R": (1, 1)},
    "GRBG": {"G1": (0, 0), "R": (1, 0), "B": (0, 1), "G2": (1, 1)},
    "GBRG": {"G1": (0, 0), "B": (1, 0), "R": (0, 1), "G2": (1, 1)},
}


def detect_bayer_pattern(header) -> str:
    """Read BAYERPAT from FITS header.  Returns uppercase pattern string."""
    for key in ("BAYERPAT", "COLORTYP", "CFA_PAT"):
        val = header.get(key, None)
        if val is not None:
            val = str(val).strip().strip("'").upper()
            if val in BAYER_OFFSETS:
                return val
    # Default to RGGB (most common for CMOS astro cameras)
    logger.warning("BAYERPAT not found in header; defaulting to RGGB")
    return "RGGB"


def pixel_type(x: int, y: int, offsets: dict) -> int:
    """Return 0=R, 1=G, 2=B for the CFA pixel at (x, y)."""
    cx = x & 1
    cy = y & 1
    if (cx, cy) == offsets["R"]:
        return 0
    if (cx, cy) == offsets["B"]:
        return 2
    return 1


def is_green_on_red_row(y: int, offsets: dict) -> bool:
    return (y & 1) == offsets["R"][1]


# ---------------------------------------------------------------------------
# Bilinear demosaic (matches C++ BayerDemosaic::Demosaic)
# ---------------------------------------------------------------------------

def bilinear_demosaic(cfa: np.ndarray, pattern: str) -> np.ndarray:
    """
    Bilinear demosaic a single-channel CFA image to 3-channel RGB.

    The algorithm exactly mirrors BayerDemosaic.cpp:
      - Red pixel: R=direct, G=cardinal avg, B=diagonal avg
      - Blue pixel: R=diagonal avg, G=cardinal avg, B=direct
      - Green on red row: G=direct, R=left/right avg, B=top/bottom avg
      - Green on blue row: G=direct, R=top/bottom avg, B=left/right avg

    Args:
        cfa: float32 (H, W) mono CFA data
        pattern: one of RGGB, BGGR, GRBG, GBRG

    Returns:
        float32 (H, W, 3) RGB image
    """
    h, w = cfa.shape
    offsets = BAYER_OFFSETS[pattern]

    rgb = np.zeros((h, w, 3), dtype=np.float32)

    # Padded source for clamped neighbour access
    src = np.pad(cfa, ((1, 1), (1, 1)), mode="edge")  # access via src[y+1, x+1]

    for y in range(h):
        sy = y + 1  # offset into padded array
        for x in range(w):
            sx = x + 1
            pt = pixel_type(x, y, offsets)

            if pt == 0:  # Red
                R = src[sy, sx]
                G = (src[sy - 1, sx] + src[sy + 1, sx] + src[sy, sx - 1] + src[sy, sx + 1]) * 0.25
                B = (src[sy - 1, sx - 1] + src[sy - 1, sx + 1] + src[sy + 1, sx - 1] + src[sy + 1, sx + 1]) * 0.25
            elif pt == 2:  # Blue
                R = (src[sy - 1, sx - 1] + src[sy - 1, sx + 1] + src[sy + 1, sx - 1] + src[sy + 1, sx + 1]) * 0.25
                G = (src[sy - 1, sx] + src[sy + 1, sx] + src[sy, sx - 1] + src[sy, sx + 1]) * 0.25
                B = src[sy, sx]
            else:  # Green
                G = src[sy, sx]
                if is_green_on_red_row(y, offsets):
                    R = (src[sy, sx - 1] + src[sy, sx + 1]) * 0.5
                    B = (src[sy - 1, sx] + src[sy + 1, sx]) * 0.5
                else:
                    R = (src[sy - 1, sx] + src[sy + 1, sx]) * 0.5
                    B = (src[sy, sx - 1] + src[sy, sx + 1]) * 0.5

            rgb[y, x, 0] = R
            rgb[y, x, 1] = G
            rgb[y, x, 2] = B

    return rgb


def bilinear_demosaic_fast(cfa: np.ndarray, pattern: str) -> np.ndarray:
    """
    Vectorised bilinear demosaic (much faster than per-pixel loop).

    Uses convolution-style neighbour averaging via slicing.
    Falls back to the exact loop version for very small images.
    """
    h, w = cfa.shape
    if h < 4 or w < 4:
        return bilinear_demosaic(cfa, pattern)

    offsets = BAYER_OFFSETS[pattern]
    rgb = np.zeros((h, w, 3), dtype=np.float32)

    # Padded CFA for safe neighbour access
    p = np.pad(cfa, ((1, 1), (1, 1)), mode="edge")

    # Cardinal and diagonal averages for the full padded image
    cardinal = (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:]) * 0.25
    diagonal = (p[:-2, :-2] + p[:-2, 2:] + p[2:, :-2] + p[2:, 2:]) * 0.25

    # Horizontal and vertical averages
    horiz = (p[1:-1, :-2] + p[1:-1, 2:]) * 0.5
    vert = (p[:-2, 1:-1] + p[2:, 1:-1]) * 0.5

    # Build per-pixel type mask
    yy, xx = np.mgrid[:h, :w]
    cx = xx & 1
    cy = yy & 1

    r_mask = (cx == offsets["R"][0]) & (cy == offsets["R"][1])
    b_mask = (cx == offsets["B"][0]) & (cy == offsets["B"][1])
    g_mask = ~r_mask & ~b_mask

    # Green on red row / blue row
    g_red_row = g_mask & (cy == offsets["R"][1])
    g_blue_row = g_mask & ~(cy == offsets["R"][1])

    # --- Red channel ---
    rgb[:, :, 0][r_mask] = cfa[r_mask]                  # direct
    rgb[:, :, 0][b_mask] = diagonal[b_mask]              # diag avg
    rgb[:, :, 0][g_red_row] = horiz[g_red_row]           # left/right
    rgb[:, :, 0][g_blue_row] = vert[g_blue_row]          # top/bottom

    # --- Green channel ---
    rgb[:, :, 1][g_mask] = cfa[g_mask]                   # direct
    rgb[:, :, 1][r_mask] = cardinal[r_mask]              # cardinal avg
    rgb[:, :, 1][b_mask] = cardinal[b_mask]              # cardinal avg

    # --- Blue channel ---
    rgb[:, :, 2][b_mask] = cfa[b_mask]                   # direct
    rgb[:, :, 2][r_mask] = diagonal[r_mask]              # diag avg
    rgb[:, :, 2][g_red_row] = vert[g_red_row]            # top/bottom
    rgb[:, :, 2][g_blue_row] = horiz[g_blue_row]         # left/right

    return rgb


# ---------------------------------------------------------------------------
# Arcsinh stretch (per-channel)
# ---------------------------------------------------------------------------

def arcsinh_stretch(img: np.ndarray, scale: float = 5.0) -> np.ndarray:
    """
    Per-channel arcsinh stretch to [0, 1].

    arcsinh(x * scale) / arcsinh(scale)
    """
    out = np.zeros_like(img)
    denom = np.arcsinh(scale)
    for ch in range(img.shape[2]):
        chan = img[:, :, ch].astype(np.float64)
        # Normalise to [0, 1] first
        mn, mx = chan.min(), chan.max()
        if mx > mn:
            chan = (chan - mn) / (mx - mn)
        stretched = np.arcsinh(chan * scale) / denom
        out[:, :, ch] = np.clip(stretched, 0, 1).astype(np.float32)
    return out


# ---------------------------------------------------------------------------
# Rule-based 7-class labeling
# ---------------------------------------------------------------------------

def label_tile(rgb: np.ndarray) -> np.ndarray:
    """
    Generate a 7-class segmentation mask from an RGB tile.

    Heuristic rules:
      - BrightCompact: very bright (>5 sigma above median), compact
      - FaintCompact:  moderately bright (>2.5 sigma), compact
      - BrightExtended: extended emission above background with colour excess (R-B>0.05)
      - DarkExtended:  significantly darker than background (<median - 1.5 sigma)
      - StarHalo:      dilated ring around bright compact sources
      - Artifact:      isolated very bright single pixels (hot pixel detection)
      - Background:    everything else

    Args:
        rgb: float32 (H, W, 3) image in [0, 1]

    Returns:
        int64 (H, W) mask with class IDs in [0, 6]
    """
    h, w = rgb.shape[:2]
    mask = np.zeros((h, w), dtype=np.int64)  # default Background

    gray = rgb.mean(axis=2)
    median = np.median(gray)
    sigma = np.std(gray)

    if sigma < 1e-8:
        return mask  # uniform image -> all background

    # --- Artifact: isolated very bright single pixels (hot pixels) ---
    # A hot pixel is bright but its immediate neighbours are not.
    bright_thresh = median + 8 * sigma
    hot_candidates = gray > bright_thresh
    # Check isolation: for each candidate, see if any of the 8 neighbours
    # are also above a lower threshold.
    struct = generate_binary_structure(2, 2)  # 3x3 connectivity
    dilated = binary_dilation(hot_candidates, structure=struct, iterations=1)
    # If the dilated region is much larger than the original, it is extended -> not hot pixel
    labeled_hot, n_hot = label(hot_candidates)
    for lbl in range(1, n_hot + 1):
        region = labeled_hot == lbl
        area = region.sum()
        if area <= 3:  # truly isolated: 1-3 pixels
            mask[region] = CLASS_ARTIFACT

    # --- BrightCompact: very bright (>5 sigma) ---
    bright_compact = gray > (median + 5 * sigma)
    # Remove already-labeled artifacts
    bright_compact = bright_compact & (mask == CLASS_BACKGROUND)
    labeled_bc, n_bc = label(bright_compact)
    for lbl in range(1, n_bc + 1):
        region = labeled_bc == lbl
        area = region.sum()
        # Compact: area < 1% of tile
        if area < 0.01 * h * w:
            mask[region] = CLASS_BRIGHT_COMPACT

    # --- StarHalo: dilated ring around BrightCompact ---
    bc_mask = mask == CLASS_BRIGHT_COMPACT
    if bc_mask.any():
        halo_struct = generate_binary_structure(2, 2)
        dilated_bc = binary_dilation(bc_mask, structure=halo_struct, iterations=5)
        halo_ring = dilated_bc & (~bc_mask) & (mask == CLASS_BACKGROUND)
        # Only keep the halo where brightness is above background
        halo_ring = halo_ring & (gray > median + 0.5 * sigma)
        mask[halo_ring] = CLASS_STAR_HALO

    # --- FaintCompact: moderately bright (>2.5 sigma) ---
    faint_compact = gray > (median + 2.5 * sigma)
    faint_compact = faint_compact & (mask == CLASS_BACKGROUND)
    labeled_fc, n_fc = label(faint_compact)
    for lbl in range(1, n_fc + 1):
        region = labeled_fc == lbl
        area = region.sum()
        if area < 0.01 * h * w:
            mask[region] = CLASS_FAINT_COMPACT

    # --- BrightExtended: extended emission with color excess ---
    # Look for regions above background that are NOT compact stars
    emission_thresh = median + 1.0 * sigma
    extended_bright = (gray > emission_thresh) & (mask == CLASS_BACKGROUND)
    # Color excess: R - B > 0.05 (emission nebulae tend to be redder)
    color_excess = rgb[:, :, 0] - rgb[:, :, 2]
    has_color = np.abs(color_excess) > 0.05
    extended_bright = extended_bright & has_color
    labeled_ext, n_ext = label(extended_bright)
    for lbl in range(1, n_ext + 1):
        region = labeled_ext == lbl
        area = region.sum()
        # Extended: area > 0.5% of tile (larger than a star)
        if area > 0.005 * h * w:
            mask[region] = CLASS_BRIGHT_EXTENDED

    # --- DarkExtended: significantly darker than background ---
    dark_thresh = median - 1.5 * sigma
    dark_ext = (gray < dark_thresh) & (mask == CLASS_BACKGROUND)
    labeled_dark, n_dark = label(dark_ext)
    for lbl in range(1, n_dark + 1):
        region = labeled_dark == lbl
        area = region.sum()
        if area > 0.005 * h * w:
            mask[region] = CLASS_DARK_EXTENDED

    return mask


# ---------------------------------------------------------------------------
# Tile extraction
# ---------------------------------------------------------------------------

def extract_tiles(
    img: np.ndarray,
    tile_size: int = 512,
    overlap: int = 64,
) -> list:
    """
    Extract overlapping tiles from an image.

    Returns list of (tile_img, row, col) where row/col are the top-left pixel coords.
    """
    h, w = img.shape[:2]
    step = tile_size - overlap
    tiles = []

    for y in range(0, h - tile_size + 1, step):
        for x in range(0, w - tile_size + 1, step):
            tile = img[y : y + tile_size, x : x + tile_size]
            tiles.append((tile, y, x))

    # Handle right/bottom edges if image is not perfectly tiled
    # Right edge column
    if (w - tile_size) % step != 0 and w >= tile_size:
        for y in range(0, h - tile_size + 1, step):
            x = w - tile_size
            tile = img[y : y + tile_size, x : x + tile_size]
            tiles.append((tile, y, x))
    # Bottom edge row
    if (h - tile_size) % step != 0 and h >= tile_size:
        for x in range(0, w - tile_size + 1, step):
            y = h - tile_size
            tile = img[y : y + tile_size, x : x + tile_size]
            tiles.append((tile, y, x))
    # Bottom-right corner
    if h >= tile_size and w >= tile_size:
        y = h - tile_size
        x = w - tile_size
        tile = img[y : y + tile_size, x : x + tile_size]
        tiles.append((tile, y, x))

    return tiles


# ---------------------------------------------------------------------------
# FITS processing
# ---------------------------------------------------------------------------

def process_fits_file(
    fits_path: str,
    output_dir: Path,
    tile_counter: int,
    tile_size: int = 512,
    overlap: int = 64,
) -> int:
    """
    Process a single FITS file: demosaic, stretch, tile, label.

    Returns the updated tile counter.
    """
    logger.info("Processing: %s", fits_path)

    with pyfits.open(fits_path) as hdul:
        # Find the image HDU
        data = None
        header = None
        for hdu in hdul:
            if hdu.data is not None and hdu.data.ndim == 2:
                data = hdu.data.astype(np.float32)
                header = hdu.header
                break

        if data is None:
            logger.warning("No 2D image data found in %s, skipping", fits_path)
            return tile_counter

    h, w = data.shape
    logger.info("  Image size: %d x %d", w, h)

    # Detect Bayer pattern
    pattern = detect_bayer_pattern(header)
    logger.info("  Bayer pattern: %s", pattern)

    # Normalise to [0, 1]
    dmin, dmax = data.min(), data.max()
    if dmax > dmin:
        data = (data - dmin) / (dmax - dmin)
    else:
        data = np.zeros_like(data)

    # Bilinear demosaic
    logger.info("  Demosaicing (%d x %d)...", w, h)
    rgb = bilinear_demosaic_fast(data, pattern)

    # Per-channel arcsinh stretch
    logger.info("  Applying arcsinh stretch...")
    rgb = arcsinh_stretch(rgb, scale=5.0)

    # Skip images that are too small to tile
    if h < tile_size or w < tile_size:
        logger.warning(
            "  Image too small for %dx%d tiles (%dx%d), skipping",
            tile_size,
            tile_size,
            w,
            h,
        )
        return tile_counter

    # Extract tiles
    tiles = extract_tiles(rgb, tile_size=tile_size, overlap=overlap)
    logger.info("  Extracted %d tiles", len(tiles))

    # Label and save each tile
    saved = 0
    for tile_img, row, col in tiles:
        mask = label_tile(tile_img)

        tile_name = f"tile_{tile_counter:06d}.npy"
        mask_name = f"mask_{tile_counter:06d}.npy"

        np.save(output_dir / tile_name, tile_img.astype(np.float32))
        np.save(output_dir / mask_name, mask.astype(np.int64))

        tile_counter += 1
        saved += 1

    logger.info("  Saved %d tile/mask pairs (counter now at %d)", saved, tile_counter)
    return tile_counter


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Process raw Bayer FITS files into 7-class training tiles"
    )
    parser.add_argument(
        "--input-dir",
        type=str,
        required=True,
        help="Directory containing raw FITS files",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        required=True,
        help="Output directory for tile/mask .npy files",
    )
    parser.add_argument(
        "--tile-size",
        type=int,
        default=512,
        help="Tile dimension in pixels (default: 512)",
    )
    parser.add_argument(
        "--overlap",
        type=int,
        default=64,
        help="Overlap between adjacent tiles in pixels (default: 64)",
    )
    parser.add_argument(
        "--start-index",
        type=int,
        default=0,
        help="Starting tile index (useful when appending to existing dataset)",
    )
    parser.add_argument(
        "--stretch-scale",
        type=float,
        default=5.0,
        help="Arcsinh stretch scale factor (default: 5.0)",
    )
    args = parser.parse_args()

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)

    if not input_dir.is_dir():
        sys.exit(f"ERROR: Input directory does not exist: {input_dir}")

    output_dir.mkdir(parents=True, exist_ok=True)

    # Find FITS files
    fits_extensions = {".fits", ".fit", ".fts", ".FITS", ".FIT"}
    fits_files = sorted(
        [f for f in input_dir.iterdir() if f.suffix in fits_extensions]
    )

    if not fits_files:
        sys.exit(f"ERROR: No FITS files found in {input_dir}")

    logger.info("Found %d FITS files in %s", len(fits_files), input_dir)
    logger.info("Output directory: %s", output_dir)
    logger.info("Tile size: %d, Overlap: %d", args.tile_size, args.overlap)

    tile_counter = args.start_index

    for fits_path in fits_files:
        try:
            tile_counter = process_fits_file(
                str(fits_path),
                output_dir,
                tile_counter,
                tile_size=args.tile_size,
                overlap=args.overlap,
            )
        except Exception as e:
            logger.error("Failed to process %s: %s", fits_path, e)
            continue

    logger.info("Processing complete. Total tiles: %d", tile_counter - args.start_index)


if __name__ == "__main__":
    main()
