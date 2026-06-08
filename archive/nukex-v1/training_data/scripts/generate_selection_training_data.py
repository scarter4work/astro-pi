#!/usr/bin/env python3
"""
Generate training data for the ML Pixel Selector model.

Takes a directory of aligned FITS subframes and a "ground truth" stacked image,
then extracts per-pixel feature vectors and labels for training the pixel
selection model.

For each sampled pixel position (x, y):
  - Reads values from all N frames
  - Computes distribution statistics (mu, sigma, skewness, kurtosis, etc.)
  - Reads segmentation class from a pre-computed segmentation map (optional)
  - Computes stack summary statistics
  - Labels: which frame's value is closest to the ground truth pixel

Outputs numpy arrays: features.npy, labels.npy, metadata.npy

Usage:
    python generate_selection_training_data.py \
        --frames-dir /path/to/aligned/fits \
        --ground-truth /path/to/stacked.fits \
        --output-dir /path/to/output \
        --num-samples 1000000 \
        --segmentation-map /path/to/segmentation.npy  (optional)

See docs/plans/2026-02-19-ml-pixel-selector-design.md for full design context.
"""

import argparse
import logging
import os
import sys
from pathlib import Path
from typing import Optional

import numpy as np

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants matching the C++ RegionClass enum (RegionStatistics.h)
# ---------------------------------------------------------------------------

NUM_REGION_CLASSES = 8
REGION_CLASS_NAMES = [
    "Background",       # 0
    "BrightCompact",    # 1
    "FaintCompact",     # 2
    "BrightExtended",   # 3
    "DarkExtended",     # 4
    "Artifact",         # 5
    "StarHalo",         # 6
    "Vignette",         # 7
]

NUM_DIST_TYPES = 5  # Gaussian, Lognormal, Skewed, Bimodal, Uniform

N_MAX = 64  # Maximum number of frames supported by the model

# Feature vector layout (must match train_pixel_selector.py):
#   Distribution stats: 10 (5 scalars + 5 one-hot dist type)
#   Segmentation context: 12 (8 one-hot class + confidence + 3 spatial)
#   Stack summary: 6
#   Per-frame candidates: N_MAX * 3 = 192
#   Frame mask: N_MAX = 64
#   Total: 284
GLOBAL_FEATURE_DIM = 28   # dist(10) + seg(12) + summary(6)
PER_FRAME_DIM = 3          # value, z_score, frame_weight
TOTAL_FEATURE_DIM = GLOBAL_FEATURE_DIM + N_MAX * PER_FRAME_DIM + N_MAX  # 284


# ---------------------------------------------------------------------------
# FITS I/O helpers
# ---------------------------------------------------------------------------

def load_fits_image(path: str) -> np.ndarray:
    """Load a FITS image and return as float32 numpy array.

    Returns array of shape (H, W) for mono or (H, W, C) for color.
    Values are normalized to [0, 1] range.
    """
    try:
        from astropy.io import fits
    except ImportError:
        logger.error("astropy is required: pip install astropy")
        sys.exit(1)

    with fits.open(path) as hdul:
        data = hdul[0].data.astype(np.float32)

    # Handle FITS axis ordering (C, H, W) -> (H, W, C) or just (H, W)
    if data.ndim == 3:
        data = np.transpose(data, (1, 2, 0))

    # Normalize to [0, 1]
    dmin, dmax = data.min(), data.max()
    if dmax > dmin:
        data = (data - dmin) / (dmax - dmin)
    else:
        data = np.zeros_like(data)

    return data


