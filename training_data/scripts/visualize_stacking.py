#!/usr/bin/env python3
"""
Diagnostic Visualization Tools for NukeX Intelligent Stacking System

Generates visualization maps for debugging and validating the intelligent
pixel selection system:

1. Source Frame Map - Which frame contributed each pixel (false color)
2. Selection Confidence Map - How confident was the selection
3. Class Strategy Map - What selection strategy was used at each pixel
4. Outlier Rejection Map - How many frames were rejected at each pixel
5. Multi-panel Summary Figure - Overview of all diagnostics

The script works with real stacking output or generates synthetic data
for testing the visualization infrastructure.

Usage:
    # With real stacking output
    python visualize_stacking.py --result integrated.fits --metadata metadata.bin --output diagnostics/

    # Generate synthetic test data
    python visualize_stacking.py --synthetic --output diagnostics/

    # Specify image dimensions for synthetic mode
    python visualize_stacking.py --synthetic --width 1024 --height 1024 --num-frames 20 --output diagnostics/

Copyright (c) 2026 Scott Carter
"""

import argparse
import struct
import sys
from pathlib import Path
from typing import Optional, Tuple, Dict, List
import numpy as np

# Use non-interactive backend for headless operation
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap, BoundaryNorm, LinearSegmentedColormap
from matplotlib.patches import Patch
import matplotlib.gridspec as gridspec

# Import segmentation palette for class colors
try:
    from segmentation_palette import (
        NUM_CLASSES as SEG_NUM_CLASSES,
        CLASS_NAMES,
        CLASS_DISPLAY_NAMES,
        CLASS_COLORS_RGB,
        CLASS_COLORS_NORM,
    )
    HAS_PALETTE = True
except ImportError:
    HAS_PALETTE = False
    print("Warning: segmentation_palette not found, using default colors")


# --------------------------------------------------------------------------
# Constants for stacking metadata
# --------------------------------------------------------------------------

# Distribution types (match StackDistributionType enum)
DISTRIBUTION_TYPES = [
    'Gaussian',      # 0
    'Lognormal',     # 1
    'Skewed',        # 2
    'Bimodal',       # 3
    'Uniform',       # 4
]

# Selection strategies (derived from distribution + class)
SELECTION_STRATEGIES = [
    'Median',           # 0: Default, select closest to median
    'FavorHigh',        # 1: For emission nebulae, favor higher signal
    'FavorLow',         # 2: For dark nebulae, preserve low values
    'Aggressive',       # 3: High sigma rejection for artifacts
    'Conservative',     # 4: Low rejection for transitions
    'WeightedMean',     # 5: Weighted by confidence
    'Maximum',          # 6: For star cores (preserve peaks)
    'Minimum',          # 7: For dark regions (preserve minima)
]

# Strategy colors for visualization
STRATEGY_COLORS = [
    (0.5, 0.5, 0.5),    # 0: Median - gray
    (1.0, 0.4, 0.4),    # 1: FavorHigh - red
    (0.2, 0.2, 0.6),    # 2: FavorLow - dark blue
    (1.0, 0.8, 0.0),    # 3: Aggressive - yellow
    (0.6, 1.0, 0.6),    # 4: Conservative - light green
    (0.8, 0.5, 0.8),    # 5: WeightedMean - purple
    (1.0, 1.0, 0.4),    # 6: Maximum - bright yellow
    (0.2, 0.4, 0.8),    # 7: Minimum - blue
]


# --------------------------------------------------------------------------
# Data structures
# --------------------------------------------------------------------------

class PixelStackMetadata:
    """
    Python representation of C++ PixelStackMetadata structure.

    Matches the binary layout:
    - StackDistributionParams (24 bytes)
    - selectedValue (4 bytes, float)
    - sourceFrame (2 bytes, uint16)
    - confidence (4 bytes, float)
    - outlierMask (4 bytes, uint32)
    """

    # Distribution params
    dist_type: int = 0
    mu: float = 0.0
    sigma: float = 0.0
    skewness: float = 0.0
    kurtosis: float = 0.0
    quality: float = 0.0

    # Selection result
    selected_value: float = 0.0
    source_frame: int = 0
    confidence: float = 0.0
    outlier_mask: int = 0

    # ML class (optional)
    ml_class: int = 0

    def is_outlier(self, frame_index: int) -> bool:
        """Check if a frame was marked as outlier."""
        return (frame_index < 32) and ((self.outlier_mask & (1 << frame_index)) != 0)

    def outlier_count(self) -> int:
        """Count number of rejected frames."""
        return bin(self.outlier_mask).count('1')

    def get_strategy(self) -> int:
        """Infer selection strategy from distribution and class."""
        # This is a simplified heuristic - real strategy is determined by class
        if self.ml_class == 0:  # Background
            return 0  # Median
        elif self.ml_class in [5]:  # Emission nebula
            return 1  # FavorHigh
        elif self.ml_class in [7, 13]:  # Dark nebula, dust lane
            return 2  # FavorLow
        elif self.ml_class in [16, 17, 18]:  # Artifacts
            return 3  # Aggressive
        elif self.ml_class in [1, 4]:  # Bright/saturated stars
            return 6  # Maximum
        else:
            return 0  # Default median


