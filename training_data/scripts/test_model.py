#!/usr/bin/env python3
"""
Test the trained 21-class segmentation model on an image.
"""

import argparse
import numpy as np
import torch
from PIL import Image
from pathlib import Path
import sys

# Add model path
sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet

# Import unified palette
from segmentation_palette import (
    NUM_CLASSES,
    CLASS_NAMES,
    CLASS_COLORS_RGB as CLASS_COLORS,
    CLASS_DISPLAY_NAMES,
    apply_colormap,
)


def load_model(model_path, device, in_channels=4):
    """Load trained model."""
    model = AstroUNet(in_channels=in_channels, num_classes=NUM_CLASSES, base_features=32)
    checkpoint = torch.load(model_path, map_location=device)
    model.load_state_dict(checkpoint['model_state_dict'])
    model = model.to(device)
    model.eval()
    return model


def preprocess_image(image_path, size=512):
    """Load and preprocess image with color contrast channel."""
    img = Image.open(image_path).convert('RGB')
    original_size = img.size
    img = img.resize((size, size), Image.BILINEAR)
    img_np = np.array(img).astype(np.float32) / 255.0

    # Add color contrast channel: (Blue - Red)
    blue = img_np[:, :, 2]
    red = img_np[:, :, 0]
    green = img_np[:, :, 1]

    # Detect narrowband/monochrome images where all channels are nearly identical
    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_narrowband = channel_std < 0.01

    if is_narrowband:
        color_contrast = np.zeros_like(blue)[:, :, np.newaxis]
    else:
        color_contrast = (blue - red)[:, :, np.newaxis]

    # Stack as 4 channels: RGB + color_contrast
    img_4ch = np.concatenate([img_np, color_contrast], axis=2)

    img_tensor = torch.from_numpy(img_4ch).permute(2, 0, 1).unsqueeze(0)
    return img_tensor, original_size


def create_visualization(mask, original_img=None):
    """Create colored visualization of segmentation mask."""
    # Use unified palette colormap
    vis = apply_colormap(mask)

    if original_img is not None:
        # Blend with original
        h, w = mask.shape
        original_np = np.array(original_img.resize((w, h)))
        vis = (0.5 * vis + 0.5 * original_np).astype(np.uint8)

    return vis


def print_class_stats(mask):
    """Print statistics about detected classes."""
    unique, counts = np.unique(mask, return_counts=True)
    total = mask.size

    print("\nDetected classes:")
    print("-" * 50)
    for class_idx, count in sorted(zip(unique, counts), key=lambda x: -x[1]):
        pct = 100 * count / total
        if pct > 0.1:  # Only show classes with >0.1%
            print(f"  {CLASS_NAMES[class_idx]:25s}: {pct:6.2f}% ({count:,} pixels)")


def main():
    parser = argparse.ArgumentParser(description='Test segmentation model')
    parser.add_argument('image', help='Input image path')
    parser.add_argument('--model', default='/home/scarter4work/projects/NukeX/training_data/models/best_model.pth',
                        help='Model path')
    parser.add_argument('--output', help='Output path (default: input_segmented.png)')
    parser.add_argument('--size', type=int, default=512, help='Processing size')
    parser.add_argument('--blend', action='store_true', help='Blend with original image')
    args = parser.parse_args()

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Device: {device}")

    # Load model
    print(f"Loading model: {args.model}")
    model = load_model(args.model, device)
    print("Model loaded")

    # Load and preprocess image
    print(f"Processing: {args.image}")
    img_tensor, original_size = preprocess_image(args.image, args.size)
    img_tensor = img_tensor.to(device)

    # Run inference
    with torch.no_grad():
        output = model(img_tensor)
        pred = output.argmax(dim=1).squeeze().cpu().numpy()

    # Print class statistics
    print_class_stats(pred)

    # Create visualization
    original_img = Image.open(args.image).convert('RGB') if args.blend else None
    vis = create_visualization(pred, original_img)

    # Save output
    if args.output:
        output_path = args.output
    else:
        input_path = Path(args.image)
        output_path = input_path.parent / f"{input_path.stem}_segmented.png"

    # Also save the raw mask
    mask_path = str(output_path).replace('_segmented.png', '_mask.png')

    Image.fromarray(vis).save(output_path)
    Image.fromarray(pred.astype(np.uint8)).save(mask_path)

    print(f"\nSaved visualization: {output_path}")
    print(f"Saved mask: {mask_path}")


if __name__ == '__main__':
    main()
