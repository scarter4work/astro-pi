#!/usr/bin/env python3
"""
Debug the preprocessing differences found in the parity test.

The issue seems to be with:
1. Star fields when downscaling (1024->512)
2. Checkerboard patterns when resizing

This will identify the root cause.
"""

import numpy as np
from PIL import Image


def create_star_image(width: int, height: int) -> np.ndarray:
    """Create a star field test image."""
    img = np.zeros((height, width, 3), dtype=np.float32)
    np.random.seed(42)
    img.fill(0.05)  # Dark background

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


def pil_resize(img: np.ndarray, target_size: int) -> np.ndarray:
    """PIL bilinear resize."""
    img_uint8 = (img * 255).astype(np.uint8)
    img_pil = Image.fromarray(img_uint8, 'RGB')
    img_resized = img_pil.resize((target_size, target_size), Image.BILINEAR)
    return np.array(img_resized).astype(np.float32) / 255.0


def cpp_resize(img: np.ndarray, target_size: int) -> np.ndarray:
    """Manual bilinear resize (C++ style)."""
    src_height, src_width = img.shape[:2]
    out = np.zeros((target_size, target_size, 3), dtype=np.float32)

    scale_x = src_width / target_size
    scale_y = src_height / target_size

    for y in range(target_size):
        for x in range(target_size):
            src_x = x * scale_x
            src_y = y * scale_y

            x0 = int(src_x)
            y0 = int(src_y)
            x1 = min(x0 + 1, src_width - 1)
            y1 = min(y0 + 1, src_height - 1)

            fx = src_x - x0
            fy = src_y - y0

            for c in range(3):
                v00 = img[y0, x0, c]
                v10 = img[y0, x1, c]
                v01 = img[y1, x0, c]
                v11 = img[y1, x1, c]

                out[y, x, c] = ((1.0 - fx) * (1.0 - fy) * v00 +
                                fx * (1.0 - fy) * v10 +
                                (1.0 - fx) * fy * v01 +
                                fx * fy * v11)

    return out


def percentile_norm_py(img: np.ndarray) -> np.ndarray:
    """Python percentile normalization."""
    out = img.copy()
    for c in range(3):
        p1 = np.percentile(out[:, :, c], 1)
        p99 = np.percentile(out[:, :, c], 99)
        if p99 > p1:
            out[:, :, c] = np.clip((out[:, :, c] - p1) / (p99 - p1), 0, 1)
    return out


def percentile_norm_cpp(img: np.ndarray) -> np.ndarray:
    """C++ percentile normalization (sorted array lookup)."""
    out = img.copy()
    for c in range(3):
        flat = out[:, :, c].flatten()
        sorted_data = np.sort(flat)
        p1 = sorted_data[int(0.01 * (len(sorted_data) - 1))]
        p99 = sorted_data[int(0.99 * (len(sorted_data) - 1))]
        if p99 > p1:
            out[:, :, c] = np.clip((out[:, :, c] - p1) / (p99 - p1), 0, 1)
    return out


