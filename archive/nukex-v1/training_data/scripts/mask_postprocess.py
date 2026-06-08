#!/usr/bin/env python3
"""
Mask Post-Processing Functions for NukeX Segmentation

Raw ML segmentation masks often contain noise and artifacts that need cleanup.
This module provides morphological and statistical cleaning operations.

Functions:
- morphological_cleanup: Close holes and remove noise using morphological ops
- remove_small_regions: Filter out isolated regions smaller than a threshold
- smooth_boundaries: Soften class boundaries using Gaussian blur on one-hot encoding
- postprocess_mask: Apply all post-processing steps in sequence

Usage:
    from mask_postprocess import postprocess_mask

    cleaned_mask = postprocess_mask(
        raw_mask,
        morph=True,
        min_region=100,
        smooth=False,
        sigma=1.0
    )

Copyright (c) 2026 Scott Carter
"""

import numpy as np

try:
    from scipy import ndimage
    HAS_SCIPY = True
except ImportError:
    HAS_SCIPY = False

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False


def morphological_cleanup(mask: np.ndarray, kernel_size: int = 3) -> np.ndarray:
    """
    Close small holes and remove noise using morphological operations.

    Applies morphological closing (dilation followed by erosion) to fill small
    holes within regions, then opening (erosion followed by dilation) to remove
    small isolated noise pixels.

    Args:
        mask: Input segmentation mask (H, W) with integer class labels
        kernel_size: Size of the morphological kernel (default: 3)
                    Larger values = more aggressive cleaning

    Returns:
        Cleaned mask with same shape and dtype as input
    """
    if not HAS_CV2:
        print("Warning: OpenCV not available, skipping morphological cleanup")
        return mask

    # Create the structuring element (kernel)
    kernel = np.ones((kernel_size, kernel_size), np.uint8)

    # Get unique classes
    unique_classes = np.unique(mask)

    # Process each class separately to preserve class boundaries
    cleaned = np.zeros_like(mask)

    for class_id in unique_classes:
        # Create binary mask for this class
        class_mask = (mask == class_id).astype(np.uint8)

        # Close: fill small holes within the region
        closed = cv2.morphologyEx(class_mask, cv2.MORPH_CLOSE, kernel)

        # Open: remove small isolated noise
        opened = cv2.morphologyEx(closed, cv2.MORPH_OPEN, kernel)

        # Add to result where this class "won"
        cleaned[opened == 1] = class_id

    # Handle conflicts where multiple classes want the same pixel:
    # The last class in the loop wins. For more sophisticated handling,
    # use a priority-based approach.

    return cleaned


def remove_small_regions(mask: np.ndarray, min_size: int = 100) -> np.ndarray:
    """
    Remove isolated regions smaller than min_size pixels.

    For each class, finds connected components and removes those with
    fewer than min_size pixels, replacing them with background (class 0).

    Args:
        mask: Input segmentation mask (H, W) with integer class labels
        min_size: Minimum region size in pixels (default: 100)
                 Regions smaller than this are removed

    Returns:
        Cleaned mask with small regions replaced by background
    """
    if not HAS_CV2:
        print("Warning: OpenCV not available, skipping small region removal")
        return mask

    cleaned = mask.copy()

    # Process each class except background
    for class_id in np.unique(mask):
        if class_id == 0:  # Skip background
            continue

        # Create binary mask for this class
        class_mask = (mask == class_id).astype(np.uint8)

        # Find connected components
        n_labels, labels, stats, _ = cv2.connectedComponentsWithStats(
            class_mask, connectivity=8
        )

        # Remove small components
        for i in range(1, n_labels):  # Skip label 0 (background in component analysis)
            area = stats[i, cv2.CC_STAT_AREA]
            if area < min_size:
                # Set small region to background
                cleaned[labels == i] = 0

    return cleaned


