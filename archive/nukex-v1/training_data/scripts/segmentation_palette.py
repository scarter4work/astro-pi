#!/usr/bin/env python3
"""
Unified Segmentation Palette for NukeX 21-Class Model

This module provides a consistent color palette used across all NukeX
visualization tools (Python test scripts and C++ PixInsight module).

The palette is designed to be:
- Perceptually distinct: All 21 classes are visually separable
- Semantically meaningful: Colors match astronomical expectations
  (red for emission nebulae, blue for reflection, etc.)
- Colorblind-friendly: Major categories use different hues/luminances

Color Design Rationale:
-----------------------
BACKGROUND (0): Dark blue-gray - neutral, non-intrusive
STARS (1-4): Yellow to white spectrum - bright, attention-grabbing
  - Bright: Pure yellow (most visible)
  - Medium: Gold/amber
  - Faint: Pale gold
  - Saturated: Pure white (indicates clipping)
NEBULAE (5-8): Spectral colors matching real appearance
  - Emission: Red/magenta (H-alpha)
  - Reflection: Blue (scattered starlight)
  - Dark: Dark brown (obscuring dust)
  - Planetary: Cyan/teal (OIII)
GALAXIES (9-12): Warm orange/purple tones
  - Spiral: Orange (arms visible)
  - Elliptical: Salmon/coral
  - Irregular: Violet
  - Core: Bright yellow-orange (AGN/nucleus)
DUST LANES (13): Dark brown (obscuring material)
STAR CLUSTERS (14-15): Distinct from individual stars
  - Open: Light sky blue
  - Globular: Warm peach/apricot
ARTIFACTS (16-20): Warning colors, distinct from astronomical objects
  - Hot pixel: Bright red (danger/warning)
  - Satellite: Bright green (artificial)
  - Diffraction: Magenta (optical artifact)
  - Gradient: Olive/khaki (systematic)
  - Noise: Gray (random)

Copyright (c) 2026 Scott Carter
"""

from typing import Tuple, List, Optional
import numpy as np

# Number of classes in the segmentation model
NUM_CLASSES = 21

# Class names in order (index = class ID)
CLASS_NAMES = [
    'background',           # 0
    'star_bright',          # 1
    'star_medium',          # 2
    'star_faint',           # 3
    'star_saturated',       # 4
    'nebula_emission',      # 5
    'nebula_reflection',    # 6
    'nebula_dark',          # 7
    'nebula_planetary',     # 8
    'galaxy_spiral',        # 9
    'galaxy_elliptical',    # 10
    'galaxy_irregular',     # 11
    'galaxy_core',          # 12
    'dust_lane',            # 13
    'star_cluster_open',    # 14
    'star_cluster_globular',# 15
    'artifact_hot_pixel',   # 16
    'artifact_satellite',   # 17
    'artifact_diffraction', # 18
    'artifact_gradient',    # 19
    'artifact_noise',       # 20
]

# Human-readable display names
CLASS_DISPLAY_NAMES = [
    'Background',
    'Bright Star',
    'Medium Star',
    'Faint Star',
    'Saturated Star',
    'Emission Nebula',
    'Reflection Nebula',
    'Dark Nebula',
    'Planetary Nebula',
    'Spiral Galaxy',
    'Elliptical Galaxy',
    'Irregular Galaxy',
    'Galaxy Core',
    'Dust Lane',
    'Open Cluster',
    'Globular Cluster',
    'Hot Pixel',
    'Satellite Trail',
    'Diffraction Spike',
    'Gradient',
    'Noise',
]