class StackingMetadata:
    """
    Container for full image stacking metadata.

    Stores per-pixel metadata for the entire integrated image.
    """

    def __init__(self, width: int, height: int, num_frames: int = 0):
        self.width = width
        self.height = height
        self.num_frames = num_frames

        # Per-pixel arrays (H x W)
        self.source_frame = np.zeros((height, width), dtype=np.uint16)
        self.confidence = np.zeros((height, width), dtype=np.float32)
        self.outlier_count = np.zeros((height, width), dtype=np.uint8)
        self.outlier_mask = np.zeros((height, width), dtype=np.uint32)
        self.ml_class = np.zeros((height, width), dtype=np.uint8)
        self.strategy = np.zeros((height, width), dtype=np.uint8)

        # Distribution parameters (optional, for detailed analysis)
        self.dist_type = np.zeros((height, width), dtype=np.uint8)
        self.mu = np.zeros((height, width), dtype=np.float32)
        self.sigma = np.zeros((height, width), dtype=np.float32)

    @classmethod
    def from_binary(cls, filepath: str) -> 'StackingMetadata':
        """
        Load metadata from binary file.

        Binary format:
        - Header: width (4 bytes), height (4 bytes), num_frames (4 bytes)
        - Per-pixel data: H x W x PixelStackMetadata structures
        """
        with open(filepath, 'rb') as f:
            # Read header
            width, height, num_frames = struct.unpack('<III', f.read(12))

            meta = cls(width, height, num_frames)

            # Read per-pixel data
            # This assumes packed structure - adjust based on actual binary format
            for y in range(height):
                for x in range(width):
                    # Read StackDistributionParams (24 bytes)
                    dist_type = struct.unpack('<B', f.read(1))[0]
                    f.read(3)  # padding
                    mu, sigma, skewness, kurtosis, quality = struct.unpack('<5f', f.read(20))

                    # Read selection result
                    selected_value = struct.unpack('<f', f.read(4))[0]
                    source_frame = struct.unpack('<H', f.read(2))[0]
                    f.read(2)  # padding
                    confidence = struct.unpack('<f', f.read(4))[0]
                    outlier_mask = struct.unpack('<I', f.read(4))[0]

                    # Store in arrays
                    meta.source_frame[y, x] = source_frame
                    meta.confidence[y, x] = confidence
                    meta.outlier_mask[y, x] = outlier_mask
                    meta.outlier_count[y, x] = bin(outlier_mask).count('1')
                    meta.dist_type[y, x] = dist_type
                    meta.mu[y, x] = mu
                    meta.sigma[y, x] = sigma

            return meta

    @classmethod
    def from_numpy(cls,
                   source_frame: np.ndarray,
                   confidence: np.ndarray,
                   outlier_mask: np.ndarray,
                   ml_class: Optional[np.ndarray] = None,
                   num_frames: int = 20) -> 'StackingMetadata':
        """
        Create metadata from numpy arrays (for testing or Python-based stacking).
        """
        height, width = source_frame.shape
        meta = cls(width, height, num_frames)

        meta.source_frame = source_frame.astype(np.uint16)
        meta.confidence = confidence.astype(np.float32)
        meta.outlier_mask = outlier_mask.astype(np.uint32)

        # Compute outlier count
        for y in range(height):
            for x in range(width):
                meta.outlier_count[y, x] = bin(int(meta.outlier_mask[y, x])).count('1')

        if ml_class is not None:
            meta.ml_class = ml_class.astype(np.uint8)
            # Compute strategy from class
            for y in range(height):
                for x in range(width):
                    meta.strategy[y, x] = _infer_strategy(meta.ml_class[y, x])

        return meta


def _infer_strategy(ml_class: int) -> int:
    """Infer selection strategy from ML class."""
    if ml_class == 0:  # Background
        return 0  # Median
    elif ml_class in [5]:  # Emission nebula
        return 1  # FavorHigh
    elif ml_class in [7, 13]:  # Dark nebula, dust lane
        return 2  # FavorLow
    elif ml_class in [16, 17, 18, 19, 20]:  # Artifacts
        return 3  # Aggressive
    elif ml_class in [1, 4]:  # Bright/saturated stars
        return 6  # Maximum
    elif ml_class in [2, 3]:  # Medium/faint stars
        return 0  # Median
    elif ml_class in [6]:  # Reflection nebula
        return 4  # Conservative
    elif ml_class in [9, 10, 11, 12]:  # Galaxies
        return 5  # WeightedMean
    else:
        return 0  # Default


# --------------------------------------------------------------------------
# Synthetic data generation
# --------------------------------------------------------------------------