def smooth_boundaries(mask: np.ndarray, sigma: float = 1.0,
                      preserve_edges: bool = True) -> np.ndarray:
    """
    Smooth class boundaries using Gaussian blur on one-hot encoding.

    Converts the mask to one-hot encoding (one channel per class),
    applies Gaussian blur to each channel, then takes argmax to get
    the final class assignment. This creates smoother transitions
    between classes while maintaining hard class labels.

    Args:
        mask: Input segmentation mask (H, W) with integer class labels
        sigma: Gaussian blur sigma (default: 1.0)
               Larger values = smoother boundaries
        preserve_edges: If True, preserve strong edges between very
                       different classes (default: True)

    Returns:
        Mask with smoothed boundaries
    """
    if not HAS_SCIPY:
        print("Warning: SciPy not available, skipping boundary smoothing")
        return mask

    unique_classes = np.unique(mask)
    n_classes = int(unique_classes.max()) + 1
    H, W = mask.shape

    # Create one-hot encoding (soft masks)
    one_hot = np.zeros((n_classes, H, W), dtype=np.float32)
    for class_id in unique_classes:
        one_hot[int(class_id)] = (mask == class_id).astype(np.float32)

    # Apply Gaussian blur to each class channel
    blurred = np.zeros_like(one_hot)
    for i in range(n_classes):
        if np.any(one_hot[i]):
            blurred[i] = ndimage.gaussian_filter(one_hot[i], sigma=sigma)

    # Take argmax to get final class assignment
    smoothed = np.argmax(blurred, axis=0).astype(mask.dtype)

    if preserve_edges:
        # Preserve original mask where confidence is high
        # (max probability is significantly higher than 2nd highest)
        sorted_probs = np.sort(blurred, axis=0)
        max_prob = sorted_probs[-1]  # Highest probability
        second_prob = sorted_probs[-2]  # Second highest

        # Where confidence is high (large gap between 1st and 2nd),
        # keep the argmax result. Where confidence is low, revert to original.
        confidence = max_prob - second_prob
        high_confidence = confidence > 0.3  # Threshold for "confident" prediction

        # Revert low-confidence regions to original
        smoothed = np.where(high_confidence, smoothed, mask)

    return smoothed


def fill_holes_in_class(mask: np.ndarray, class_id: int,
                        max_hole_size: int = 500) -> np.ndarray:
    """
    Fill holes within a specific class region.

    A "hole" is defined as a region of background (or other class) that is
    completely surrounded by the target class. This is useful for filling
    in gaps within nebulae or galaxies.

    Args:
        mask: Input segmentation mask (H, W)
        class_id: The class to fill holes in
        max_hole_size: Maximum hole size to fill (default: 500)
                      Larger holes are preserved

    Returns:
        Mask with holes filled in the specified class
    """
    if not HAS_CV2:
        print("Warning: OpenCV not available, skipping hole filling")
        return mask

    cleaned = mask.copy()

    # Create binary mask for this class
    class_mask = (mask == class_id).astype(np.uint8)

    # Find contours
    contours, hierarchy = cv2.findContours(
        class_mask, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE
    )

    if hierarchy is None or len(contours) == 0:
        return mask

    # hierarchy[0][i] = [next, prev, child, parent]
    # A contour with a parent is a hole
    for i, (contour, h) in enumerate(zip(contours, hierarchy[0])):
        parent = h[3]
        if parent >= 0:  # This is a hole (has a parent)
            area = cv2.contourArea(contour)
            if area < max_hole_size:
                # Fill this hole with the class
                cv2.drawContours(cleaned, [contour], -1, int(class_id), -1)

    return cleaned


