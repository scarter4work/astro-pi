#!/usr/bin/env python3
"""
Test v6 model on RGB images to verify emission/reflection nebula discrimination.
"""

import os
import sys
import numpy as np
import torch
from PIL import Image
from pathlib import Path

sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet

Image.MAX_IMAGE_PIXELS = None

NUM_CLASSES = 21
CLASS_NAMES = [
    'background', 'star_bright', 'star_medium', 'star_faint', 'star_saturated',
    'nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary',
    'galaxy_spiral', 'galaxy_elliptical', 'galaxy_irregular', 'galaxy_core',
    'dust_lane', 'star_cluster_open', 'star_cluster_globular',
    'artifact_hot_pixel', 'artifact_satellite', 'artifact_diffraction',
    'artifact_gradient', 'artifact_noise'
]

# Nebula class indices
NEBULA_EMISSION = 5
NEBULA_REFLECTION = 6


def load_model(model_path, device):
    model = AstroUNet(in_channels=4, num_classes=NUM_CLASSES)
    checkpoint = torch.load(model_path, map_location=device, weights_only=True)
    if 'model_state_dict' in checkpoint:
        model.load_state_dict(checkpoint['model_state_dict'])
    else:
        model.load_state_dict(checkpoint)
    model = model.to(device)
    model.eval()
    return model


def segment_image(model, image, device, tile_size=512):
    """Segment an image using the model."""
    img_array = np.array(image).astype(np.float32) / 255.0
    h, w = img_array.shape[:2]

    # Add color contrast channel
    blue = img_array[:, :, 2]
    red = img_array[:, :, 0]
    green = img_array[:, :, 1]

    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_mono = channel_std < 0.01

    if is_mono:
        color_contrast = np.zeros_like(blue)
    else:
        color_contrast = blue - red

    img_4ch = np.concatenate([img_array, color_contrast[:, :, np.newaxis]], axis=2)
    output_mask = np.zeros((h, w), dtype=np.uint8)

    stride = tile_size // 2

    for y in range(0, h, stride):
        for x in range(0, w, stride):
            y_end = min(y + tile_size, h)
            x_end = min(x + tile_size, w)
            y_start = max(0, y_end - tile_size)
            x_start = max(0, x_end - tile_size)

            tile = img_4ch[y_start:y_end, x_start:x_end]

            pad_h = tile_size - tile.shape[0]
            pad_w = tile_size - tile.shape[1]
            if pad_h > 0 or pad_w > 0:
                tile = np.pad(tile, ((0, pad_h), (0, pad_w), (0, 0)), mode='reflect')

            tile_tensor = torch.from_numpy(tile).permute(2, 0, 1).unsqueeze(0).float().to(device)

            with torch.no_grad():
                pred = model(tile_tensor)
                pred_mask = pred.argmax(dim=1).squeeze().cpu().numpy()

            if pad_h > 0:
                pred_mask = pred_mask[:-pad_h]
            if pad_w > 0:
                pred_mask = pred_mask[:, :-pad_w]

            margin = stride // 4
            copy_y_start = margin if y > 0 else 0
            copy_y_end = pred_mask.shape[0] - margin if y_end < h else pred_mask.shape[0]
            copy_x_start = margin if x > 0 else 0
            copy_x_end = pred_mask.shape[1] - margin if x_end < w else pred_mask.shape[1]

            out_y_start = y_start + copy_y_start
            out_y_end = y_start + copy_y_end
            out_x_start = x_start + copy_x_start
            out_x_end = x_start + copy_x_end

            output_mask[out_y_start:out_y_end, out_x_start:out_x_end] = \
                pred_mask[copy_y_start:copy_y_end, copy_x_start:copy_x_end]

    return output_mask


def analyze_image(image_path, model, device):
    """Analyze a single image and report nebula classification."""
    print(f"\n{'='*60}")
    print(f"Image: {os.path.basename(image_path)}")
    print(f"{'='*60}")

    img = Image.open(image_path).convert('RGB')

    # Resize if too large
    max_size = 2048
    if max(img.size) > max_size:
        ratio = max_size / max(img.size)
        new_size = (int(img.size[0] * ratio), int(img.size[1] * ratio))
        img = img.resize(new_size, Image.BILINEAR)

    print(f"Size: {img.size[0]}x{img.size[1]}")

    # Check color characteristics
    img_array = np.array(img).astype(np.float32) / 255.0
    red = img_array[:, :, 0]
    green = img_array[:, :, 1]
    blue = img_array[:, :, 2]

    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_mono = channel_std < 0.01

    print(f"Color analysis:")
    print(f"  R mean: {red.mean():.3f}, G mean: {green.mean():.3f}, B mean: {blue.mean():.3f}")
    print(f"  Channel std: {channel_std:.4f} ({'MONOCHROME' if is_mono else 'COLOR'})")

    if not is_mono:
        # Check which color dominates in bright regions
        brightness = (red + green + blue) / 3
        bright_mask = brightness > 0.3
        if bright_mask.sum() > 0:
            red_bright = red[bright_mask].mean()
            blue_bright = blue[bright_mask].mean()
            print(f"  Bright regions: R={red_bright:.3f}, B={blue_bright:.3f}")
            if red_bright > blue_bright * 1.1:
                print(f"  -> RED dominant (likely emission nebula)")
            elif blue_bright > red_bright * 1.1:
                print(f"  -> BLUE dominant (likely reflection nebula)")
            else:
                print(f"  -> Neutral/mixed colors")

    # Segment
    mask = segment_image(model, img, device)

    # Count pixels per class
    print(f"\nSegmentation results:")
    total_pixels = mask.size

    class_counts = {}
    for c in range(NUM_CLASSES):
        count = np.sum(mask == c)
        if count > 0:
            pct = 100 * count / total_pixels
            class_counts[CLASS_NAMES[c]] = (count, pct)

    # Sort by count
    for name, (count, pct) in sorted(class_counts.items(), key=lambda x: -x[1][0]):
        if pct > 0.1:  # Only show classes with >0.1%
            print(f"  {name}: {pct:.2f}%")

    # Specific nebula analysis
    emission_pct = class_counts.get('nebula_emission', (0, 0))[1]
    reflection_pct = class_counts.get('nebula_reflection', (0, 0))[1]

    print(f"\nNebula classification:")
    print(f"  Emission nebula: {emission_pct:.2f}%")
    print(f"  Reflection nebula: {reflection_pct:.2f}%")

    if emission_pct > 1 or reflection_pct > 1:
        if emission_pct > reflection_pct * 2:
            print(f"  -> Model classifies as EMISSION dominant")
        elif reflection_pct > emission_pct * 2:
            print(f"  -> Model classifies as REFLECTION dominant")
        else:
            print(f"  -> Model sees MIXED emission/reflection")

    return mask


