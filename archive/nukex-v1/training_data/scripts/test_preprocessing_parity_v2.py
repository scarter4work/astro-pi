#!/usr/bin/env python3
"""
CRITICAL: Test Preprocessing Parity v2 - Comprehensive Analysis

This version specifically tests the RESIZE step differences, which is the
most likely source of mismatch between Python and C++.

The v1 test showed exact match because we were using PIL to resize in both cases
(Python training and C++ simulation). This test separates them properly.

Key insight: The Python training uses PIL.Image.resize(BILINEAR), while
C++ uses manual bilinear interpolation. These should be very close but
may have tiny floating-point differences.
"""

import numpy as np
from PIL import Image
from pathlib import Path
import json
from datetime import datetime


def pil_resize_and_preprocess(img_path: str, target_size: int = 512) -> tuple:
    """
    Exact Python training preprocessing using PIL resize.
    This is what the model was trained on.
    """
    # Load and resize using PIL (what train_21class.py does)
    img = Image.open(img_path).convert('RGB')
    img = img.resize((target_size, target_size), Image.BILINEAR)
    img = np.array(img).astype(np.float32) / 255.0

    # Percentile normalization
    percentiles = {}
    for c in range(3):
        p1 = np.percentile(img[:, :, c], 1)
        p99 = np.percentile(img[:, :, c], 99)
        percentiles[f'ch{c}_p1'] = float(p1)
        percentiles[f'ch{c}_p99'] = float(p99)
        if p99 > p1:
            img[:, :, c] = np.clip((img[:, :, c] - p1) / (p99 - p1), 0, 1)
        else:
            img[:, :, c] = np.clip(img[:, :, c], 0, 1)

    # Color contrast
    color_contrast = img[:, :, 2] - img[:, :, 0]

    # Stack and convert to NCHW
    img_4ch = np.concatenate([img, color_contrast[:, :, np.newaxis]], axis=2)
    tensor = np.transpose(img_4ch, (2, 0, 1))[np.newaxis, ...]

    return tensor.astype(np.float32), percentiles


def manual_bilinear_resize_and_preprocess(img_path: str, target_size: int = 512) -> tuple:
    """
    C++ inference preprocessing using manual bilinear interpolation.
    This simulates what SegmentationModel.cpp does.
    """
    # Load original image (no PIL resize)
    img_pil = Image.open(img_path).convert('RGB')
    src_width, src_height = img_pil.size
    img_array = np.array(img_pil).astype(np.float32) / 255.0

    out_width = target_size
    out_height = target_size

    # Scale factors
    scale_x = src_width / out_width
    scale_y = src_height / out_height

    # Allocate output
    channel_r = np.zeros((out_height, out_width), dtype=np.float32)
    channel_g = np.zeros((out_height, out_width), dtype=np.float32)
    channel_b = np.zeros((out_height, out_width), dtype=np.float32)

    # Manual bilinear interpolation (matching C++ exactly)
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

            # Bilinear for each channel
            for c, ch_out in enumerate([channel_r, channel_g, channel_b]):
                v00 = img_array[y0, x0, c]
                v10 = img_array[y0, x1, c]
                v01 = img_array[y1, x0, c]
                v11 = img_array[y1, x1, c]

                value = ((1.0 - fx) * (1.0 - fy) * v00 +
                         fx * (1.0 - fy) * v10 +
                         (1.0 - fx) * fy * v01 +
                         fx * fy * v11)

                ch_out[y, x] = value

    # Stack channels
    img = np.stack([channel_r, channel_g, channel_b], axis=2)

    # Percentile normalization
    percentiles = {}
    for c in range(3):
        flat = img[:, :, c].flatten()
        sorted_data = np.sort(flat)
        p1_idx = int(0.01 * (len(sorted_data) - 1))
        p99_idx = int(0.99 * (len(sorted_data) - 1))
        p1 = sorted_data[p1_idx]
        p99 = sorted_data[p99_idx]

        percentiles[f'ch{c}_p1'] = float(p1)
        percentiles[f'ch{c}_p99'] = float(p99)

        range_val = p99 - p1 if p99 > p1 else 1.0
        img[:, :, c] = np.clip((img[:, :, c] - p1) / range_val, 0, 1)

    # Color contrast
    color_contrast = img[:, :, 2] - img[:, :, 0]

    # Stack and convert to NCHW
    img_4ch = np.concatenate([img, color_contrast[:, :, np.newaxis]], axis=2)
    tensor = np.transpose(img_4ch, (2, 0, 1))[np.newaxis, ...]

    return tensor.astype(np.float32), percentiles


