#!/usr/bin/env python3
"""
Synthetic Color Augmentation Module for NukeX ML Training Pipeline

This module adds realistic synthetic colors to monochrome training images
to help the model learn color-based features for nebula classification.

The v6 model fails on RGB images because 81% of nebula training data is
monochrome (B-R channel always 0). This augmentation adds synthetic color
to mono images during training.

Classes:
    5: nebula_emission   -> tint RED (Ha emission)
    6: nebula_reflection -> tint BLUE (scattered starlight)
    8: nebula_planetary  -> tint TEAL/GREEN (OIII emission)

Usage:
    from synthetic_color import apply_color_augmentation

    augmented_img = apply_color_augmentation(img, mask, p=0.5)
"""

import numpy as np
from PIL import Image
from pathlib import Path
from scipy.ndimage import gaussian_filter
import random


# Realistic astronomical colors (RGB normalized 0-1)
EMISSION_COLOR = np.array([1.0, 0.4, 0.4])      # Ha red (#ff6666)
REFLECTION_COLOR = np.array([0.4, 0.6, 1.0])    # Blue (#6699ff)
PLANETARY_COLOR = np.array([0.4, 1.0, 0.8])     # OIII teal (#66ffcc)
STAR_COLOR = np.array([1.0, 1.0, 1.0])          # Neutral white

# Class indices
CLASS_EMISSION = 5
CLASS_REFLECTION = 6
CLASS_PLANETARY = 8
STAR_CLASSES = {1, 2, 3, 4}  # star_bright, star_medium, star_faint, star_saturated


def detect_mono_image(img_array: np.ndarray, threshold: float = 2.0) -> bool:
    """
    Detect if an image is monochrome (all channels identical or nearly so).

    Args:
        img_array: Image array (H,W,3) or (H,W) with values 0-255 or 0-1
        threshold: Maximum allowed difference between channels (in 0-255 range)

    Returns:
        True if image is monochrome, False if it has color information
    """
    if img_array.ndim == 2:
        return True

    if img_array.shape[2] == 1:
        return True

    # Normalize to 0-255 range for comparison
    if img_array.max() <= 1.0:
        arr = (img_array * 255).astype(np.float32)
    else:
        arr = img_array.astype(np.float32)

    r = arr[:, :, 0]
    g = arr[:, :, 1]
    b = arr[:, :, 2]

    # Check maximum difference between channels
    max_rg_diff = np.abs(r - g).max()
    max_rb_diff = np.abs(r - b).max()
    max_gb_diff = np.abs(g - b).max()

    max_diff = max(max_rg_diff, max_rb_diff, max_gb_diff)

    return max_diff < threshold


