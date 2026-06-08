#!/usr/bin/env python3
"""
Auto-label RGB images for training using:
1. Existing model for structural classification (stars, background, etc.)
2. Color-based heuristics to correct nebula type (emission vs reflection)

This creates training data that teaches the model to use the B-R color channel
to distinguish emission (red) from reflection (blue) nebulae.
"""

import os
import sys
import json
import numpy as np
import torch
from PIL import Image
from pathlib import Path
from tqdm import tqdm
import glob

# Add model path
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

# Class indices
BACKGROUND = 0
NEBULA_EMISSION = 5
NEBULA_REFLECTION = 6
NEBULA_DARK = 7
NEBULA_PLANETARY = 8


def load_model(model_path, device):
    """Load the trained segmentation model."""
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
    """Segment an image using the model with tiling for large images."""
    img_array = np.array(image).astype(np.float32) / 255.0
    h, w = img_array.shape[:2]

    # Add color contrast channel
    blue = img_array[:, :, 2]
    red = img_array[:, :, 0]
    green = img_array[:, :, 1]

    # Detect if monochrome
    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_mono = channel_std < 0.01

    if is_mono:
        color_contrast = np.zeros_like(blue)
    else:
        color_contrast = blue - red

    img_4ch = np.concatenate([img_array, color_contrast[:, :, np.newaxis]], axis=2)

    # Create output mask
    output_mask = np.zeros((h, w), dtype=np.uint8)

    # Process in tiles
    stride = tile_size // 2  # 50% overlap

    for y in range(0, h, stride):
        for x in range(0, w, stride):
            # Extract tile
            y_end = min(y + tile_size, h)
            x_end = min(x + tile_size, w)
            y_start = max(0, y_end - tile_size)
            x_start = max(0, x_end - tile_size)

            tile = img_4ch[y_start:y_end, x_start:x_end]

            # Pad if needed
            pad_h = tile_size - tile.shape[0]
            pad_w = tile_size - tile.shape[1]
            if pad_h > 0 or pad_w > 0:
                tile = np.pad(tile, ((0, pad_h), (0, pad_w), (0, 0)), mode='reflect')

            # Convert to tensor
            tile_tensor = torch.from_numpy(tile).permute(2, 0, 1).unsqueeze(0).float().to(device)

            # Predict
            with torch.no_grad():
                pred = model(tile_tensor)
                pred_mask = pred.argmax(dim=1).squeeze().cpu().numpy()

            # Remove padding
            if pad_h > 0:
                pred_mask = pred_mask[:-pad_h]
            if pad_w > 0:
                pred_mask = pred_mask[:, :-pad_w]

            # Copy to output (center region only to avoid edge artifacts)
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


def correct_nebula_classification(image, mask):
    """
    Use color information to correct emission/reflection classification.

    For pixels classified as any nebula type:
    - If BLUE dominant (B > R * 1.2): reclassify as REFLECTION
    - If RED dominant (R > B * 1.2): reclassify as EMISSION
    """
    img_array = np.array(image).astype(np.float32) / 255.0
    corrected_mask = mask.copy()

    red = img_array[:, :, 0]
    blue = img_array[:, :, 2]

    # Check if image is actually color (not monochrome)
    green = img_array[:, :, 1]
    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_mono = channel_std < 0.01

    if is_mono:
        # For monochrome images, don't correct - keep model's prediction
        return corrected_mask

    # Find nebula pixels (emission, reflection, or any nebula-like)
    nebula_mask = (mask == NEBULA_EMISSION) | (mask == NEBULA_REFLECTION)

    # Also include pixels that might be misclassified as other things
    # but have nebula-like characteristics (moderate brightness, not point sources)

    # Calculate color dominance
    blue_dominant = (blue > red * 1.2) & nebula_mask
    red_dominant = (red > blue * 1.2) & nebula_mask

    # Apply corrections
    corrected_mask[blue_dominant] = NEBULA_REFLECTION
    corrected_mask[red_dominant] = NEBULA_EMISSION

    return corrected_mask


def process_images(image_dir, output_dir, model, device, pattern="*.png"):
    """Process all images in a directory."""
    os.makedirs(output_dir, exist_ok=True)

    # Find all images
    image_files = glob.glob(os.path.join(image_dir, pattern))
    image_files.extend(glob.glob(os.path.join(image_dir, "*.jpg")))
    image_files.extend(glob.glob(os.path.join(image_dir, "*.jpeg")))
    image_files.extend(glob.glob(os.path.join(image_dir, "*.tif")))
    image_files.extend(glob.glob(os.path.join(image_dir, "*.tiff")))

    pairs = []

    for img_path in tqdm(image_files, desc=f"Processing {os.path.basename(image_dir)}"):
        try:
            # Load image
            img = Image.open(img_path).convert('RGB')

            # Skip very small images
            if img.size[0] < 256 or img.size[1] < 256:
                continue

            # Resize large images for processing
            max_size = 2048
            if max(img.size) > max_size:
                ratio = max_size / max(img.size)
                new_size = (int(img.size[0] * ratio), int(img.size[1] * ratio))
                img_resized = img.resize(new_size, Image.BILINEAR)
            else:
                img_resized = img

            # Generate segmentation mask
            mask = segment_image(model, img_resized, device)

            # Correct nebula classification using color
            mask = correct_nebula_classification(img_resized, mask)

            # Save image and mask
            base_name = Path(img_path).stem
            img_out = os.path.join(output_dir, f"{base_name}_img.png")
            mask_out = os.path.join(output_dir, f"{base_name}_mask.png")

            img_resized.save(img_out)
            Image.fromarray(mask.astype(np.uint8)).save(mask_out)

            pairs.append({
                'image': img_out,
                'mask': mask_out
            })

        except Exception as e:
            print(f"Error processing {img_path}: {e}")

    return pairs


def main():
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # Load model
    model_path = '/home/scarter4work/projects/NukeX/training_data/models_v5/best_model.pth'
    print(f"Loading model from {model_path}...")
    model = load_model(model_path, device)

    # RGB source directories to process
    rgb_sources = {
        'DeepSpaceYolo': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/DeepSpaceYolo/DeepSpaceYoloDataset/images',
        'ESA_Hubble': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/ESA_Hubble_images',
        'M78_composites': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/M78_composites',
        'QNAP_composites': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/QNAP_composites',
        'NOIRLab': '/home/scarter4work/projects/NukeX/training_data/rgb_sources/NOIRLab',
    }

    output_base = '/home/scarter4work/projects/NukeX/training_data/labeled_rgb'
    os.makedirs(output_base, exist_ok=True)

    all_pairs = []

    for name, source_dir in rgb_sources.items():
        if not os.path.exists(source_dir):
            print(f"Skipping {name}: directory not found")
            continue

        output_dir = os.path.join(output_base, name)
        print(f"\n=== Processing {name} ===")

        pairs = process_images(source_dir, output_dir, model, device)
        all_pairs.extend(pairs)
        print(f"  Generated {len(pairs)} training pairs")

    # Save manifest
    manifest_path = os.path.join(output_base, 'rgb_manifest.json')
    with open(manifest_path, 'w') as f:
        json.dump({'pairs': all_pairs}, f, indent=2)

    print(f"\n=== Complete ===")
    print(f"Total training pairs: {len(all_pairs)}")
    print(f"Manifest saved to: {manifest_path}")


if __name__ == "__main__":
    main()