def generate_synthetic_metadata(width: int, height: int, num_frames: int = 20,
                                seed: int = 42) -> StackingMetadata:
    """
    Generate synthetic stacking metadata for testing visualization.

    Creates realistic-looking patterns:
    - Star regions with high confidence, specific source frames
    - Nebula regions with varied confidence
    - Background with median selection
    - Some outlier regions (simulating clouds/satellites)
    """
    np.random.seed(seed)

    meta = StackingMetadata(width, height, num_frames)

    # Create coordinate grids
    y_coords, x_coords = np.mgrid[0:height, 0:width]
    center_y, center_x = height // 2, width // 2

    # Generate ML class map with realistic regions
    ml_class = np.zeros((height, width), dtype=np.uint8)

    # Background (class 0)
    ml_class[:] = 0

    # Add emission nebula (class 5) - central bright region
    dist_from_center = np.sqrt((x_coords - center_x)**2 + (y_coords - center_y)**2)
    nebula_mask = dist_from_center < min(width, height) * 0.3
    ml_class[nebula_mask] = 5

    # Add dark nebula (class 7) - off-center dark region
    dark_center_x = center_x - width // 4
    dark_center_y = center_y + height // 4
    dark_dist = np.sqrt((x_coords - dark_center_x)**2 + (y_coords - dark_center_y)**2)
    dark_mask = dark_dist < min(width, height) * 0.15
    ml_class[dark_mask] = 7

    # Add some stars (classes 1-4)
    num_stars = max(20, (width * height) // 5000)
    for _ in range(num_stars):
        sx = np.random.randint(10, width - 10)
        sy = np.random.randint(10, height - 10)
        star_class = np.random.choice([1, 2, 3, 4], p=[0.2, 0.4, 0.3, 0.1])
        star_radius = 5 if star_class <= 2 else 3

        for dy in range(-star_radius, star_radius + 1):
            for dx in range(-star_radius, star_radius + 1):
                if 0 <= sy + dy < height and 0 <= sx + dx < width:
                    if dx*dx + dy*dy <= star_radius*star_radius:
                        ml_class[sy + dy, sx + dx] = star_class

    # Add satellite trail (class 17)
    trail_y = height // 3
    for x in range(width):
        for dy in range(-2, 3):
            if 0 <= trail_y + dy < height:
                ml_class[trail_y + dy, x] = 17

    # Add hot pixels (class 16)
    num_hot = max(5, (width * height) // 10000)
    for _ in range(num_hot):
        hx = np.random.randint(0, width)
        hy = np.random.randint(0, height)
        ml_class[hy, hx] = 16

    meta.ml_class = ml_class

    # Generate source frame map
    # Background: mostly from middle frames (less variation)
    # Nebula: from frames with best signal
    # Stars: from frames with best seeing
    base_frame = np.random.randint(num_frames // 3, 2 * num_frames // 3, (height, width)).astype(np.uint16)

    # Stars should come from specific "best" frames (simulate best seeing)
    best_seeing_frames = [3, 7, 12, 18]
    star_mask = (ml_class >= 1) & (ml_class <= 4)
    for y in range(height):
        for x in range(width):
            if star_mask[y, x]:
                base_frame[y, x] = np.random.choice(best_seeing_frames)

    # Emission nebula prefers high-signal frames
    best_signal_frames = [5, 8, 11, 15]
    emission_mask = ml_class == 5
    for y in range(height):
        for x in range(width):
            if emission_mask[y, x]:
                base_frame[y, x] = np.random.choice(best_signal_frames)

    # Artifacts get rejected in most frames
    artifact_mask = (ml_class >= 16)
    base_frame[artifact_mask] = 0  # First frame where artifact exists

    meta.source_frame = base_frame

    # Generate confidence map
    confidence = np.ones((height, width), dtype=np.float32) * 0.8

    # High confidence in nebula cores
    confidence[emission_mask] = np.clip(
        0.9 + np.random.normal(0, 0.05, np.sum(emission_mask)),
        0.7, 1.0
    )

    # High confidence in stars
    confidence[star_mask] = np.clip(
        0.95 + np.random.normal(0, 0.03, np.sum(star_mask)),
        0.85, 1.0
    )

    # Lower confidence in dark regions
    confidence[dark_mask] = np.clip(
        0.6 + np.random.normal(0, 0.1, np.sum(dark_mask)),
        0.3, 0.8
    )

    # Low confidence for artifacts (we reject them but aren't sure)
    confidence[artifact_mask] = np.clip(
        0.4 + np.random.normal(0, 0.15, np.sum(artifact_mask)),
        0.1, 0.6
    )

    # Background has medium confidence
    background_mask = ml_class == 0
    confidence[background_mask] = np.clip(
        0.75 + np.random.normal(0, 0.08, np.sum(background_mask)),
        0.5, 0.9
    )

    meta.confidence = confidence

    # Generate outlier mask
    outlier_mask = np.zeros((height, width), dtype=np.uint32)

    # Random outliers in background (clouds, etc.) - affect ~5% of pixels
    random_outlier_mask = np.random.random((height, width)) < 0.05
    # Each affected pixel has 1-3 frames rejected
    for y in range(height):
        for x in range(width):
            if random_outlier_mask[y, x] and ml_class[y, x] == 0:
                num_outliers = np.random.randint(1, 4)
                rejected_frames = np.random.choice(num_frames, num_outliers, replace=False)
                for f in rejected_frames:
                    outlier_mask[y, x] |= (1 << f)

    # Satellite trail should be rejected in all but 1-2 frames
    trail_mask = ml_class == 17
    for y in range(height):
        for x in range(width):
            if trail_mask[y, x]:
                # Reject all frames except where trail exists (frames 5-7)
                for f in range(num_frames):
                    if f < 5 or f > 7:
                        outlier_mask[y, x] |= (1 << f)

    # Hot pixels rejected in frames where they appear
    hot_mask = ml_class == 16
    for y in range(height):
        for x in range(width):
            if hot_mask[y, x]:
                # Random frames have hot pixel
                num_affected = np.random.randint(1, num_frames // 2)
                affected_frames = np.random.choice(num_frames, num_affected, replace=False)
                for f in affected_frames:
                    outlier_mask[y, x] |= (1 << f)

    meta.outlier_mask = outlier_mask

    # Compute outlier count
    for y in range(height):
        for x in range(width):
            meta.outlier_count[y, x] = bin(int(outlier_mask[y, x])).count('1')

    # Compute strategy from class
    for y in range(height):
        for x in range(width):
            meta.strategy[y, x] = _infer_strategy(ml_class[y, x])

    return meta


# --------------------------------------------------------------------------
# Visualization functions
# --------------------------------------------------------------------------

def create_source_frame_map(meta: StackingMetadata,
                            output_path: Optional[str] = None,
                            title: str = "Source Frame Map") -> plt.Figure:
    """
    Create visualization showing which frame contributed each pixel.

    Uses a cyclic colormap to distinguish frame indices.
    """
    fig, ax = plt.subplots(figsize=(12, 10))

    # Create colormap with distinct colors for each frame
    num_colors = max(meta.num_frames, int(np.max(meta.source_frame)) + 1)
    colors = plt.cm.hsv(np.linspace(0, 1, num_colors))
    cmap = ListedColormap(colors)

    im = ax.imshow(meta.source_frame, cmap=cmap,
                   vmin=0, vmax=num_colors-1, interpolation='nearest')

    cbar = plt.colorbar(im, ax=ax, label='Frame Index')
    cbar.set_ticks(np.linspace(0, num_colors-1, min(10, num_colors)))
    cbar.set_ticklabels([str(int(t)) for t in cbar.get_ticks()])

    ax.set_title(title, fontsize=14)
    ax.set_xlabel('X (pixels)')
    ax.set_ylabel('Y (pixels)')

    # Add statistics
    unique_frames = np.unique(meta.source_frame)
    stats_text = f"Frames used: {len(unique_frames)}/{meta.num_frames}\n"
    stats_text += f"Most common: Frame {np.bincount(meta.source_frame.ravel()).argmax()}"
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes,
            verticalalignment='top', fontsize=10,
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

    plt.tight_layout()

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


def create_confidence_map(meta: StackingMetadata,
                          output_path: Optional[str] = None,
                          title: str = "Selection Confidence Map") -> plt.Figure:
    """
    Create visualization of selection confidence at each pixel.

    Uses a diverging colormap to highlight low confidence regions.
    """
    fig, ax = plt.subplots(figsize=(12, 10))

    # Custom colormap: red (low) -> yellow (medium) -> green (high)
    cmap = LinearSegmentedColormap.from_list(
        'confidence',
        [(0.8, 0.2, 0.2),    # Red - low confidence
         (1.0, 1.0, 0.4),    # Yellow - medium
         (0.2, 0.8, 0.2)]    # Green - high confidence
    )

    im = ax.imshow(meta.confidence, cmap=cmap, vmin=0, vmax=1, interpolation='nearest')

    cbar = plt.colorbar(im, ax=ax, label='Confidence')
    cbar.set_ticks([0, 0.25, 0.5, 0.75, 1.0])
    cbar.set_ticklabels(['0%', '25%', '50%', '75%', '100%'])

    ax.set_title(title, fontsize=14)
    ax.set_xlabel('X (pixels)')
    ax.set_ylabel('Y (pixels)')

    # Add statistics
    stats_text = f"Mean confidence: {np.mean(meta.confidence):.2%}\n"
    stats_text += f"Min: {np.min(meta.confidence):.2%}, Max: {np.max(meta.confidence):.2%}\n"
    low_conf = np.sum(meta.confidence < 0.5) / meta.confidence.size
    stats_text += f"Low confidence (<50%): {low_conf:.1%} of pixels"
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes,
            verticalalignment='top', fontsize=10,
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

    plt.tight_layout()

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


def create_strategy_map(meta: StackingMetadata,
                        output_path: Optional[str] = None,
                        title: str = "Selection Strategy Map") -> plt.Figure:
    """
    Create visualization showing what selection strategy was used at each pixel.
    """
    fig, ax = plt.subplots(figsize=(14, 10))

    # Create colormap from strategy colors
    cmap = ListedColormap(STRATEGY_COLORS)
    bounds = np.arange(-0.5, len(SELECTION_STRATEGIES) + 0.5, 1)
    norm = BoundaryNorm(bounds, cmap.N)

    im = ax.imshow(meta.strategy, cmap=cmap, norm=norm, interpolation='nearest')

    # Create legend instead of colorbar
    patches = [Patch(facecolor=STRATEGY_COLORS[i], label=SELECTION_STRATEGIES[i])
               for i in range(len(SELECTION_STRATEGIES))]
    ax.legend(handles=patches, loc='upper right',
              bbox_to_anchor=(1.25, 1.0), title='Strategy')

    ax.set_title(title, fontsize=14)
    ax.set_xlabel('X (pixels)')
    ax.set_ylabel('Y (pixels)')

    # Add statistics
    unique, counts = np.unique(meta.strategy, return_counts=True)
    stats_text = "Strategy distribution:\n"
    for s, c in sorted(zip(unique, counts), key=lambda x: -x[1])[:5]:
        pct = c / meta.strategy.size * 100
        stats_text += f"  {SELECTION_STRATEGIES[s]}: {pct:.1f}%\n"
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes,
            verticalalignment='top', fontsize=9,
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

    plt.tight_layout()

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


def create_outlier_map(meta: StackingMetadata,
                       output_path: Optional[str] = None,
                       title: str = "Outlier Rejection Map") -> plt.Figure:
    """
    Create visualization showing how many frames were rejected at each pixel.
    """
    fig, ax = plt.subplots(figsize=(12, 10))

    # Colormap: white (0) -> yellow (few) -> red (many)
    max_outliers = max(int(np.max(meta.outlier_count)), meta.num_frames // 2)
    cmap = LinearSegmentedColormap.from_list(
        'outliers',
        [(1.0, 1.0, 1.0),    # White - no outliers
         (1.0, 1.0, 0.4),    # Yellow - few
         (1.0, 0.6, 0.0),    # Orange - moderate
         (0.8, 0.0, 0.0)]    # Red - many
    )

    im = ax.imshow(meta.outlier_count, cmap=cmap, vmin=0, vmax=max_outliers,
                   interpolation='nearest')

    cbar = plt.colorbar(im, ax=ax, label='Frames Rejected')

    ax.set_title(title, fontsize=14)
    ax.set_xlabel('X (pixels)')
    ax.set_ylabel('Y (pixels)')

    # Add statistics
    total_rejections = np.sum(meta.outlier_count)
    pixels_with_outliers = np.sum(meta.outlier_count > 0)
    avg_rejections = total_rejections / meta.outlier_count.size

    stats_text = f"Total rejections: {total_rejections:,}\n"
    stats_text += f"Pixels with outliers: {pixels_with_outliers:,} ({pixels_with_outliers/meta.outlier_count.size:.1%})\n"
    stats_text += f"Avg rejections/pixel: {avg_rejections:.2f}\n"
    stats_text += f"Max rejections: {int(np.max(meta.outlier_count))}"
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes,
            verticalalignment='top', fontsize=10,
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

    plt.tight_layout()

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


def create_class_map(meta: StackingMetadata,
                     output_path: Optional[str] = None,
                     title: str = "ML Segmentation Class Map") -> plt.Figure:
    """
    Create visualization of ML segmentation classes.

    Uses the NukeX segmentation palette if available.
    """
    fig, ax = plt.subplots(figsize=(14, 10))

    if HAS_PALETTE:
        # Use the segmentation palette
        cmap = ListedColormap([tuple(c/255 for c in rgb) for rgb in CLASS_COLORS_RGB])
        bounds = np.arange(-0.5, SEG_NUM_CLASSES + 0.5, 1)
        norm = BoundaryNorm(bounds, cmap.N)

        im = ax.imshow(meta.ml_class, cmap=cmap, norm=norm, interpolation='nearest')

        # Create legend with actual classes present
        unique_classes = np.unique(meta.ml_class)
        patches = [Patch(facecolor=tuple(c/255 for c in CLASS_COLORS_RGB[i]),
                        label=CLASS_DISPLAY_NAMES[i])
                  for i in unique_classes if i < SEG_NUM_CLASSES]
        ax.legend(handles=patches, loc='upper right',
                 bbox_to_anchor=(1.3, 1.0), title='Class')
    else:
        # Fallback colormap
        im = ax.imshow(meta.ml_class, cmap='tab20', interpolation='nearest')
        plt.colorbar(im, ax=ax, label='Class ID')

    ax.set_title(title, fontsize=14)
    ax.set_xlabel('X (pixels)')
    ax.set_ylabel('Y (pixels)')

    # Add class statistics
    unique, counts = np.unique(meta.ml_class, return_counts=True)
    stats_text = "Class distribution:\n"
    for c, cnt in sorted(zip(unique, counts), key=lambda x: -x[1])[:6]:
        pct = cnt / meta.ml_class.size * 100
        if HAS_PALETTE and c < SEG_NUM_CLASSES:
            name = CLASS_DISPLAY_NAMES[c][:15]
        else:
            name = f"Class {c}"
        stats_text += f"  {name}: {pct:.1f}%\n"
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes,
            verticalalignment='top', fontsize=9,
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

    plt.tight_layout()

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


def create_summary_figure(meta: StackingMetadata,
                          result_image: Optional[np.ndarray] = None,
                          output_path: Optional[str] = None,
                          title: str = "Intelligent Stacking Diagnostics") -> plt.Figure:
    """
    Create multi-panel summary figure with all diagnostic visualizations.
    """
    # Figure with 2x3 grid
    fig = plt.figure(figsize=(20, 14))

    # Use GridSpec for flexible layout
    if result_image is not None:
        gs = gridspec.GridSpec(2, 3, figure=fig, hspace=0.3, wspace=0.25)
    else:
        gs = gridspec.GridSpec(2, 3, figure=fig, hspace=0.3, wspace=0.25)

    # Panel 1: Source Frame Map
    ax1 = fig.add_subplot(gs[0, 0])
    num_colors = max(meta.num_frames, int(np.max(meta.source_frame)) + 1)
    colors = plt.cm.hsv(np.linspace(0, 1, num_colors))
    cmap = ListedColormap(colors)
    im1 = ax1.imshow(meta.source_frame, cmap=cmap, vmin=0, vmax=num_colors-1)
    ax1.set_title('Source Frame', fontsize=12)
    ax1.axis('off')
    cbar1 = plt.colorbar(im1, ax=ax1, fraction=0.046, pad=0.04)
    cbar1.set_label('Frame #', fontsize=9)

    # Panel 2: Confidence Map
    ax2 = fig.add_subplot(gs[0, 1])
    conf_cmap = LinearSegmentedColormap.from_list('confidence',
        [(0.8, 0.2, 0.2), (1.0, 1.0, 0.4), (0.2, 0.8, 0.2)])
    im2 = ax2.imshow(meta.confidence, cmap=conf_cmap, vmin=0, vmax=1)
    ax2.set_title('Selection Confidence', fontsize=12)
    ax2.axis('off')
    cbar2 = plt.colorbar(im2, ax=ax2, fraction=0.046, pad=0.04)
    cbar2.set_label('Confidence', fontsize=9)

    # Panel 3: Outlier Rejection Map
    ax3 = fig.add_subplot(gs[0, 2])
    max_outliers = max(int(np.max(meta.outlier_count)), 1)
    outlier_cmap = LinearSegmentedColormap.from_list('outliers',
        [(1.0, 1.0, 1.0), (1.0, 1.0, 0.4), (1.0, 0.6, 0.0), (0.8, 0.0, 0.0)])
    im3 = ax3.imshow(meta.outlier_count, cmap=outlier_cmap, vmin=0, vmax=max_outliers)
    ax3.set_title('Frames Rejected', fontsize=12)
    ax3.axis('off')
    cbar3 = plt.colorbar(im3, ax=ax3, fraction=0.046, pad=0.04)
    cbar3.set_label('Count', fontsize=9)

    # Panel 4: ML Class Map
    ax4 = fig.add_subplot(gs[1, 0])
    if HAS_PALETTE:
        class_cmap = ListedColormap([tuple(c/255 for c in rgb) for rgb in CLASS_COLORS_RGB])
        bounds = np.arange(-0.5, SEG_NUM_CLASSES + 0.5, 1)
        norm = BoundaryNorm(bounds, class_cmap.N)
        im4 = ax4.imshow(meta.ml_class, cmap=class_cmap, norm=norm)
    else:
        im4 = ax4.imshow(meta.ml_class, cmap='tab20')
    ax4.set_title('ML Segmentation Class', fontsize=12)
    ax4.axis('off')

    # Panel 5: Strategy Map
    ax5 = fig.add_subplot(gs[1, 1])
    strat_cmap = ListedColormap(STRATEGY_COLORS)
    bounds = np.arange(-0.5, len(SELECTION_STRATEGIES) + 0.5, 1)
    norm = BoundaryNorm(bounds, strat_cmap.N)
    im5 = ax5.imshow(meta.strategy, cmap=strat_cmap, norm=norm)
    ax5.set_title('Selection Strategy', fontsize=12)
    ax5.axis('off')

    # Panel 6: Result image or statistics
    ax6 = fig.add_subplot(gs[1, 2])
    if result_image is not None:
        ax6.imshow(result_image)
        ax6.set_title('Integrated Result', fontsize=12)
        ax6.axis('off')
    else:
        # Show statistics panel
        ax6.axis('off')

        stats_text = "STACKING STATISTICS\n"
        stats_text += "=" * 30 + "\n\n"

        stats_text += f"Image size: {meta.width} x {meta.height}\n"
        stats_text += f"Frames stacked: {meta.num_frames}\n\n"

        stats_text += "SOURCE FRAME:\n"
        unique_frames = np.unique(meta.source_frame)
        stats_text += f"  Frames used: {len(unique_frames)}/{meta.num_frames}\n"
        mode_frame = np.bincount(meta.source_frame.ravel()).argmax()
        stats_text += f"  Most common: Frame {mode_frame}\n\n"

        stats_text += "CONFIDENCE:\n"
        stats_text += f"  Mean: {np.mean(meta.confidence):.2%}\n"
        stats_text += f"  Min/Max: {np.min(meta.confidence):.2%}/{np.max(meta.confidence):.2%}\n"
        low_conf = np.sum(meta.confidence < 0.5) / meta.confidence.size
        stats_text += f"  Low (<50%): {low_conf:.1%}\n\n"

        stats_text += "OUTLIER REJECTION:\n"
        total_rej = np.sum(meta.outlier_count)
        pix_with_outliers = np.sum(meta.outlier_count > 0)
        stats_text += f"  Total rejections: {total_rej:,}\n"
        stats_text += f"  Pixels affected: {pix_with_outliers:,}\n"
        stats_text += f"  Max per pixel: {int(np.max(meta.outlier_count))}\n\n"

        stats_text += "STRATEGIES USED:\n"
        unique, counts = np.unique(meta.strategy, return_counts=True)
        for s, c in sorted(zip(unique, counts), key=lambda x: -x[1])[:4]:
            pct = c / meta.strategy.size * 100
            stats_text += f"  {SELECTION_STRATEGIES[s]}: {pct:.1f}%\n"

        ax6.text(0.1, 0.95, stats_text, transform=ax6.transAxes,
                fontsize=10, verticalalignment='top',
                fontfamily='monospace',
                bbox=dict(boxstyle='round', facecolor='#f0f0f0', alpha=0.9))

    # Add main title
    fig.suptitle(title, fontsize=16, fontweight='bold', y=0.98)

    plt.tight_layout(rect=[0, 0.02, 1, 0.96])

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


def create_histogram_panel(meta: StackingMetadata,
                           output_path: Optional[str] = None,
                           title: str = "Stacking Metadata Histograms") -> plt.Figure:
    """
    Create histograms of stacking metadata distributions.
    """
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Source frame histogram
    ax1 = axes[0, 0]
    ax1.hist(meta.source_frame.ravel(), bins=meta.num_frames,
             range=(0, meta.num_frames), color='steelblue', edgecolor='black', alpha=0.7)
    ax1.set_xlabel('Frame Index')
    ax1.set_ylabel('Pixel Count')
    ax1.set_title('Source Frame Distribution')
    ax1.axvline(np.median(meta.source_frame), color='red', linestyle='--',
                label=f'Median: {int(np.median(meta.source_frame))}')
    ax1.legend()

    # Confidence histogram
    ax2 = axes[0, 1]
    ax2.hist(meta.confidence.ravel(), bins=50, range=(0, 1),
             color='green', edgecolor='black', alpha=0.7)
    ax2.set_xlabel('Confidence')
    ax2.set_ylabel('Pixel Count')
    ax2.set_title('Selection Confidence Distribution')
    ax2.axvline(np.mean(meta.confidence), color='red', linestyle='--',
                label=f'Mean: {np.mean(meta.confidence):.2f}')
    ax2.legend()

    # Outlier count histogram
    ax3 = axes[1, 0]
    max_outliers = int(np.max(meta.outlier_count)) + 1
    ax3.hist(meta.outlier_count.ravel(), bins=max(max_outliers, 10),
             range=(0, max_outliers), color='orange', edgecolor='black', alpha=0.7)
    ax3.set_xlabel('Frames Rejected')
    ax3.set_ylabel('Pixel Count')
    ax3.set_title('Outlier Rejection Distribution')
    ax3.set_yscale('log')  # Log scale since most pixels have 0 outliers

    # Strategy pie chart
    ax4 = axes[1, 1]
    unique, counts = np.unique(meta.strategy, return_counts=True)
    labels = [SELECTION_STRATEGIES[s] for s in unique]
    colors = [STRATEGY_COLORS[s] for s in unique]

    # Only show significant strategies (>1%)
    total = sum(counts)
    significant = [(l, c, col) for l, c, col in zip(labels, counts, colors) if c/total > 0.01]
    if significant:
        labels_sig, counts_sig, colors_sig = zip(*significant)
        other_count = total - sum(counts_sig)
        if other_count > 0:
            labels_sig = list(labels_sig) + ['Other']
            counts_sig = list(counts_sig) + [other_count]
            colors_sig = list(colors_sig) + [(0.7, 0.7, 0.7)]

        ax4.pie(counts_sig, labels=labels_sig, colors=colors_sig,
                autopct='%1.1f%%', startangle=90)
    ax4.set_title('Selection Strategy Distribution')

    fig.suptitle(title, fontsize=14, fontweight='bold')
    plt.tight_layout(rect=[0, 0, 1, 0.96])

    if output_path:
        fig.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"Saved: {output_path}")

    return fig


# --------------------------------------------------------------------------
# FITS loading utilities
# --------------------------------------------------------------------------

def load_fits_image(filepath: str) -> Optional[np.ndarray]:
    """
    Load FITS image file.

    Returns:
        numpy array (H, W) or (H, W, C) for RGB
    """
    try:
        from astropy.io import fits
        with fits.open(filepath) as hdul:
            data = hdul[0].data
            if data is None and len(hdul) > 1:
                data = hdul[1].data

            if data is None:
                print(f"Warning: No image data found in {filepath}")
                return None

            # Handle different axis orders
            if data.ndim == 3:
                # Could be (C, H, W) or (H, W, C)
                if data.shape[0] in [1, 3, 4]:  # Likely (C, H, W)
                    data = np.moveaxis(data, 0, -1)

            return data.astype(np.float32)
    except ImportError:
        print("Warning: astropy not available for FITS loading")
        return None
    except Exception as e:
        print(f"Error loading FITS: {e}")
        return None


# --------------------------------------------------------------------------
# Main entry point
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Generate diagnostic visualizations for NukeX intelligent stacking',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate visualizations from real stacking output
  python visualize_stacking.py --result integrated.fits --metadata metadata.bin --output diagnostics/

  # Generate synthetic test data
  python visualize_stacking.py --synthetic --output diagnostics/

  # Customize synthetic dimensions
  python visualize_stacking.py --synthetic --width 2048 --height 2048 --num-frames 30 --output diagnostics/
"""
    )

    parser.add_argument('--result', help='Path to integrated result FITS file')
    parser.add_argument('--metadata', help='Path to stacking metadata binary file')
    parser.add_argument('--output', default='stacking_diagnostics',
                        help='Output directory for visualizations')
    parser.add_argument('--synthetic', action='store_true',
                        help='Generate synthetic test data')
    parser.add_argument('--width', type=int, default=512,
                        help='Width for synthetic data (default: 512)')
    parser.add_argument('--height', type=int, default=512,
                        help='Height for synthetic data (default: 512)')
    parser.add_argument('--num-frames', type=int, default=20,
                        help='Number of frames for synthetic data (default: 20)')
    parser.add_argument('--seed', type=int, default=42,
                        help='Random seed for synthetic data (default: 42)')
    parser.add_argument('--individual', action='store_true',
                        help='Save individual map images in addition to summary')
    parser.add_argument('--histogram', action='store_true',
                        help='Generate histogram panel')

    args = parser.parse_args()

    # Create output directory
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Load or generate metadata
    if args.synthetic:
        print(f"Generating synthetic stacking metadata ({args.width}x{args.height}, {args.num_frames} frames)...")
        meta = generate_synthetic_metadata(args.width, args.height, args.num_frames, args.seed)
        result_image = None

    elif args.metadata:
        print(f"Loading metadata from: {args.metadata}")
        try:
            meta = StackingMetadata.from_binary(args.metadata)
            print(f"  Image size: {meta.width} x {meta.height}")
            print(f"  Frames: {meta.num_frames}")
        except Exception as e:
            print(f"Error loading metadata: {e}")
            print("Try using --synthetic to generate test data")
            return 1

        # Load result image if available
        result_image = None
        if args.result:
            print(f"Loading result image: {args.result}")
            result_image = load_fits_image(args.result)
            if result_image is not None:
                # Normalize for display
                result_image = np.clip(result_image, 0, 1)
                if result_image.ndim == 2:
                    result_image = np.stack([result_image]*3, axis=-1)
    else:
        print("Error: Must specify --metadata <file> or --synthetic")
        parser.print_help()
        return 1

    print(f"\nGenerating visualizations...")
    print(f"Output directory: {output_dir}")

    # Generate individual maps if requested
    if args.individual:
        print("\nCreating individual maps...")
        create_source_frame_map(meta, output_dir / 'source_frame_map.png')
        create_confidence_map(meta, output_dir / 'confidence_map.png')
        create_strategy_map(meta, output_dir / 'strategy_map.png')
        create_outlier_map(meta, output_dir / 'outlier_map.png')
        create_class_map(meta, output_dir / 'class_map.png')

    # Generate histograms if requested
    if args.histogram:
        print("\nCreating histogram panel...")
        create_histogram_panel(meta, output_dir / 'histograms.png')

    # Always generate summary figure
    print("\nCreating summary figure...")
    create_summary_figure(
        meta,
        result_image=result_image if args.result else None,
        output_path=output_dir / 'summary.png',
        title="NukeX Intelligent Stacking Diagnostics"
    )

    # Print summary statistics
    print("\n" + "="*60)
    print("STACKING DIAGNOSTICS SUMMARY")
    print("="*60)
    print(f"Image dimensions: {meta.width} x {meta.height}")
    print(f"Frames in stack: {meta.num_frames}")
    print()
    print("Source Frame Statistics:")
    unique_frames = np.unique(meta.source_frame)
    print(f"  Unique frames used: {len(unique_frames)}/{meta.num_frames}")
    print(f"  Most common frame: {np.bincount(meta.source_frame.ravel()).argmax()}")
    print()
    print("Confidence Statistics:")
    print(f"  Mean: {np.mean(meta.confidence):.2%}")
    print(f"  Std Dev: {np.std(meta.confidence):.2%}")
    print(f"  Min/Max: {np.min(meta.confidence):.2%} / {np.max(meta.confidence):.2%}")
    low_conf = np.sum(meta.confidence < 0.5) / meta.confidence.size
    print(f"  Low confidence (<50%): {low_conf:.1%}")
    print()
    print("Outlier Rejection Statistics:")
    total_rej = np.sum(meta.outlier_count)
    pix_with_outliers = np.sum(meta.outlier_count > 0)
    print(f"  Total rejections: {total_rej:,}")
    print(f"  Pixels with outliers: {pix_with_outliers:,} ({pix_with_outliers/meta.outlier_count.size:.1%})")
    print(f"  Max rejections per pixel: {int(np.max(meta.outlier_count))}")
    print()
    print("Selection Strategies:")
    unique, counts = np.unique(meta.strategy, return_counts=True)
    for s, c in sorted(zip(unique, counts), key=lambda x: -x[1]):
        pct = c / meta.strategy.size * 100
        print(f"  {SELECTION_STRATEGIES[s]}: {pct:.1f}%")
    print()
    print(f"Output saved to: {output_dir}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