def add_synthetic_color(mono_img: np.ndarray,
                        mask: np.ndarray,
                        object_type: str = None,
                        color_strength: float = 0.6,
                        edge_smoothing: float = 3.0) -> np.ndarray:
    """
    Add synthetic color to a monochrome image based on segmentation mask.

    Args:
        mono_img: Grayscale image (H,W) or (H,W,3) with identical channels
                  Values should be 0-1 or 0-255
        mask: Segmentation mask (H,W) with class labels 0-20
        object_type: Optional hint about object type (e.g., "M42", "NGC7293")
        color_strength: How strongly to apply colors (0-1, default 0.6)
        edge_smoothing: Gaussian sigma for smoothing color transitions

    Returns:
        RGB image (H,W,3) with realistic color tinting, same value range as input
    """
    # Normalize input
    was_uint8 = mono_img.dtype == np.uint8
    if was_uint8:
        mono_img = mono_img.astype(np.float32) / 255.0

    # Ensure we have a grayscale base
    if mono_img.ndim == 3:
        gray = mono_img[:, :, 0].copy()  # Use red channel as grayscale
    else:
        gray = mono_img.copy()

    h, w = gray.shape

    # Initialize output as neutral gray (replicate to RGB)
    output = np.stack([gray, gray, gray], axis=-1)

    # Create smooth masks for each nebula type
    emission_mask = (mask == CLASS_EMISSION).astype(np.float32)
    reflection_mask = (mask == CLASS_REFLECTION).astype(np.float32)
    planetary_mask = (mask == CLASS_PLANETARY).astype(np.float32)

    # Create star mask (stars should stay neutral/white)
    star_mask = np.zeros((h, w), dtype=np.float32)
    for star_class in STAR_CLASSES:
        star_mask += (mask == star_class).astype(np.float32)
    star_mask = np.clip(star_mask, 0, 1)

    # Smooth the masks for gradual color transitions (no hard edges)
    if edge_smoothing > 0:
        emission_mask = gaussian_filter(emission_mask, sigma=edge_smoothing)
        reflection_mask = gaussian_filter(reflection_mask, sigma=edge_smoothing)
        planetary_mask = gaussian_filter(planetary_mask, sigma=edge_smoothing)
        star_mask = gaussian_filter(star_mask, sigma=edge_smoothing / 2)  # Less smoothing for stars

    # Weight color intensity by pixel brightness (brighter nebula = more color)
    # This creates a more realistic look where color is visible in brighter regions
    brightness_weight = np.clip(gray, 0, 1) ** 0.7  # Gamma for better mid-tone response

    # Apply colors with intensity modulation
    for nebula_mask, nebula_color in [
        (emission_mask, EMISSION_COLOR),
        (reflection_mask, REFLECTION_COLOR),
        (planetary_mask, PLANETARY_COLOR),
    ]:
        # Skip if no pixels of this type
        if nebula_mask.max() < 0.01:
            continue

        # Calculate color contribution
        # Color strength varies with both mask value and brightness
        weight = nebula_mask * brightness_weight * color_strength
        weight = weight[:, :, np.newaxis]  # Expand for RGB

        # Create colored version
        colored = gray[:, :, np.newaxis] * nebula_color

        # Blend with output
        # For emission: boost red, reduce blue
        # For reflection: boost blue, reduce red
        # For planetary: boost green/teal
        output = output * (1 - weight) + colored * weight

    # Ensure stars remain white/neutral
    # Stars should not pick up nebula colors
    star_weight = star_mask[:, :, np.newaxis] * 0.8  # Strong but not complete
    neutral_stars = gray[:, :, np.newaxis] * STAR_COLOR
    output = output * (1 - star_weight) + neutral_stars * star_weight

    # Add subtle color variation based on object type hints
    if object_type:
        object_lower = object_type.lower()
        # Famous emission nebulae - enhance red
        if any(x in object_lower for x in ['m42', 'orion', 'rosette', 'lagoon', 'm8', 'carina']):
            # Slight global red enhancement for Ha-rich regions
            output[:, :, 0] = np.clip(output[:, :, 0] * 1.05, 0, 1)
            output[:, :, 2] = np.clip(output[:, :, 2] * 0.95, 0, 1)
        # Famous planetary nebulae - enhance teal/green
        elif any(x in object_lower for x in ['ring', 'm57', 'helix', 'ngc7293', 'dumbbell', 'm27']):
            output[:, :, 1] = np.clip(output[:, :, 1] * 1.03, 0, 1)
            output[:, :, 2] = np.clip(output[:, :, 2] * 1.02, 0, 1)
        # Famous reflection nebulae - enhance blue
        elif any(x in object_lower for x in ['pleiades', 'm45', 'witch head', 'iris']):
            output[:, :, 2] = np.clip(output[:, :, 2] * 1.05, 0, 1)
            output[:, :, 0] = np.clip(output[:, :, 0] * 0.95, 0, 1)

    # Ensure valid range
    output = np.clip(output, 0, 1)

    # Convert back to original range
    if was_uint8:
        output = (output * 255).astype(np.uint8)

    return output


def apply_color_jitter(img: np.ndarray,
                       brightness_range: tuple = (0.9, 1.1),
                       saturation_range: tuple = (0.8, 1.2),
                       hue_shift_range: tuple = (-0.05, 0.05)) -> np.ndarray:
    """
    Apply color jitter augmentation to an already-colored image.

    Args:
        img: RGB image (H,W,3), values 0-1 or 0-255
        brightness_range: Min/max brightness multiplier
        saturation_range: Min/max saturation multiplier
        hue_shift_range: Min/max hue shift (in 0-1 range)

    Returns:
        Color-jittered image
    """
    was_uint8 = img.dtype == np.uint8
    if was_uint8:
        img = img.astype(np.float32) / 255.0

    # Random brightness adjustment
    brightness = random.uniform(*brightness_range)
    img = img * brightness

    # Convert to HSV-like space for saturation adjustment
    # Simple approximation: adjust distance from gray
    gray = np.mean(img, axis=2, keepdims=True)
    saturation = random.uniform(*saturation_range)
    img = gray + (img - gray) * saturation

    # Subtle hue shift (rotate RGB channels slightly)
    hue_shift = random.uniform(*hue_shift_range)
    if abs(hue_shift) > 0.01:
        # Circular shift in color space
        shift_matrix = np.array([
            [1 - abs(hue_shift), abs(hue_shift) * (1 if hue_shift > 0 else 0), abs(hue_shift) * (1 if hue_shift < 0 else 0)],
            [abs(hue_shift) * (1 if hue_shift < 0 else 0), 1 - abs(hue_shift), abs(hue_shift) * (1 if hue_shift > 0 else 0)],
            [abs(hue_shift) * (1 if hue_shift > 0 else 0), abs(hue_shift) * (1 if hue_shift < 0 else 0), 1 - abs(hue_shift)]
        ])
        img = np.clip(np.tensordot(img, shift_matrix, axes=(2, 1)), 0, 1)

    img = np.clip(img, 0, 1)

    if was_uint8:
        img = (img * 255).astype(np.uint8)

    return img