def load_all_frames(frames_dir: str, channel: int = 0) -> list[np.ndarray]:
    """Load all FITS files from a directory, returning single-channel arrays.

    Args:
        frames_dir: Directory containing .fits/.fit files
        channel: Which channel to extract (0=R/mono, 1=G, 2=B)

    Returns:
        List of 2D float32 arrays, all same shape (H, W)
    """
    fits_extensions = {".fits", ".fit", ".fts", ".FITS", ".FIT"}
    frame_paths = sorted(
        p for p in Path(frames_dir).iterdir()
        if p.suffix in fits_extensions
    )

    if not frame_paths:
        logger.error(f"No FITS files found in {frames_dir}")
        sys.exit(1)

    logger.info(f"Loading {len(frame_paths)} frames from {frames_dir}")

    frames = []
    for p in frame_paths:
        img = load_fits_image(str(p))
        if img.ndim == 3:
            if channel < img.shape[2]:
                img = img[:, :, channel]
            else:
                img = img[:, :, 0]
        frames.append(img)

    # Validate dimensions
    h, w = frames[0].shape
    for i, f in enumerate(frames):
        if f.shape != (h, w):
            logger.error(
                f"Frame {frame_paths[i]} has shape {f.shape}, "
                f"expected ({h}, {w})"
            )
            sys.exit(1)

    logger.info(f"Loaded {len(frames)} frames, each {w}x{h}")
    return frames


# ---------------------------------------------------------------------------
# Distribution fitting (mirrors C++ PixelStackAnalyzer logic)
# ---------------------------------------------------------------------------

def fit_distribution(values: np.ndarray) -> dict:
    """Fit distribution parameters to a 1D array of pixel values across frames.

    Mirrors the logic in PixelStackAnalyzer::FitDistribution.

    Returns dict with keys: mu, sigma, skewness, kurtosis, quality, dist_type
    """
    n = len(values)
    if n < 2:
        return {
            "mu": float(values[0]) if n == 1 else 0.0,
            "sigma": 0.0,
            "skewness": 0.0,
            "kurtosis": 0.0,
            "quality": 0.0,
            "dist_type": 0,  # Gaussian
        }

    # Robust estimators (matching C++ useRobustEstimators=true)
    median = float(np.median(values))
    mad = float(np.median(np.abs(values - median)))
    sigma = mad * 1.4826  # MAD to sigma conversion

    if sigma < 1e-10:
        sigma = float(np.std(values))

    mu = median

    # Compute higher moments
    if sigma > 1e-10:
        z = (values - mu) / sigma
        skewness = float(np.mean(z ** 3))
        kurtosis = float(np.mean(z ** 4)) - 3.0  # Excess kurtosis
    else:
        skewness = 0.0
        kurtosis = 0.0

    # Determine distribution type (mirrors C++ DetermineDistributionType)
    # 0=Gaussian, 1=Lognormal, 2=Skewed, 3=Bimodal, 4=Uniform
    skew_thresh = 0.5
    bimodal_thresh = 0.55

    # Bimodality coefficient (Sarle's)
    if n >= 3 and sigma > 1e-10:
        bimodality = (skewness ** 2 + 1) / (kurtosis + 3 * (n - 1) ** 2 / ((n - 2) * (n - 3)) + 1e-10)
    else:
        bimodality = 0.0

    if bimodality > bimodal_thresh:
        dist_type = 3  # Bimodal
    elif abs(skewness) > skew_thresh:
        if skewness > 0:
            dist_type = 1  # Lognormal (right-skewed)
        else:
            dist_type = 2  # Skewed (left-skewed)
    elif sigma / max(abs(mu), 1e-10) > 0.5:
        dist_type = 4  # Uniform (high CV)
    else:
        dist_type = 0  # Gaussian

    # Quality: based on fit residuals (simplified)
    if sigma > 1e-10:
        z = (values - mu) / sigma
        # Fraction within 2 sigma as quality proxy
        quality = float(np.mean(np.abs(z) < 2.0))
    else:
        quality = 1.0

    return {
        "mu": mu,
        "sigma": sigma,
        "skewness": skewness,
        "kurtosis": kurtosis,
        "quality": quality,
        "dist_type": dist_type,
    }


# ---------------------------------------------------------------------------
# Feature extraction
# ---------------------------------------------------------------------------