def analyze_resize_differences(img_path: str, target_size: int = 512):
    """
    Compare PIL resize vs manual bilinear resize to quantify differences.
    """
    print(f"\n{'='*70}")
    print(f"RESIZE METHOD COMPARISON")
    print(f"Image: {img_path}")
    print(f"{'='*70}\n")

    # Load original
    img_pil = Image.open(img_path).convert('RGB')
    orig_size = img_pil.size
    print(f"Original size: {orig_size}")
    print(f"Target size: {target_size}x{target_size}\n")

    # PIL resize
    print("Computing PIL resize...")
    pil_tensor, pil_perc = pil_resize_and_preprocess(img_path, target_size)

    # Manual bilinear resize (C++ style)
    print("Computing manual bilinear resize (C++ style)...")
    cpp_tensor, cpp_perc = manual_bilinear_resize_and_preprocess(img_path, target_size)

    # Compare
    print("\n" + "-"*50)
    print("COMPARISON: PIL BILINEAR vs C++ MANUAL BILINEAR")
    print("-"*50)

    channel_names = ['R', 'G', 'B', 'B-R']
    for c, cname in enumerate(channel_names):
        ch_pil = pil_tensor[0, c]
        ch_cpp = cpp_tensor[0, c]
        diff = np.abs(ch_pil - ch_cpp)

        print(f"\nChannel {cname}:")
        print(f"  PIL:     [{ch_pil.min():.6f}, {ch_pil.max():.6f}] mean={ch_pil.mean():.6f}")
        print(f"  C++:     [{ch_cpp.min():.6f}, {ch_cpp.max():.6f}] mean={ch_cpp.mean():.6f}")
        print(f"  Diff:    max={diff.max():.6e} mean={diff.mean():.6e}")

    # Overall metrics
    total_diff = np.abs(pil_tensor - cpp_tensor)
    max_diff = total_diff.max()
    mean_diff = total_diff.mean()
    rmse = np.sqrt((total_diff ** 2).mean())

    print(f"\n" + "-"*50)
    print("OVERALL METRICS")
    print("-"*50)
    print(f"Max absolute difference: {max_diff:.6e}")
    print(f"Mean absolute difference: {mean_diff:.6e}")
    print(f"RMSE: {rmse:.6e}")

    # Percentile comparison
    print(f"\n" + "-"*50)
    print("PERCENTILE COMPARISON (affects normalization)")
    print("-"*50)
    for key in pil_perc:
        pil_val = pil_perc[key]
        cpp_val = cpp_perc.get(key, 'N/A')
        diff = abs(pil_val - cpp_val)
        status = "OK" if diff < 0.001 else "DIFF!"
        print(f"  {key}: PIL={pil_val:.6f}, C++={cpp_val:.6f}, diff={diff:.6e} [{status}]")

    # Verdict
    print(f"\n" + "="*70)
    print("ANALYSIS")
    print("="*70)

    TOLERANCE = 0.01

    if max_diff < TOLERANCE:
        print(f"\nSTATUS: ACCEPTABLE")
        print(f"The difference ({max_diff:.6f}) is within tolerance ({TOLERANCE}).")
        print(f"\nThis small difference is expected due to:")
        print(f"  1. Floating-point precision differences")
        print(f"  2. Minor variations in bilinear interpolation algorithms")
        print(f"\nIMPACT: Negligible - model predictions will be consistent.")
    elif max_diff < 0.05:
        print(f"\nSTATUS: MINOR CONCERN")
        print(f"The difference ({max_diff:.6f}) is slightly above tolerance.")
        print(f"\nIMPACT: May cause minor prediction variations. Monitor closely.")
    else:
        print(f"\nSTATUS: CRITICAL ISSUE")
        print(f"The difference ({max_diff:.6f}) is significant!")
        print(f"\nIMPACT: Model will produce inconsistent predictions!")

    # Show where the biggest differences are
    print(f"\n" + "-"*50)
    print("DIFFERENCE LOCATION ANALYSIS")
    print("-"*50)

    # Find pixels with largest differences
    diff_map = total_diff[0].sum(axis=0)  # Sum across channels
    y_max, x_max = np.unravel_index(np.argmax(diff_map), diff_map.shape)
    print(f"Largest difference at pixel ({x_max}, {y_max})")

    # Check if differences are at edges (common for resize algorithms)
    edge_diff = np.mean([
        diff_map[0, :].mean(),
        diff_map[-1, :].mean(),
        diff_map[:, 0].mean(),
        diff_map[:, -1].mean()
    ])
    center_diff = diff_map[10:-10, 10:-10].mean()
    print(f"Average edge difference: {edge_diff:.6e}")
    print(f"Average center difference: {center_diff:.6e}")

    if edge_diff > center_diff * 2:
        print("NOTE: Differences are concentrated at edges (common for resize)")
    else:
        print("NOTE: Differences are evenly distributed")

    return {
        'max_diff': float(max_diff),
        'mean_diff': float(mean_diff),
        'rmse': float(rmse),
        'status': 'OK' if max_diff < TOLERANCE else 'WARN' if max_diff < 0.05 else 'FAIL'
    }


