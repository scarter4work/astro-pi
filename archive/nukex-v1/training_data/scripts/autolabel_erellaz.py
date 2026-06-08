#!/usr/bin/env python3
"""
Auto-label Erellaz composites and append to existing RGB manifest.
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

sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet

Image.MAX_IMAGE_PIXELS = None

NUM_CLASSES = 21
BACKGROUND = 0
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
    img_array = np.array(image).astype(np.float32) / 255.0
    h, w = img_array.shape[:2]

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


def correct_nebula_classification(image, mask):
    img_array = np.array(image).astype(np.float32) / 255.0
    corrected_mask = mask.copy()

    red = img_array[:, :, 0]
    blue = img_array[:, :, 2]
    green = img_array[:, :, 1]

    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_mono = channel_std < 0.01

    if is_mono:
        return corrected_mask

    nebula_mask = (mask == NEBULA_EMISSION) | (mask == NEBULA_REFLECTION)
    blue_dominant = (blue > red * 1.2) & nebula_mask
    red_dominant = (red > blue * 1.2) & nebula_mask

    corrected_mask[blue_dominant] = NEBULA_REFLECTION
    corrected_mask[red_dominant] = NEBULA_EMISSION

    return corrected_mask


def main():
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    model_path = '/home/scarter4work/projects/NukeX/training_data/models_v5/best_model.pth'
    print(f"Loading model from {model_path}...")
    model = load_model(model_path, device)

    source_dir = '/home/scarter4work/projects/NukeX/training_data/rgb_sources/Erellaz_composites'
    output_dir = '/home/scarter4work/projects/NukeX/training_data/labeled_rgb/Erellaz'
    manifest_path = '/home/scarter4work/projects/NukeX/training_data/labeled_rgb/rgb_manifest.json'

    os.makedirs(output_dir, exist_ok=True)

    # Load existing manifest
    with open(manifest_path) as f:
        manifest = json.load(f)

    existing_count = len(manifest['pairs'])
    print(f"Existing pairs in manifest: {existing_count}")

    # Find images - only process the combined ones, not the part files
    image_files = [f for f in glob.glob(os.path.join(source_dir, "*.png"))
                   if '_p1_' not in f and '_p2_' not in f]

    print(f"\n=== Processing {len(image_files)} Erellaz composites ===")
    new_pairs = []

    for img_path in tqdm(image_files, desc="Processing Erellaz"):
        try:
            img = Image.open(img_path).convert('RGB')

            # Resize large images
            max_size = 2048
            if max(img.size) > max_size:
                ratio = max_size / max(img.size)
                new_size = (int(img.size[0] * ratio), int(img.size[1] * ratio))
                img_resized = img.resize(new_size, Image.BILINEAR)
            else:
                img_resized = img

            mask = segment_image(model, img_resized, device)
            mask = correct_nebula_classification(img_resized, mask)

            base_name = Path(img_path).stem
            img_out = os.path.join(output_dir, f"{base_name}_img.png")
            mask_out = os.path.join(output_dir, f"{base_name}_mask.png")

            img_resized.save(img_out)
            Image.fromarray(mask.astype(np.uint8)).save(mask_out)

            new_pairs.append({
                'image': img_out,
                'mask': mask_out
            })

        except Exception as e:
            print(f"Error processing {img_path}: {e}")

    # Add to manifest
    manifest['pairs'].extend(new_pairs)

    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)

    print(f"\n=== Complete ===")
    print(f"New pairs added: {len(new_pairs)}")
    print(f"Total pairs: {len(manifest['pairs'])}")


if __name__ == "__main__":
    main()