def apply_color_augmentation(img: np.ndarray,
                             mask: np.ndarray,
                             p: float = 0.5,
                             object_type: str = None) -> np.ndarray:
    """
    Apply color augmentation to training images.

    - For monochrome images: add synthetic coloring with probability p
    - For color images: apply color jitter instead

    Args:
        img: Input image (H,W,3), values 0-1 or 0-255
        mask: Segmentation mask (H,W) with class labels
        p: Probability of applying augmentation (default 0.5)
        object_type: Optional object type hint

    Returns:
        Augmented image
    """
    # Random skip
    if random.random() > p:
        return img

    is_mono = detect_mono_image(img)

    if is_mono:
        # Add synthetic color to mono images
        # Vary color strength for diversity
        color_strength = random.uniform(0.4, 0.8)
        edge_smoothing = random.uniform(2.0, 5.0)

        return add_synthetic_color(
            img, mask,
            object_type=object_type,
            color_strength=color_strength,
            edge_smoothing=edge_smoothing
        )
    else:
        # Apply color jitter to already-colored images
        return apply_color_jitter(img)


def test_synthetic_coloring():
    """
    Test the synthetic coloring on sample training data.
    Saves before/after comparison images to /tmp/
    """
    print("=" * 60)
    print("Testing Synthetic Color Augmentation")
    print("=" * 60)

    # Find a sample mono image with nebula content
    data_dirs = [
        Path('/home/scarter4work/projects/NukeX/training_data/labeled/bright_emission'),
        Path('/home/scarter4work/projects/NukeX/training_data/labeled/planetary_nebula'),
    ]

    test_pairs = []

    for data_dir in data_dirs:
        if not data_dir.exists():
            continue
        mask_files = list(data_dir.rglob("*_mask.png"))
        for mask_path in mask_files[:3]:  # Take up to 3 from each
            img_path = mask_path.with_name(mask_path.name.replace("_mask.png", "_img.png"))
            if img_path.exists():
                test_pairs.append((img_path, mask_path))

    if not test_pairs:
        print("ERROR: No test images found!")
        return False

    print(f"\nFound {len(test_pairs)} test images")

    results = []

    for i, (img_path, mask_path) in enumerate(test_pairs[:5]):  # Test up to 5
        print(f"\n--- Test {i+1}: {img_path.name} ---")

        # Load image and mask
        img = Image.open(img_path).convert('RGB')
        mask = Image.open(mask_path)

        img_array = np.array(img)
        mask_array = np.array(mask)

        print(f"Image size: {img_array.shape}")
        print(f"Is monochrome: {detect_mono_image(img_array)}")

        # Get unique classes in mask
        unique_classes = np.unique(mask_array)
        class_names = {
            0: 'background', 5: 'nebula_emission', 6: 'nebula_reflection',
            7: 'nebula_dark', 8: 'nebula_planetary', 1: 'star_bright',
            2: 'star_medium', 3: 'star_faint', 4: 'star_saturated'
        }
        print(f"Classes present: {[class_names.get(c, f'class_{c}') for c in unique_classes]}")

        # Apply synthetic coloring
        colored = add_synthetic_color(img_array, mask_array, color_strength=0.7)

        # Check color differences
        r_diff = np.abs(colored[:,:,0].astype(float) - colored[:,:,2].astype(float)).mean()
        print(f"R-B difference after coloring: {r_diff:.2f}")

        # Create comparison image (original | colored)
        comparison = np.concatenate([img_array, colored], axis=1)

        # Save comparison
        output_path = f'/tmp/synthetic_color_test_{i+1}.png'
        Image.fromarray(comparison).save(output_path)
        print(f"Saved comparison to: {output_path}")

        results.append({
            'input': str(img_path),
            'output': output_path,
            'was_mono': detect_mono_image(img_array),
            'r_b_diff': r_diff
        })

    # Test color jitter on a colored image
    print("\n--- Testing Color Jitter on RGB Image ---")
    rgb_dir = Path('/home/scarter4work/projects/NukeX/training_data/labeled_rgb')
    if rgb_dir.exists():
        rgb_images = list(rgb_dir.rglob("*_img.png"))[:1]
        if rgb_images:
            rgb_path = rgb_images[0]
            rgb_img = np.array(Image.open(rgb_path).convert('RGB'))
            if not detect_mono_image(rgb_img):
                print(f"Testing on: {rgb_path.name}")
                jittered = apply_color_jitter(rgb_img)
                comparison = np.concatenate([rgb_img, jittered], axis=1)
                output_path = '/tmp/synthetic_color_jitter_test.png'
                Image.fromarray(comparison).save(output_path)
                print(f"Saved jitter comparison to: {output_path}")

    print("\n" + "=" * 60)
    print("TEST RESULTS SUMMARY")
    print("=" * 60)

    for r in results:
        print(f"  {Path(r['input']).name}:")
        print(f"    Mono: {r['was_mono']}, R-B diff: {r['r_b_diff']:.2f}")
        print(f"    Output: {r['output']}")

    print("\n" + "=" * 60)
    print("TASK 1 COMPLETE")
    print("=" * 60)

    return True


if __name__ == '__main__':
    test_synthetic_coloring()