def postprocess_mask(mask: np.ndarray,
                     morph: bool = True,
                     morph_kernel_size: int = 3,
                     min_region: int = 100,
                     smooth: bool = False,
                     sigma: float = 1.0,
                     fill_holes: bool = False,
                     hole_classes: list = None,
                     max_hole_size: int = 500,
                     verbose: bool = False) -> np.ndarray:
    """
    Apply all post-processing steps to a segmentation mask.

    This is the main entry point for mask post-processing. Steps are applied
    in order:
    1. Morphological cleanup (close holes, remove noise)
    2. Remove small isolated regions
    3. Smooth boundaries (optional)
    4. Fill holes in specific classes (optional)

    Args:
        mask: Input segmentation mask (H, W) with integer class labels
        morph: Apply morphological cleanup (default: True)
        morph_kernel_size: Kernel size for morphological ops (default: 3)
        min_region: Minimum region size in pixels (default: 100)
                   Set to 0 to disable small region removal
        smooth: Smooth class boundaries (default: False)
        sigma: Gaussian sigma for boundary smoothing (default: 1.0)
        fill_holes: Fill holes in specified classes (default: False)
        hole_classes: List of class IDs to fill holes in (default: None)
                     If None and fill_holes is True, fills holes in all
                     non-background classes
        max_hole_size: Maximum hole size to fill (default: 500)
        verbose: Print progress information (default: False)

    Returns:
        Post-processed mask

    Example:
        >>> raw_mask = load_mask('segmentation.png')
        >>> cleaned = postprocess_mask(
        ...     raw_mask,
        ...     morph=True,
        ...     min_region=100,
        ...     smooth=True,
        ...     sigma=1.0
        ... )
    """
    result = mask.copy().astype(np.int32)

    if verbose:
        unique_before = np.unique(result)
        total_pixels = result.size
        print(f"Post-processing mask: {result.shape}")
        print(f"  Classes before: {len(unique_before)} ({unique_before})")

    # Step 1: Morphological cleanup
    if morph:
        if verbose:
            print(f"  Applying morphological cleanup (kernel={morph_kernel_size})...")
        result = morphological_cleanup(result, kernel_size=morph_kernel_size)

    # Step 2: Remove small regions
    if min_region > 0:
        if verbose:
            print(f"  Removing regions smaller than {min_region} pixels...")
        result = remove_small_regions(result, min_size=min_region)

    # Step 3: Smooth boundaries
    if smooth:
        if verbose:
            print(f"  Smoothing boundaries (sigma={sigma})...")
        result = smooth_boundaries(result, sigma=sigma)

    # Step 4: Fill holes in specific classes
    if fill_holes:
        if hole_classes is None:
            # Default: fill holes in all non-background classes
            hole_classes = [c for c in np.unique(result) if c != 0]

        if verbose:
            print(f"  Filling holes in classes {hole_classes} (max_size={max_hole_size})...")

        for class_id in hole_classes:
            result = fill_holes_in_class(result, class_id, max_hole_size)

    if verbose:
        unique_after = np.unique(result)
        print(f"  Classes after: {len(unique_after)} ({unique_after})")

        # Report changes per class
        for class_id in np.union1d(unique_before, unique_after):
            before_count = np.sum(mask == class_id)
            after_count = np.sum(result == class_id)
            diff = after_count - before_count
            diff_pct = (diff / before_count * 100) if before_count > 0 else float('inf')
            if diff != 0:
                print(f"    Class {class_id}: {before_count} -> {after_count} ({diff:+d}, {diff_pct:+.1f}%)")

    return result.astype(mask.dtype)


def validate_mask(mask: np.ndarray, num_classes: int = 21) -> dict:
    """
    Validate a segmentation mask and report statistics.

    Args:
        mask: Segmentation mask to validate
        num_classes: Expected number of classes (default: 21)

    Returns:
        Dictionary with validation results
    """
    results = {
        'valid': True,
        'shape': mask.shape,
        'dtype': str(mask.dtype),
        'unique_classes': np.unique(mask).tolist(),
        'num_classes_found': len(np.unique(mask)),
        'class_counts': {},
        'warnings': [],
        'errors': []
    }

    # Check for invalid class IDs
    invalid_ids = [c for c in results['unique_classes'] if c < 0 or c >= num_classes]
    if invalid_ids:
        results['errors'].append(f"Invalid class IDs found: {invalid_ids}")
        results['valid'] = False

    # Count pixels per class
    for class_id in results['unique_classes']:
        count = int(np.sum(mask == class_id))
        results['class_counts'][int(class_id)] = count

    # Check for very small classes (might indicate noise)
    total_pixels = mask.size
    for class_id, count in results['class_counts'].items():
        pct = count / total_pixels * 100
        if 0 < pct < 0.01:  # Less than 0.01%
            results['warnings'].append(
                f"Class {class_id} has only {count} pixels ({pct:.4f}%) - might be noise"
            )

    return results


