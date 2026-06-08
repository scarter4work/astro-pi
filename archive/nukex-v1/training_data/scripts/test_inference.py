#!/usr/bin/env python3
"""
Simple inference script to test the model on a sample image.
Generates side-by-side visualization: original + colorized segmentation mask.
"""

import torch
import torch.nn as nn
from torchvision import transforms
from PIL import Image
import numpy as np
import sys
from pathlib import Path

# Class color mapping (21 classes)
CLASS_COLORS = {
    0: (0, 0, 0),           # background
    1: (255, 0, 0),         # bright_emission
    2: (255, 100, 0),       # faint_emission
    3: (0, 0, 255),         # dark_nebula
    4: (100, 100, 255),     # reflection_nebula
    5: (255, 255, 0),       # galaxy_core
    6: (255, 200, 0),       # galaxy_arm
    7: (200, 0, 0),         # dust_lane
    8: (255, 0, 255),       # star_core
    9: (255, 150, 200),     # star_halo
    10: (0, 255, 0),        # transition_bright_faint
    11: (100, 255, 100),    # transition_emission_dark
    12: (0, 255, 255),      # transition_star_nebula
    13: (150, 75, 0),       # stellar_artifact
    14: (200, 200, 200),    # noise_spike
    15: (100, 100, 100),    # gradient_transition
    16: (255, 100, 100),    # nebula_boundary
    17: (100, 255, 255),    # halo_transition
    18: (200, 100, 200),    # color_fringing
    19: (150, 150, 0),      # saturation_region
    20: (50, 200, 50),      # low_confidence
}

class UNet(nn.Module):
    """UNet segmentation model matching the checkpoint dimensions."""
    def __init__(self, in_channels=4, num_classes=21):
        super().__init__()
        self.inc = DoubleConv(in_channels, 32)
        self.down1 = Down(32, 64)
        self.down2 = Down(64, 128)
        self.down3 = Down(128, 256)
        self.down4 = Down(256, 256)
        self.up1 = Up(512, 128)
        self.up2 = Up(256, 64)
        self.up3 = Up(128, 32)
        self.up4 = Up(64, 32)
        self.outc = OutConv(32, num_classes)

    def forward(self, x):
        x1 = self.inc(x)
        x2 = self.down1(x1)
        x3 = self.down2(x2)
        x4 = self.down3(x3)
        x5 = self.down4(x4)
        x = self.up1(x5, x4)
        x = self.up2(x, x3)
        x = self.up3(x, x2)
        x = self.up4(x, x1)
        logits = self.outc(x)
        return logits

class DoubleConv(nn.Module):
    def __init__(self, in_channels, out_channels):
        super().__init__()
        self.conv = nn.Sequential(
            nn.Conv2d(in_channels, out_channels, 3, padding=1, bias=False),
            nn.BatchNorm2d(out_channels),
            nn.ReLU(inplace=True),
            nn.Conv2d(out_channels, out_channels, 3, padding=1, bias=False),
            nn.BatchNorm2d(out_channels),
            nn.ReLU(inplace=True),
        )

    def forward(self, x):
        return self.conv(x)

class Down(nn.Module):
    def __init__(self, in_channels, out_channels):
        super().__init__()
        self.pool_conv = nn.Sequential(
            nn.MaxPool2d(2),
            DoubleConv(in_channels, out_channels),
        )

    def forward(self, x):
        return self.pool_conv(x)

class Up(nn.Module):
    def __init__(self, in_channels, out_channels):
        super().__init__()
        self.up = nn.Upsample(scale_factor=2, mode='bilinear', align_corners=False)
        self.conv = DoubleConv(in_channels, out_channels)

    def forward(self, x1, x2):
        x1 = self.up(x1)
        x = torch.cat([x2, x1], dim=1)
        return self.conv(x)

class OutConv(nn.Module):
    def __init__(self, in_channels, out_channels):
        super().__init__()
        self.weight = nn.Parameter(torch.zeros(out_channels, in_channels, 1, 1))
        self.bias = nn.Parameter(torch.zeros(out_channels))

    def forward(self, x):
        return torch.nn.functional.conv2d(x, self.weight, self.bias, padding=0)

def colorize_mask(mask, class_colors):
    """Convert class indices to RGB image."""
    h, w = mask.shape
    colored = np.zeros((h, w, 3), dtype=np.uint8)

    for class_id, color in class_colors.items():
        colored[mask == class_id] = color

    return colored

def run_inference(image_path, model_path, output_path):
    """Run inference and save visualization."""

    # Device
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    # Load image
    print(f"Loading image: {image_path}")
    img = Image.open(image_path).convert('RGB')
    original_size = img.size
    print(f"Original size: {original_size}")

    # Resize to 512x512 for model
    img_resized = img.resize((512, 512), Image.BILINEAR)

    # Preprocess - create 4-channel input (RGB + normalized mean)
    transform = transforms.Compose([
        transforms.ToTensor(),
        transforms.Normalize(mean=[0.485, 0.456, 0.406],
                           std=[0.229, 0.224, 0.225])
    ])
    img_tensor = transform(img_resized)  # 3 channels

    # Add 4th channel (normalized mean intensity)
    mean_channel = img_tensor.mean(dim=0, keepdim=True)
    img_tensor = torch.cat([img_tensor, mean_channel], dim=0)  # Now 4 channels
    img_tensor = img_tensor.unsqueeze(0).to(device)

    print(f"Input shape: {img_tensor.shape}")

    # Load model
    print(f"Loading model: {model_path}")
    model = UNet(in_channels=4, num_classes=21)
    checkpoint = torch.load(model_path, map_location=device)

    # Handle checkpoint format
    if isinstance(checkpoint, dict) and 'model_state_dict' in checkpoint:
        model.load_state_dict(checkpoint['model_state_dict'])
    elif isinstance(checkpoint, dict) and 'state_dict' in checkpoint:
        model.load_state_dict(checkpoint['state_dict'])
    else:
        model.load_state_dict(checkpoint)

    model = model.to(device)
    model.eval()

    # Inference
    print("Running inference...")
    with torch.no_grad():
        output = model(img_tensor)

    # Get class predictions
    pred = torch.argmax(output, dim=1)[0].cpu().numpy()
    print(f"Prediction shape: {pred.shape}")
    print(f"Classes found: {np.unique(pred)}")

    # Resize back to original
    pred_resized = Image.fromarray(pred.astype(np.uint8)).resize(original_size, Image.NEAREST)
    mask = np.array(pred_resized)

    # Colorize
    colored_mask = colorize_mask(mask, CLASS_COLORS)
    mask_img = Image.fromarray(colored_mask)

    # Create side-by-side visualization
    total_width = original_size[0] * 2
    total_height = original_size[1]
    viz = Image.new('RGB', (total_width, total_height))
    viz.paste(img, (0, 0))
    viz.paste(mask_img, (original_size[0], 0))

    # Save
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    viz.save(output_path)
    print(f"Saved visualization to: {output_path}")
    print(f"Visualization size: {viz.size}")

if __name__ == '__main__':
    image_path = '/home/scarter4work/projects/NukeX/training_data/labeled/bright_emission/M42/mastDownload/HST/j93k07hvq/j93k07hvq_drc_img.png'
    model_path = '/home/scarter4work/projects/NukeX/training_data/models_v8_test/best_model.pth'
    output_path = '/home/scarter4work/projects/NukeX/training_data/sample_results/test_visualization.png'

    run_inference(image_path, model_path, output_path)