# RGB colors as 0-255 tuples (for image output)
# Carefully chosen to be perceptually distinct
CLASS_COLORS_RGB: List[Tuple[int, int, int]] = [
    # Background - dark blue-gray
    (26, 26, 51),           # 0: background

    # Stars - yellow/gold/white spectrum
    (255, 255, 0),          # 1: star_bright - pure yellow
    (255, 204, 51),         # 2: star_medium - golden amber
    (230, 191, 102),        # 3: star_faint - pale gold
    (255, 255, 255),        # 4: star_saturated - pure white

    # Nebulae - spectral colors matching real appearance
    (255, 51, 102),         # 5: nebula_emission - red/magenta (H-alpha)
    (102, 153, 255),        # 6: nebula_reflection - blue
    (51, 26, 38),           # 7: nebula_dark - dark brown/maroon
    (0, 204, 204),          # 8: nebula_planetary - cyan/teal (OIII)

    # Galaxies - orange/purple spectrum
    (255, 153, 51),         # 9: galaxy_spiral - orange
    (230, 128, 128),        # 10: galaxy_elliptical - salmon/coral
    (153, 102, 204),        # 11: galaxy_irregular - violet
    (255, 204, 0),          # 12: galaxy_core - bright amber/yellow

    # Structural features
    (102, 51, 26),          # 13: dust_lane - dark brown
    (153, 204, 255),        # 14: star_cluster_open - light sky blue
    (255, 179, 128),        # 15: star_cluster_globular - peach/apricot

    # Artifacts - warning colors
    (255, 0, 51),           # 16: artifact_hot_pixel - bright red
    (51, 255, 51),          # 17: artifact_satellite - bright green
    (255, 51, 255),         # 18: artifact_diffraction - magenta
    (153, 153, 51),         # 19: artifact_gradient - olive/khaki
    (102, 102, 102),        # 20: artifact_noise - medium gray
]

# RGB colors normalized to 0.0-1.0 (for matplotlib/torch)
CLASS_COLORS_NORM: List[Tuple[float, float, float]] = [
    tuple(c / 255.0 for c in rgb) for rgb in CLASS_COLORS_RGB
]

# Category groupings for analysis
CATEGORY_STARS = [1, 2, 3, 4]
CATEGORY_NEBULAE = [5, 6, 7, 8]
CATEGORY_GALAXIES = [9, 10, 11, 12]
CATEGORY_CLUSTERS = [14, 15]
CATEGORY_ARTIFACTS = [16, 17, 18, 19, 20]
CATEGORY_DUST = [7, 13]  # Dark nebula and dust lanes


def get_color_for_class(class_id: int, normalized: bool = False) -> Tuple:
    """
    Get the color for a given class ID.

    Args:
        class_id: Integer class ID (0-20)
        normalized: If True, return 0.0-1.0 values; if False, return 0-255

    Returns:
        Tuple of (R, G, B) values
    """
    if class_id < 0 or class_id >= NUM_CLASSES:
        # Return magenta for unknown classes (obvious error indicator)
        return (1.0, 0.0, 1.0) if normalized else (255, 0, 255)

    if normalized:
        return CLASS_COLORS_NORM[class_id]
    return CLASS_COLORS_RGB[class_id]


def get_class_name(class_id: int, display: bool = False) -> str:
    """
    Get the name for a given class ID.

    Args:
        class_id: Integer class ID (0-20)
        display: If True, return human-readable display name

    Returns:
        Class name string
    """
    if class_id < 0 or class_id >= NUM_CLASSES:
        return 'unknown'

    if display:
        return CLASS_DISPLAY_NAMES[class_id]
    return CLASS_NAMES[class_id]


def get_class_id(class_name: str) -> int:
    """
    Get the class ID for a given class name.

    Args:
        class_name: Class name string (case-insensitive)

    Returns:
        Integer class ID, or -1 if not found
    """
    name_lower = class_name.lower().replace(' ', '_')
    for i, name in enumerate(CLASS_NAMES):
        if name.lower() == name_lower:
            return i
    return -1


def create_colormap_array() -> np.ndarray:
    """
    Create a numpy array suitable for indexed color mapping.

    Returns:
        Array of shape (NUM_CLASSES, 3) with uint8 RGB values
    """
    return np.array(CLASS_COLORS_RGB, dtype=np.uint8)


def apply_colormap(mask: np.ndarray) -> np.ndarray:
    """
    Apply the segmentation colormap to a class mask.

    Args:
        mask: 2D numpy array of integer class IDs (H, W)

    Returns:
        3D numpy array of RGB values (H, W, 3), dtype uint8
    """
    h, w = mask.shape
    colored = np.zeros((h, w, 3), dtype=np.uint8)

    for class_id, color in enumerate(CLASS_COLORS_RGB):
        colored[mask == class_id] = color

    return colored