# =============================================================================
# CLI INTERFACE
# =============================================================================

def main():
    """Command-line interface for mask post-processing."""
    import argparse

    parser = argparse.ArgumentParser(
        description='Post-process segmentation masks to remove noise and artifacts',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic cleanup
  python mask_postprocess.py input_mask.png output_mask.png

  # With all options
  python mask_postprocess.py input.png output.png --morph --min-region 50 --smooth --sigma 1.5

  # Just validate a mask
  python mask_postprocess.py input_mask.png --validate
        """
    )

    parser.add_argument('input', type=str, help='Input mask file (PNG, NPY)')
    parser.add_argument('output', type=str, nargs='?', help='Output mask file')
    parser.add_argument('--no-morph', action='store_true',
                        help='Disable morphological cleanup')
    parser.add_argument('--morph-kernel', type=int, default=3,
                        help='Morphological kernel size (default: 3)')
    parser.add_argument('--min-region', type=int, default=100,
                        help='Minimum region size in pixels (default: 100, 0 to disable)')
    parser.add_argument('--smooth', action='store_true',
                        help='Enable boundary smoothing')
    parser.add_argument('--sigma', type=float, default=1.0,
                        help='Gaussian sigma for smoothing (default: 1.0)')
    parser.add_argument('--fill-holes', action='store_true',
                        help='Fill holes in non-background classes')
    parser.add_argument('--max-hole-size', type=int, default=500,
                        help='Maximum hole size to fill (default: 500)')
    parser.add_argument('--validate', action='store_true',
                        help='Validate mask and print statistics (no output)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print verbose progress')

    args = parser.parse_args()

    # Load input mask
    from pathlib import Path

    input_path = Path(args.input)
    if input_path.suffix == '.npy':
        mask = np.load(args.input).astype(np.int32)
    else:
        try:
            from PIL import Image
            img = Image.open(args.input)
            if img.mode == 'RGB' or img.mode == 'RGBA':
                img = img.convert('L')
            mask = np.array(img).astype(np.int32)
        except ImportError:
            print("ERROR: PIL required for image files: pip install pillow")
            return 1

    if args.verbose:
        print(f"Loaded mask: {mask.shape}, dtype: {mask.dtype}")
        print(f"Class range: [{mask.min()}, {mask.max()}]")

    # Validate only mode
    if args.validate:
        results = validate_mask(mask)
        print(f"\nMask Validation Results:")
        print(f"  Shape: {results['shape']}")
        print(f"  Classes found: {results['num_classes_found']}")
        print(f"  Class IDs: {results['unique_classes']}")
        print(f"\n  Class pixel counts:")
        for class_id, count in sorted(results['class_counts'].items()):
            pct = count / mask.size * 100
            print(f"    {class_id}: {count:>10} pixels ({pct:>6.2f}%)")

        if results['warnings']:
            print(f"\n  Warnings:")
            for w in results['warnings']:
                print(f"    - {w}")

        if results['errors']:
            print(f"\n  Errors:")
            for e in results['errors']:
                print(f"    - {e}")

        return 0 if results['valid'] else 1

    # Require output for processing
    if not args.output:
        parser.error("output is required when not using --validate")

    # Apply post-processing
    cleaned = postprocess_mask(
        mask,
        morph=not args.no_morph,
        morph_kernel_size=args.morph_kernel,
        min_region=args.min_region,
        smooth=args.smooth,
        sigma=args.sigma,
        fill_holes=args.fill_holes,
        max_hole_size=args.max_hole_size,
        verbose=args.verbose
    )

    # Save output
    output_path = Path(args.output)
    if output_path.suffix == '.npy':
        np.save(args.output, cleaned)
    else:
        try:
            from PIL import Image
            img = Image.fromarray(cleaned.astype(np.uint8))
            img.save(args.output)
        except ImportError:
            print("ERROR: PIL required for image files: pip install pillow")
            return 1

    print(f"Saved post-processed mask to: {args.output}")
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())