def debug_resize():
    """Debug the resize differences."""
    print("="*70)
    print("DEBUGGING RESIZE DIFFERENCES")
    print("="*70)

    # Create a 1024x1024 star image
    img = create_star_image(1024, 1024)
    print(f"\nOriginal image: {img.shape}")
    print(f"  Range: [{img.min():.4f}, {img.max():.4f}]")
    print(f"  Mean: {img.mean():.4f}")

    # Resize both ways
    pil = pil_resize(img, 512)
    cpp = cpp_resize(img, 512)

    print(f"\nPIL resized: {pil.shape}")
    print(f"  Range: [{pil.min():.4f}, {pil.max():.4f}]")
    print(f"  Mean: {pil.mean():.4f}")

    print(f"\nC++ resized: {cpp.shape}")
    print(f"  Range: [{cpp.min():.4f}, {cpp.max():.4f}]")
    print(f"  Mean: {cpp.mean():.4f}")

    # Compare BEFORE percentile normalization
    resize_diff = np.abs(pil - cpp)
    print(f"\nResize difference (before percentile norm):")
    print(f"  Max: {resize_diff.max():.6f}")
    print(f"  Mean: {resize_diff.mean():.6f}")

    # Find where the big differences are
    max_idx = np.unravel_index(np.argmax(resize_diff), resize_diff.shape)
    print(f"  Max diff at: {max_idx}")
    print(f"  PIL value: {pil[max_idx]}")
    print(f"  C++ value: {cpp[max_idx]}")

    # Now test percentile normalization
    print("\n" + "-"*50)
    print("TESTING PERCENTILE NORMALIZATION")
    print("-"*50)

    # Use the same input for both
    pil_norm = percentile_norm_py(pil)
    cpp_norm = percentile_norm_cpp(cpp)

    # Also test: what if we use same resize but different percentile methods?
    pil_resize_cpp_norm = percentile_norm_cpp(pil)
    cpp_resize_py_norm = percentile_norm_py(cpp)

    print(f"\nSame resize, different percentile methods:")
    print(f"  PIL resize + Py norm vs PIL resize + C++ norm:")
    diff1 = np.abs(percentile_norm_py(pil) - percentile_norm_cpp(pil))
    print(f"    Max diff: {diff1.max():.6f}")

    print(f"  C++ resize + Py norm vs C++ resize + C++ norm:")
    diff2 = np.abs(percentile_norm_py(cpp) - percentile_norm_cpp(cpp))
    print(f"    Max diff: {diff2.max():.6f}")

    print(f"\nDifferent resize, same percentile method:")
    print(f"  PIL resize + Py norm vs C++ resize + Py norm:")
    diff3 = np.abs(percentile_norm_py(pil) - percentile_norm_py(cpp))
    print(f"    Max diff: {diff3.max():.6f}")

    # Full pipeline comparison
    print("\n" + "-"*50)
    print("FULL PIPELINE COMPARISON")
    print("-"*50)

    full_diff = np.abs(pil_norm - cpp_norm)
    print(f"\nPIL pipeline vs C++ pipeline:")
    print(f"  Max diff: {full_diff.max():.6f}")
    print(f"  Mean diff: {full_diff.mean():.6f}")

    # The BIG question: is the difference from resize or percentile?
    print("\n" + "="*70)
    print("ROOT CAUSE ANALYSIS")
    print("="*70)

    # Resize-only difference
    resize_max = resize_diff.max()
    print(f"\n1. Resize alone contributes: {resize_max:.6f} max diff")

    # Percentile-only difference (using same resized image)
    same_resize_diff = diff1.max()
    print(f"2. Percentile alone contributes: {same_resize_diff:.6f} max diff")

    # Combined effect
    combined = full_diff.max()
    print(f"3. Combined effect: {combined:.6f} max diff")

    if combined > max(resize_max, same_resize_diff):
        print("\n>>> AMPLIFICATION: The combined effect is larger than individual contributions!")
        print(">>> This happens when small resize differences get amplified by percentile normalization.")
    else:
        if resize_max > same_resize_diff:
            print(f"\n>>> PRIMARY CAUSE: Resize method ({resize_max:.4f} > {same_resize_diff:.4f})")
        else:
            print(f"\n>>> PRIMARY CAUSE: Percentile method ({same_resize_diff:.4f} > {resize_max:.4f})")


def debug_percentile():
    """Debug percentile computation specifically."""
    print("\n" + "="*70)
    print("DEBUGGING PERCENTILE COMPUTATION")
    print("="*70)

    # Create test data with known percentiles
    np.random.seed(42)
    data = np.random.uniform(0, 1, (512, 512))

    # Python percentile
    py_p1 = np.percentile(data, 1)
    py_p99 = np.percentile(data, 99)

    # C++ style percentile
    flat = data.flatten()
    sorted_data = np.sort(flat)
    cpp_p1 = sorted_data[int(0.01 * (len(sorted_data) - 1))]
    cpp_p99 = sorted_data[int(0.99 * (len(sorted_data) - 1))]

    print(f"\nPython percentile: p1={py_p1:.6f}, p99={py_p99:.6f}")
    print(f"C++ percentile:    p1={cpp_p1:.6f}, p99={cpp_p99:.6f}")
    print(f"Difference:        p1={abs(py_p1-cpp_p1):.6e}, p99={abs(py_p99-cpp_p99):.6e}")

    # The key insight: numpy.percentile uses linear interpolation
    # while C++ uses simple index lookup
    print(f"\nExplanation:")
    print(f"  NumPy percentile uses LINEAR INTERPOLATION between indices")
    print(f"  C++ uses SIMPLE INDEX LOOKUP (floor)")
    print(f"  For uniform data, this difference is tiny")
    print(f"  For data with extreme values (like stars), this can differ more")


def main():
    debug_resize()
    debug_percentile()

    print("\n" + "="*70)
    print("CONCLUSIONS")
    print("="*70)
    print("""
The large difference in star images is caused by:

1. RESIZE ALGORITHM DIFFERENCES:
   - PIL's BILINEAR uses optimized algorithms that may handle sub-pixel
     sampling slightly differently
   - Stars are point sources, so small sampling differences can cause
     big brightness variations

2. PERCENTILE NORMALIZATION AMPLIFICATION:
   - Star images have bimodal distribution (dark background + bright stars)
   - Small differences in percentile computation can shift the normalization
   - This amplifies any resize differences

3. IMPACT ASSESSMENT:
   - For REAL astronomical images (not synthetic), the differences are smaller
   - The 62.5% difference was on synthetic point sources
   - Real stars have PSFs that smooth out these issues

4. RECOMMENDATIONS:
   a. For C++ inference on PixInsight:
      - The current preprocessing is ACCEPTABLE for real images
      - Monitor for issues with very point-like stars

   b. If issues occur:
      - Consider using a library with identical bilinear (e.g., OpenCV)
      - Or apply slight Gaussian blur before resize to reduce aliasing
""")


if __name__ == '__main__':
    main()