def create_legend_image(
    cell_width: int = 150,
    cell_height: int = 30,
    font_size: int = 12,
    include_ids: bool = True
) -> np.ndarray:
    """
    Create a legend image showing all classes with their colors and names.

    Args:
        cell_width: Width of each legend cell in pixels
        cell_height: Height of each legend cell in pixels
        font_size: Font size for labels (requires PIL)
        include_ids: Whether to include class IDs in labels

    Returns:
        RGB numpy array of the legend image
    """
    try:
        from PIL import Image, ImageDraw, ImageFont
        use_pil = True
    except ImportError:
        use_pil = False

    # Calculate dimensions
    color_box_width = 40
    margin = 5

    # Two columns layout
    num_rows = (NUM_CLASSES + 1) // 2
    img_width = cell_width * 2 + margin * 2
    img_height = cell_height * num_rows + margin * 2

    # Create image
    legend = np.ones((img_height, img_width, 3), dtype=np.uint8) * 240  # Light gray background

    for i in range(NUM_CLASSES):
        col = i // num_rows
        row = i % num_rows

        x = margin + col * cell_width
        y = margin + row * cell_height

        # Draw color box
        color = CLASS_COLORS_RGB[i]
        legend[y+2:y+cell_height-2, x+2:x+color_box_width-2] = color

        # Draw border around color box
        legend[y+1, x+1:x+color_box_width] = (0, 0, 0)
        legend[y+cell_height-2, x+1:x+color_box_width] = (0, 0, 0)
        legend[y+1:y+cell_height-1, x+1] = (0, 0, 0)
        legend[y+1:y+cell_height-1, x+color_box_width-1] = (0, 0, 0)

    if use_pil:
        # Convert to PIL for text rendering
        img = Image.fromarray(legend)
        draw = ImageDraw.Draw(img)

        try:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", font_size)
        except:
            font = ImageFont.load_default()

        for i in range(NUM_CLASSES):
            col = i // num_rows
            row = i % num_rows

            x = margin + col * cell_width + color_box_width + 5
            y = margin + row * cell_height + (cell_height - font_size) // 2

            if include_ids:
                label = f"{i}: {CLASS_DISPLAY_NAMES[i]}"
            else:
                label = CLASS_DISPLAY_NAMES[i]

            draw.text((x, y), label, fill=(0, 0, 0), font=font)

        legend = np.array(img)

    return legend


def get_category_mask(mask: np.ndarray, category: str) -> np.ndarray:
    """
    Create a binary mask for a category of classes.

    Args:
        mask: 2D numpy array of integer class IDs
        category: One of 'stars', 'nebulae', 'galaxies', 'clusters', 'artifacts', 'dust'

    Returns:
        Boolean numpy array where True = pixel belongs to category
    """
    category_map = {
        'stars': CATEGORY_STARS,
        'nebulae': CATEGORY_NEBULAE,
        'galaxies': CATEGORY_GALAXIES,
        'clusters': CATEGORY_CLUSTERS,
        'artifacts': CATEGORY_ARTIFACTS,
        'dust': CATEGORY_DUST,
    }

    if category.lower() not in category_map:
        raise ValueError(f"Unknown category: {category}. Valid: {list(category_map.keys())}")

    class_ids = category_map[category.lower()]
    result = np.zeros(mask.shape, dtype=bool)

    for class_id in class_ids:
        result |= (mask == class_id)

    return result


def print_palette_info():
    """Print the complete palette information for reference."""
    print("=" * 70)
    print("NukeX 21-Class Segmentation Palette")
    print("=" * 70)
    print(f"{'ID':<4} {'Name':<25} {'RGB (0-255)':<18} {'Hex':<10}")
    print("-" * 70)

    for i in range(NUM_CLASSES):
        r, g, b = CLASS_COLORS_RGB[i]
        hex_color = f"#{r:02X}{g:02X}{b:02X}"
        print(f"{i:<4} {CLASS_DISPLAY_NAMES[i]:<25} ({r:3}, {g:3}, {b:3})    {hex_color}")

    print("=" * 70)


# For matplotlib compatibility
def get_matplotlib_cmap():
    """
    Get a matplotlib ListedColormap for the segmentation palette.

    Returns:
        matplotlib.colors.ListedColormap
    """
    try:
        from matplotlib.colors import ListedColormap
        return ListedColormap(CLASS_COLORS_NORM, name='nukex_segmentation')
    except ImportError:
        raise ImportError("matplotlib is required for get_matplotlib_cmap()")


if __name__ == '__main__':
    # Print palette info when run as script
    print_palette_info()

    # Optionally generate and save legend
    try:
        from PIL import Image
        legend = create_legend_image()
        img = Image.fromarray(legend)
        img.save('/tmp/nukex_segmentation_legend.png')
        print(f"\nLegend saved to: /tmp/nukex_segmentation_legend.png")
    except ImportError:
        print("\nPIL not available, skipping legend generation")