def main():
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # Load v6 model
    model_path = '/home/scarter4work/projects/NukeX/training_data/models_v6/best_model.pth'
    print(f"Loading model: {model_path}")
    model = load_model(model_path, device)

    # Test images - a mix of known emission and reflection nebulae
    test_images = []

    # Erellaz composites - emission nebulae
    erellaz_dir = '/home/scarter4work/projects/NukeX/training_data/rgb_sources/Erellaz_composites'
    if os.path.exists(erellaz_dir):
        for f in os.listdir(erellaz_dir):
            if f.endswith('.png') and '_p1_' not in f and '_p2_' not in f:
                test_images.append((os.path.join(erellaz_dir, f), "emission"))

    # NOIRLab - reflection nebulae
    noirlab_dir = '/home/scarter4work/projects/NukeX/training_data/rgb_sources/NOIRLab'
    if os.path.exists(noirlab_dir):
        for f in os.listdir(noirlab_dir):
            if f.endswith('.tif') or f.endswith('.tiff'):
                test_images.append((os.path.join(noirlab_dir, f), "reflection"))

    # M78 composites - reflection nebula
    m78_dir = '/home/scarter4work/projects/NukeX/training_data/rgb_sources/M78_composites'
    if os.path.exists(m78_dir):
        # Just test a few
        m78_files = [f for f in os.listdir(m78_dir) if f.endswith('.png')][:3]
        for f in m78_files:
            test_images.append((os.path.join(m78_dir, f), "reflection"))

    # QNAP composites - various
    qnap_dir = '/home/scarter4work/projects/NukeX/training_data/rgb_sources/QNAP_composites'
    if os.path.exists(qnap_dir):
        for f in os.listdir(qnap_dir):
            if f.endswith('.png'):
                # Try to determine type from filename
                fname = f.lower()
                if 'm78' in fname or 'm45' in fname or 'pleiades' in fname:
                    test_images.append((os.path.join(qnap_dir, f), "reflection"))
                elif 'm42' in fname or 'orion' in fname or 'm8' in fname or 'm20' in fname:
                    test_images.append((os.path.join(qnap_dir, f), "emission"))
                else:
                    test_images.append((os.path.join(qnap_dir, f), "unknown"))

    print(f"\nTesting {len(test_images)} images...")

    results = {'correct': 0, 'incorrect': 0, 'ambiguous': 0}

    for image_path, expected_type in test_images:
        mask = analyze_image(image_path, model, device)

        # Check if classification matches expected
        total = mask.size
        emission_pct = 100 * np.sum(mask == NEBULA_EMISSION) / total
        reflection_pct = 100 * np.sum(mask == NEBULA_REFLECTION) / total

        if emission_pct < 1 and reflection_pct < 1:
            detected = "none"
        elif emission_pct > reflection_pct * 2:
            detected = "emission"
        elif reflection_pct > emission_pct * 2:
            detected = "reflection"
        else:
            detected = "mixed"

        if expected_type == "unknown":
            results['ambiguous'] += 1
        elif detected == expected_type:
            results['correct'] += 1
            print(f"  ✓ CORRECT: Expected {expected_type}, detected {detected}")
        elif detected == "none" or detected == "mixed":
            results['ambiguous'] += 1
            print(f"  ? AMBIGUOUS: Expected {expected_type}, detected {detected}")
        else:
            results['incorrect'] += 1
            print(f"  ✗ INCORRECT: Expected {expected_type}, detected {detected}")

    print(f"\n{'='*60}")
    print(f"SUMMARY")
    print(f"{'='*60}")
    print(f"Correct: {results['correct']}")
    print(f"Incorrect: {results['incorrect']}")
    print(f"Ambiguous: {results['ambiguous']}")

    total_testable = results['correct'] + results['incorrect']
    if total_testable > 0:
        accuracy = 100 * results['correct'] / total_testable
        print(f"Accuracy (excluding ambiguous): {accuracy:.1f}%")


if __name__ == "__main__":
    main()