def extract_features(
    pixel_values: np.ndarray,
    seg_class: int,
    seg_confidence: float,
    neighbor_classes: np.ndarray,
    is_transition: bool,
    frame_weights: Optional[np.ndarray] = None,
) -> np.ndarray:
    """Extract the full feature vector for one pixel position.

    Args:
        pixel_values: float32 array of shape (N,) -- values from N frames
        seg_class: Segmentation class ID (0-7), or -1 if unavailable
        seg_confidence: Confidence in segmentation (0-1)
        neighbor_classes: int array of shape (8,) -- 8-neighbor classes (-1 if edge)
        is_transition: Whether this is a transition zone
        frame_weights: Optional float32 array of shape (N,) -- per-frame quality weights

    Returns:
        float32 array of shape (TOTAL_FEATURE_DIM,)
    """
    n_frames = len(pixel_values)
    features = np.zeros(TOTAL_FEATURE_DIM, dtype=np.float32)

    # --- Distribution statistics (10 floats) ---
    dist = fit_distribution(pixel_values)
    offset = 0

    features[offset + 0] = dist["mu"]
    features[offset + 1] = dist["sigma"]
    features[offset + 2] = dist["skewness"]
    features[offset + 3] = dist["kurtosis"]
    features[offset + 4] = dist["quality"]
    # One-hot distribution type (5 floats)
    if 0 <= dist["dist_type"] < NUM_DIST_TYPES:
        features[offset + 5 + dist["dist_type"]] = 1.0
    offset += 10

    # --- Segmentation context (12 floats) ---
    # One-hot class (8 floats)
    if 0 <= seg_class < NUM_REGION_CLASSES:
        features[offset + seg_class] = 1.0
    offset += NUM_REGION_CLASSES

    features[offset + 0] = seg_confidence

    # Spatial context
    valid_neighbors = neighbor_classes[neighbor_classes >= 0]
    if len(valid_neighbors) > 0 and seg_class >= 0:
        matching = float(np.sum(valid_neighbors == seg_class))
        features[offset + 1] = matching / 8.0  # num_matching_neighbors (normalized)
    features[offset + 2] = float(is_transition)

    # Dominant neighbor matches center
    if len(valid_neighbors) > 0 and seg_class >= 0:
        counts = np.bincount(valid_neighbors.astype(np.int32), minlength=NUM_REGION_CLASSES)
        dominant = int(np.argmax(counts))
        features[offset + 3] = float(dominant == seg_class)
    offset += 4

    # --- Stack summary statistics (6 floats) ---
    sorted_vals = np.sort(pixel_values)
    n = len(sorted_vals)
    q25_idx = max(0, int(n * 0.25))
    q75_idx = min(n - 1, int(n * 0.75))

    features[offset + 0] = float(np.median(pixel_values))  # stack_median
    features[offset + 1] = float(sorted_vals[q75_idx] - sorted_vals[q25_idx])  # stack_iqr
    features[offset + 2] = float(sorted_vals[-1] - sorted_vals[0])  # stack_range

    # Outlier fraction (using 3-sigma from robust stats)
    mu, sigma = dist["mu"], max(dist["sigma"], 1e-10)
    outlier_mask = np.abs(pixel_values - mu) > 3.0 * sigma
    features[offset + 3] = float(np.mean(outlier_mask))  # outlier_fraction
    features[offset + 4] = float(min(n, N_MAX)) / float(N_MAX)  # valid_frame_count (normalized)

    if frame_weights is not None and len(frame_weights) == n:
        features[offset + 5] = float(np.average(pixel_values, weights=frame_weights))
    else:
        features[offset + 5] = float(np.mean(pixel_values))
    offset += 6

    assert offset == GLOBAL_FEATURE_DIM, f"Global feature offset mismatch: {offset} != {GLOBAL_FEATURE_DIM}"

    # --- Per-frame candidate features (N_MAX x 3) ---
    n_use = min(n_frames, N_MAX)
    for i in range(n_use):
        base = GLOBAL_FEATURE_DIM + i * PER_FRAME_DIM
        features[base + 0] = pixel_values[i]
        features[base + 1] = (pixel_values[i] - mu) / max(sigma, 1e-10)  # z_score
        if frame_weights is not None and i < len(frame_weights):
            features[base + 2] = frame_weights[i]
        else:
            features[base + 2] = 1.0

    # --- Frame mask (N_MAX) ---
    mask_offset = GLOBAL_FEATURE_DIM + N_MAX * PER_FRAME_DIM
    for i in range(n_use):
        features[mask_offset + i] = 1.0

    return features