def test_with_larger_image():
    """
    Test with a larger image where resize differences are more apparent.
    """
    # Find a larger test image
    base_path = Path('/home/scarter4work/projects/NukeX/training_data')

    # Try to find images of different sizes
    test_images = []
    for pattern in ['*_viz.png', '*_img.png']:
        found = list(base_path.rglob(pattern))[:5]
        test_images.extend(found)

    if not test_images:
        print("No test images found!")
        return

    print(f"\n{'#'*70}")
    print("MULTI-IMAGE TEST")
    print(f"{'#'*70}")

    results = []
    for img_path in test_images[:3]:  # Test up to 3 images
        try:
            result = analyze_resize_differences(str(img_path))
            result['image'] = str(img_path)
            results.append(result)
        except Exception as e:
            print(f"Error processing {img_path}: {e}")

    # Summary
    print(f"\n{'='*70}")
    print("MULTI-IMAGE SUMMARY")
    print(f"{'='*70}")

    for r in results:
        img_name = Path(r['image']).name
        print(f"{r['status']}: {img_name} - max_diff={r['max_diff']:.6e}")

    all_ok = all(r['status'] == 'OK' for r in results)
    if all_ok:
        print(f"\nALL TESTS PASSED: Preprocessing parity is verified!")
    else:
        print(f"\nSOME TESTS FAILED: Investigate preprocessing differences!")


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Test preprocessing parity v2')
    parser.add_argument('--image', '-i', help='Image to test')
    parser.add_argument('--multi', action='store_true', help='Run multi-image test')

    args = parser.parse_args()

    if args.image:
        analyze_resize_differences(args.image)
    elif args.multi:
        test_with_larger_image()
    else:
        # Default single test
        default_img = '/home/scarter4work/projects/NukeX/training_data/labeled/bright_emission/M42/mastDownload/HST/j93k07hvq/j93k07hvq_drc_viz.png'
        if Path(default_img).exists():
            analyze_resize_differences(default_img)
        else:
            print("No image specified. Use --image <path> or --multi")


if __name__ == '__main__':
    main()
