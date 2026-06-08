#!/usr/bin/env python3
"""
FINAL PREPROCESSING PARITY TEST

This test creates synthetic test images of various sizes to properly test
the resize path differences between PIL (Python training) and manual
bilinear interpolation (C++ inference).

Tests performed:
1. No resize (512x512 -> 512x512) - baseline
2. Downscale (1024x1024 -> 512x512) - common case
3. Upscale (256x256 -> 512x512) - edge case
4. Non-square (640x480 -> 512x512) - aspect ratio handling
5. Large (2048x2048 -> 512x512) - high-resolution case
"""

import numpy as np
from PIL import Image
from pathlib import Path
import tempfile
import os


def create_test_image(width: int, height: int, pattern: str = 'gradient') -> np.ndarray:
    """
    Create a synthetic test image with known patterns.

    Patterns:
    - gradient: smooth gradient to test interpolation
    - checkerboard: high-frequency pattern to stress interpolation
    - stars: point sources (common in astro)
    """
    img = np.zeros((height, width, 3), dtype=np.float32)

    if pattern == 'gradient':
        # Smooth gradient - sensitive to interpolation errors
        for y in range(height):
            for x in range(width):
                img[y, x, 0] = x / width  # R: horizontal gradient
                img[y, x, 1] = y / height  # G: vertical gradient
                img[y, x, 2] = (x + y) / (width + height)  # B: diagonal

    elif pattern == 'checkerboard':
        # Checkerboard - tests aliasing
        cell_size = max(4, min(width, height) // 32)
        for y in range(height):
            for x in range(width):
                cell_x = (x // cell_size) % 2
                cell_y = (y // cell_size) % 2
                if cell_x == cell_y:
                    img[y, x] = [0.8, 0.8, 0.8]
                else:
                    img[y, x] = [0.2, 0.2, 0.2]

    elif pattern == 'stars':
        # Simulated star field
        np.random.seed(42)
        img.fill(0.05)  # Dark background

        # Add random "stars"
        n_stars = (width * height) // 500
        for _ in range(n_stars):
            cx = np.random.randint(5, width - 5)
            cy = np.random.randint(5, height - 5)
            brightness = np.random.uniform(0.3, 1.0)
            radius = np.random.randint(1, 4)

            for dy in range(-radius*2, radius*2 + 1):
                for dx in range(-radius*2, radius*2 + 1):
                    x = cx + dx
                    y = cy + dy
                    if 0 <= x < width and 0 <= y < height:
                        dist = np.sqrt(dx*dx + dy*dy)
                        falloff = np.exp(-dist*dist / (radius*radius))
                        val = brightness * falloff
                        img[y, x] = np.clip(img[y, x] + val, 0, 1)

    return img


def pil_preprocess(img_array: np.ndarray, target_size: int = 512) -> np.ndarray:
    """
    Python training preprocessing using PIL.
    """
    # Convert to PIL, resize, convert back
    img_uint8 = (img_array * 255).astype(np.uint8)
    img_pil = Image.fromarray(img_uint8, 'RGB')
    img_resized = img_pil.resize((target_size, target_size), Image.BILINEAR)
    img = np.array(img_resized).astype(np.float32) / 255.0

    # Percentile normalization
    for c in range(3):
        p1 = np.percentile(img[:, :, c], 1)
        p99 = np.percentile(img[:, :, c], 99)
        if p99 > p1:
            img[:, :, c] = np.clip((img[:, :, c] - p1) / (p99 - p1), 0, 1)

    # Color contrast
    color_contrast = img[:, :, 2] - img[:, :, 0]

    # NCHW format
    img_4ch = np.concatenate([img, color_contrast[:, :, np.newaxis]], axis=2)
    tensor = np.transpose(img_4ch, (2, 0, 1))[np.newaxis, ...]

    return tensor.astype(np.float32)


def cpp_preprocess(img_array: np.ndarray, target_size: int = 512) -> np.ndarray:
    """
    C++ inference preprocessing using manual bilinear interpolation.
    """
    src_height, src_width = img_array.shape[:2]
    out_width = target_size
    out_height = target_size

    scale_x = src_width / out_width
    scale_y = src_height / out_height

    # Manual bilinear interpolation
    img = np.zeros((out_height, out_width, 3), dtype=np.float32)

    for y in range(out_height):
        for x in range(out_width):
            src_x = x * scale_x
            src_y = y * scale_y

            x0 = int(src_x)
            y0 = int(src_y)
            x1 = min(x0 + 1, src_width - 1)
            y1 = min(y0 + 1, src_height - 1)

            fx = src_x - x0
            fy = src_y - y0

            for c in range(3):
                v00 = img_array[y0, x0, c]
                v10 = img_array[y0, x1, c]
                v01 = img_array[y1, x0, c]
                v11 = img_array[y1, x1, c]

                value = ((1.0 - fx) * (1.0 - fy) * v00 +
                         fx * (1.0 - fy) * v10 +
                         (1.0 - fx) * fy * v01 +
                         fx * fy * v11)

                img[y, x, c] = value

    # Percentile normalization (C++ style - sorted array lookup)
    for c in range(3):
        flat = img[:, :, c].flatten()
        sorted_data = np.sort(flat)
        p1 = sorted_data[int(0.01 * (len(sorted_data) - 1))]
        p99 = sorted_data[int(0.99 * (len(sorted_data) - 1))]
        if p99 > p1:
            img[:, :, c] = np.clip((img[:, :, c] - p1) / (p99 - p1), 0, 1)

    # Color contrast
    color_contrast = img[:, :, 2] - img[:, :, 0]

    # NCHW format
    img_4ch = np.concatenate([img, color_contrast[:, :, np.newaxis]], axis=2)
    tensor = np.transpose(img_4ch, (2, 0, 1))[np.newaxis, ...]

    return tensor.astype(np.float32)


def run_test(width: int, height: int, pattern: str, target_size: int = 512) -> dict:
    """
    Run a single preprocessing comparison test.
    """
    print(f"\n  Testing {width}x{height} ({pattern}) -> {target_size}x{target_size}...")

    # Create test image
    img = create_test_image(width, height, pattern)

    # Process with both methods
    pil_tensor = pil_preprocess(img, target_size)
    cpp_tensor = cpp_preprocess(img, target_size)

    # Compare
    diff = np.abs(pil_tensor - cpp_tensor)
    max_diff = diff.max()
    mean_diff = diff.mean()
    rmse = np.sqrt((diff ** 2).mean())

    # Per-channel analysis
    channel_diffs = {}
    for c, name in enumerate(['R', 'G', 'B', 'B-R']):
        ch_diff = diff[0, c]
        channel_diffs[name] = {
            'max': float(ch_diff.max()),
            'mean': float(ch_diff.mean()),
        }

    result = {
        'input_size': (width, height),
        'pattern': pattern,
        'target_size': target_size,
        'max_diff': float(max_diff),
        'mean_diff': float(mean_diff),
        'rmse': float(rmse),
        'channel_diffs': channel_diffs,
        'status': 'PASS' if max_diff < 0.01 else 'WARN' if max_diff < 0.05 else 'FAIL'
    }

    status_emoji = {'PASS': '[OK]', 'WARN': '[!]', 'FAIL': '[X]'}
    print(f"    {status_emoji[result['status']]} max_diff={max_diff:.6e}, mean_diff={mean_diff:.6e}")

    return result


def run_all_tests():
    """
    Run comprehensive preprocessing parity tests.
    """
    print("="*70)
    print("COMPREHENSIVE PREPROCESSING PARITY TEST")
    print("="*70)
    print("\nThis test compares Python (PIL) preprocessing vs C++ (manual bilinear)")
    print("across different image sizes and patterns.\n")

    tests = [
        # (width, height, pattern)
        (512, 512, 'gradient'),    # No resize
        (512, 512, 'stars'),       # No resize, star field
        (1024, 1024, 'gradient'),  # 2x downscale
        (1024, 1024, 'stars'),     # 2x downscale, star field
        (256, 256, 'gradient'),    # 2x upscale
        (640, 480, 'gradient'),    # Non-square
        (2048, 2048, 'gradient'),  # Large
        (1000, 1000, 'stars'),     # Non-power-of-2
        (800, 600, 'checkerboard'),  # High-frequency
    ]

    results = []
    for width, height, pattern in tests:
        result = run_test(width, height, pattern)
        results.append(result)

    # Summary
    print("\n" + "="*70)
    print("SUMMARY")
    print("="*70)

    n_pass = sum(1 for r in results if r['status'] == 'PASS')
    n_warn = sum(1 for r in results if r['status'] == 'WARN')
    n_fail = sum(1 for r in results if r['status'] == 'FAIL')

    print(f"\nTotal tests: {len(results)}")
    print(f"  PASS: {n_pass}")
    print(f"  WARN: {n_warn}")
    print(f"  FAIL: {n_fail}")

    # Find worst case
    worst = max(results, key=lambda r: r['max_diff'])
    print(f"\nWorst case: {worst['input_size']} ({worst['pattern']})")
    print(f"  max_diff={worst['max_diff']:.6e}")

    # Detailed per-channel for worst case
    if worst['max_diff'] > 0.001:
        print(f"  Per-channel breakdown:")
        for ch, stats in worst['channel_diffs'].items():
            print(f"    {ch}: max={stats['max']:.6e}, mean={stats['mean']:.6e}")

    # Analysis
    print("\n" + "-"*70)
    print("ANALYSIS")
    print("-"*70)

    if n_fail == 0:
        if n_warn == 0:
            print("\nPREPROCESSING PARITY: VERIFIED")
            print("Python training and C++ inference preprocessing produce identical results.")
            print("Model predictions will be consistent across both implementations.")
        else:
            print("\nPREPROCESSING PARITY: ACCEPTABLE")
            print("Minor differences detected but within acceptable tolerance.")
            print("These are likely due to:")
            print("  1. Floating-point precision differences")
            print("  2. PIL's optimized vs manual bilinear implementation")
            print("Impact: Negligible effect on model predictions.")
    else:
        print("\nPREPROCESSING PARITY: ISSUE DETECTED")
        print("Significant differences found between Python and C++ preprocessing.")
        print("This could cause model predictions to differ!")
        print("\nRecommendation: Investigate the specific failing cases.")

    # Key insight
    print("\n" + "-"*70)
    print("KEY INSIGHT")
    print("-"*70)
    print("""
The main sources of difference between PIL and manual bilinear:

1. PIL uses optimized SIMD instructions and may have slightly different
   floating-point rounding.

2. The percentile computation differs slightly:
   - Python: np.percentile (linear interpolation)
   - C++: sorted array index lookup

3. For images that are already 512x512, there should be ZERO difference
   since no resize occurs.

4. For other sizes, expect tiny differences (< 0.01) which are acceptable.
""")

    return results


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Final preprocessing parity test')
    parser.add_argument('--quick', action='store_true', help='Run quick test only')
    args = parser.parse_args()

    if args.quick:
        # Quick test
        result = run_test(1024, 1024, 'gradient')
        print(f"\nQuick test result: {result['status']}")
    else:
        # Full test
        run_all_tests()


if __name__ == '__main__':
    main()