def compute_soft_label(
    pixel_values: np.ndarray,
    ground_truth_value: float,
    noise_estimate: float = 0.01,
) -> np.ndarray:
    """Compute soft label distribution over frames.

    Instead of a hard one-hot label (which frame is "correct"), we compute
    a softmax over negative distances, with temperature = noise_estimate.
    This handles label ambiguity when multiple frames have similar values.

    Args:
        pixel_values: float32 array of shape (N,)
        ground_truth_value: The ground truth pixel value
        noise_estimate: Temperature for softmax (estimated noise level)

    Returns:
        float32 array of shape (N_MAX,) -- soft label distribution, zero-padded
    """
    n = len(pixel_values)
    label = np.zeros(N_MAX, dtype=np.float32)

    distances = np.abs(pixel_values - ground_truth_value)
    temperature = max(noise_estimate, 1e-6)

    # Softmax over negative distances
    logits = -distances / temperature
    logits -= logits.max()  # Numerical stability
    exp_logits = np.exp(logits)
    soft_dist = exp_logits / (exp_logits.sum() + 1e-10)

    n_use = min(n, N_MAX)
    label[:n_use] = soft_dist[:n_use]

    return label


# ---------------------------------------------------------------------------
# Spatial context helpers
# ---------------------------------------------------------------------------

def compute_neighbor_classes(
    seg_map: Optional[np.ndarray],
    x: int,
    y: int,
) -> tuple[np.ndarray, bool]:
    """Get 8-neighbor segmentation classes and transition zone flag.

    Args:
        seg_map: 2D int array (H, W) of class labels, or None
        x, y: Pixel coordinates

    Returns:
        (neighbor_classes[8], is_transition_zone)
    """
    if seg_map is None:
        return np.full(8, -1, dtype=np.int32), False

    h, w = seg_map.shape
    center_class = int(seg_map[y, x])

    # 8-connected neighbors (clockwise from top)
    dx = [0, 1, 1, 1, 0, -1, -1, -1]
    dy = [-1, -1, 0, 1, 1, 1, 0, -1]

    neighbors = np.full(8, -1, dtype=np.int32)
    matching = 0

    for i in range(8):
        nx, ny = x + dx[i], y + dy[i]
        if 0 <= nx < w and 0 <= ny < h:
            neighbors[i] = int(seg_map[ny, nx])
            if neighbors[i] == center_class:
                matching += 1

    is_transition = matching < 5

    return neighbors, is_transition


# ---------------------------------------------------------------------------
# Sampling strategies
# ---------------------------------------------------------------------------

def generate_sample_positions(
    height: int,
    width: int,
    num_samples: int,
    seg_map: Optional[np.ndarray] = None,
    pixel_variance: Optional[np.ndarray] = None,
    seed: int = 42,
) -> np.ndarray:
    """Generate pixel positions to sample, with stratified sampling.

    Sampling priorities:
      - 50% uniform random
      - 20% transition zones (where segmentation class changes)
      - 15% high-variance pixels (where frames disagree most)
      - 15% class-stratified (ensure rare classes are represented)

    Args:
        height, width: Image dimensions
        num_samples: Total number of samples to generate
        seg_map: Optional 2D segmentation map for stratified sampling
        pixel_variance: Optional 2D variance map for importance sampling
        seed: Random seed

    Returns:
        int32 array of shape (num_samples, 2) -- (x, y) coordinates
    """
    rng = np.random.RandomState(seed)

    # Clamp to available pixels (minus 1-pixel border for neighbor access)
    max_samples = (height - 2) * (width - 2)
    num_samples = min(num_samples, max_samples)

    # Allocate target counts per strategy
    n_uniform = int(num_samples * 0.50)
    n_transition = int(num_samples * 0.20)
    n_highvar = int(num_samples * 0.15)
    n_stratified = num_samples - n_uniform - n_transition - n_highvar

    all_positions = []

    # 1. Uniform random sampling
    ys = rng.randint(1, height - 1, size=n_uniform)
    xs = rng.randint(1, width - 1, size=n_uniform)
    all_positions.append(np.stack([xs, ys], axis=1))

    # 2. Transition zone sampling
    if seg_map is not None and n_transition > 0:
        # Find transition pixels: where center != at least 3 of 8 neighbors
        transition_mask = np.zeros((height, width), dtype=bool)
        for dy in range(-1, 2):
            for dx in range(-1, 2):
                if dx == 0 and dy == 0:
                    continue
                shifted = np.roll(np.roll(seg_map, -dy, axis=0), -dx, axis=1)
                transition_mask |= (seg_map != shifted)

        # Exclude border
        transition_mask[0, :] = False
        transition_mask[-1, :] = False
        transition_mask[:, 0] = False
        transition_mask[:, -1] = False

        trans_ys, trans_xs = np.where(transition_mask)
        if len(trans_ys) > 0:
            idx = rng.choice(len(trans_ys), size=min(n_transition, len(trans_ys)), replace=False)
            all_positions.append(np.stack([trans_xs[idx], trans_ys[idx]], axis=1))
        else:
            # Fall back to uniform
            ys = rng.randint(1, height - 1, size=n_transition)
            xs = rng.randint(1, width - 1, size=n_transition)
            all_positions.append(np.stack([xs, ys], axis=1))
    else:
        ys = rng.randint(1, height - 1, size=n_transition)
        xs = rng.randint(1, width - 1, size=n_transition)
        all_positions.append(np.stack([xs, ys], axis=1))

    # 3. High-variance sampling
    if pixel_variance is not None and n_highvar > 0:
        # Sample proportional to variance
        var_flat = pixel_variance[1:-1, 1:-1].flatten()
        var_probs = var_flat / (var_flat.sum() + 1e-10)
        idx = rng.choice(len(var_flat), size=min(n_highvar, len(var_flat)),
                         replace=False, p=var_probs)
        inner_w = width - 2
        ys = (idx // inner_w) + 1
        xs = (idx % inner_w) + 1
        all_positions.append(np.stack([xs, ys], axis=1))
    else:
        ys = rng.randint(1, height - 1, size=n_highvar)
        xs = rng.randint(1, width - 1, size=n_highvar)
        all_positions.append(np.stack([xs, ys], axis=1))

    # 4. Class-stratified sampling
    if seg_map is not None and n_stratified > 0:
        per_class = max(1, n_stratified // NUM_REGION_CLASSES)
        for c in range(NUM_REGION_CLASSES):
            class_ys, class_xs = np.where(seg_map == c)
            # Exclude border
            valid = (class_ys > 0) & (class_ys < height - 1) & (class_xs > 0) & (class_xs < width - 1)
            class_ys, class_xs = class_ys[valid], class_xs[valid]
            if len(class_ys) > 0:
                n_take = min(per_class, len(class_ys))
                idx = rng.choice(len(class_ys), size=n_take, replace=False)
                all_positions.append(np.stack([class_xs[idx], class_ys[idx]], axis=1))
    else:
        ys = rng.randint(1, height - 1, size=n_stratified)
        xs = rng.randint(1, width - 1, size=n_stratified)
        all_positions.append(np.stack([xs, ys], axis=1))

    positions = np.concatenate(all_positions, axis=0)

    # Deduplicate
    _, unique_idx = np.unique(positions[:, 0] * height + positions[:, 1], return_index=True)
    positions = positions[unique_idx]

    # Trim to requested count
    if len(positions) > num_samples:
        idx = rng.choice(len(positions), size=num_samples, replace=False)
        positions = positions[idx]

    logger.info(f"Generated {len(positions)} sample positions")
    return positions.astype(np.int32)


# ---------------------------------------------------------------------------
# Main data generation pipeline
# ---------------------------------------------------------------------------

def generate_training_data(
    frames_dir: str,
    ground_truth_path: str,
    output_dir: str,
    num_samples: int = 1_000_000,
    channel: int = 0,
    segmentation_map_path: Optional[str] = None,
    noise_estimate: float = 0.01,
    seed: int = 42,
):
    """Main pipeline: load frames, sample pixels, extract features, write output.

    Args:
        frames_dir: Directory of aligned FITS subframes
        ground_truth_path: Path to ground truth stacked FITS image
        output_dir: Where to save numpy output files
        num_samples: Number of pixel positions to sample
        channel: Channel to process (0=R/mono)
        segmentation_map_path: Optional path to .npy segmentation map
        noise_estimate: Estimated noise level for soft labels
        seed: Random seed for reproducibility
    """
    os.makedirs(output_dir, exist_ok=True)

    # Load frames
    frames = load_all_frames(frames_dir, channel=channel)
    n_frames = len(frames)
    height, width = frames[0].shape
    logger.info(f"Stack: {n_frames} frames, {width}x{height}, channel={channel}")

    if n_frames > N_MAX:
        logger.warning(
            f"Frame count ({n_frames}) exceeds N_MAX ({N_MAX}). "
            f"Only first {N_MAX} frames will be used."
        )

    # Load ground truth
    gt = load_fits_image(ground_truth_path)
    if gt.ndim == 3:
        if channel < gt.shape[2]:
            gt = gt[:, :, channel]
        else:
            gt = gt[:, :, 0]

    if gt.shape != (height, width):
        logger.error(
            f"Ground truth shape {gt.shape} does not match "
            f"frame shape ({height}, {width})"
        )
        sys.exit(1)

    # Load segmentation map (optional)
    seg_map = None
    if segmentation_map_path and os.path.exists(segmentation_map_path):
        seg_map = np.load(segmentation_map_path)
        if seg_map.shape != (height, width):
            logger.warning(
                f"Segmentation map shape {seg_map.shape} != image shape. "
                f"Resizing with nearest-neighbor interpolation."
            )
            from scipy.ndimage import zoom
            zoom_y = height / seg_map.shape[0]
            zoom_x = width / seg_map.shape[1]
            seg_map = zoom(seg_map, (zoom_y, zoom_x), order=0).astype(np.int32)
        logger.info(f"Loaded segmentation map: {seg_map.shape}")

    # Compute per-pixel variance across stack (for importance sampling)
    logger.info("Computing per-pixel variance across stack...")
    stack = np.stack(frames[:min(n_frames, N_MAX)], axis=0)  # (N, H, W)
    pixel_variance = np.var(stack, axis=0)  # (H, W)

    # Generate sample positions
    positions = generate_sample_positions(
        height, width, num_samples,
        seg_map=seg_map,
        pixel_variance=pixel_variance,
        seed=seed,
    )
    actual_samples = len(positions)

    # Pre-allocate output arrays
    features_all = np.zeros((actual_samples, TOTAL_FEATURE_DIM), dtype=np.float32)
    labels_all = np.zeros((actual_samples, N_MAX), dtype=np.float32)
    metadata_all = np.zeros((actual_samples, 4), dtype=np.float32)  # x, y, hard_label, gt_value

    logger.info(f"Extracting features for {actual_samples} samples...")

    n_use = min(n_frames, N_MAX)

    for i, (x, y) in enumerate(positions):
        if i % 100_000 == 0 and i > 0:
            logger.info(f"  Progress: {i}/{actual_samples} ({100*i/actual_samples:.1f}%)")

        # Collect pixel values from all frames
        pixel_values = np.array([frames[f][y, x] for f in range(n_use)], dtype=np.float32)

        # Get segmentation info
        seg_class = int(seg_map[y, x]) if seg_map is not None else 0
        seg_confidence = 1.0 if seg_map is not None else 0.0

        # Compute neighbor classes
        neighbor_classes, is_transition = compute_neighbor_classes(seg_map, x, y)

        # Extract features
        features_all[i] = extract_features(
            pixel_values=pixel_values,
            seg_class=seg_class,
            seg_confidence=seg_confidence,
            neighbor_classes=neighbor_classes,
            is_transition=is_transition,
            frame_weights=None,
        )

        # Compute label
        gt_val = float(gt[y, x])
        labels_all[i] = compute_soft_label(pixel_values, gt_val, noise_estimate)

        # Hard label (for metrics)
        hard_label = int(np.argmin(np.abs(pixel_values - gt_val)))

        # Metadata
        metadata_all[i] = [float(x), float(y), float(hard_label), gt_val]

    # Save outputs
    features_path = os.path.join(output_dir, "features.npy")
    labels_path = os.path.join(output_dir, "labels.npy")
    metadata_path = os.path.join(output_dir, "metadata.npy")

    np.save(features_path, features_all)
    np.save(labels_path, labels_all)
    np.save(metadata_path, metadata_all)

    logger.info(f"Saved {actual_samples} samples:")
    logger.info(f"  Features: {features_path} ({features_all.shape})")
    logger.info(f"  Labels:   {labels_path} ({labels_all.shape})")
    logger.info(f"  Metadata: {metadata_path} ({metadata_all.shape})")

    # Print summary statistics
    hard_labels = metadata_all[:, 2].astype(int)
    logger.info(f"Hard label distribution (top 5 frames):")
    counts = np.bincount(hard_labels, minlength=n_use)
    for f in np.argsort(-counts)[:5]:
        logger.info(f"  Frame {f}: {counts[f]} samples ({100*counts[f]/actual_samples:.1f}%)")

    if seg_map is not None:
        seg_at_samples = np.array([seg_map[y, x] for x, y in positions])
        logger.info(f"Class distribution in samples:")
        for c in range(NUM_REGION_CLASSES):
            n_c = np.sum(seg_at_samples == c)
            if n_c > 0:
                logger.info(f"  {REGION_CLASS_NAMES[c]}: {n_c} ({100*n_c/actual_samples:.1f}%)")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate training data for ML Pixel Selector",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument(
        "--frames-dir", required=True,
        help="Directory containing aligned FITS subframes",
    )
    parser.add_argument(
        "--ground-truth", required=True,
        help="Path to ground truth stacked FITS image",
    )
    parser.add_argument(
        "--output-dir", required=True,
        help="Output directory for numpy training data",
    )
    parser.add_argument(
        "--num-samples", type=int, default=1_000_000,
        help="Number of pixel positions to sample",
    )
    parser.add_argument(
        "--channel", type=int, default=0,
        help="Channel to process (0=R/mono, 1=G, 2=B)",
    )
    parser.add_argument(
        "--segmentation-map", default=None,
        help="Path to .npy segmentation map (optional, improves stratified sampling)",
    )
    parser.add_argument(
        "--noise-estimate", type=float, default=0.01,
        help="Estimated noise level for soft label temperature",
    )
    parser.add_argument(
        "--seed", type=int, default=42,
        help="Random seed for reproducibility",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Enable verbose logging",
    )

    return parser.parse_args()


def main():
    args = parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    generate_training_data(
        frames_dir=args.frames_dir,
        ground_truth_path=args.ground_truth,
        output_dir=args.output_dir,
        num_samples=args.num_samples,
        channel=args.channel,
        segmentation_map_path=args.segmentation_map,
        noise_estimate=args.noise_estimate,
        seed=args.seed,
    )


if __name__ == "__main__":
    main()
