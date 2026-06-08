#!/usr/bin/env python3
"""
Train 21-class segmentation model using the prepared training manifest.

Training data is pre-stretched (arcsinh) images with segmentation masks.
At inference time: stretch image -> segment -> apply mask to linear data.

Usage:
    python train_21class.py --batch-size 64 --epochs 100

Enhanced RGB Support (v7):
    # Curriculum learning for RGB domain adaptation:
    python train_21class.py --curriculum-phase 1  # 80% RGB focus
    python train_21class.py --curriculum-phase 2  # 50/50 balanced
    python train_21class.py --curriculum-phase 3  # All data fine-tuning

    # Automatic curriculum (switch phases during training):
    python train_21class.py --auto-curriculum --epochs 150

    # Enhanced augmentation (enabled by default for v7):
    python train_21class.py --enhanced-augment  # RGB-specific augmentation
    python train_21class.py --no-enhanced-augment  # Disable for backward compat
"""

import argparse
import json
import logging
import random
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader, random_split
from torch.amp import autocast, GradScaler
from PIL import Image
from pathlib import Path
from datetime import datetime
from tqdm import tqdm
import colorsys
import sys

# Synthetic color augmentation for mono->RGB conversion
from synthetic_color import apply_color_augmentation, detect_mono_image

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Add NukeX2 scripts to path for model
sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet, count_parameters

# GPU optimizations
torch.backends.cudnn.benchmark = True
torch.backends.cuda.matmul.allow_tf32 = True
torch.backends.cudnn.allow_tf32 = True

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

# Class indices for color-sensitive classes (used in validation metrics)
CLASS_EMISSION_NEBULA = 5   # nebula_emission
CLASS_REFLECTION_NEBULA = 6  # nebula_reflection


# =============================================================================
# ENHANCED AUGMENTATION FUNCTIONS (v7 RGB support)
# =============================================================================

def normalize_image_percentile(img):
    """
    Percentile-based normalization for consistent preprocessing.
    Uses 1st and 99th percentile to handle outliers and varying dynamic ranges.

    Args:
        img: numpy array of shape (H, W, 3) with values in any range
    Returns:
        Normalized image with values in [0, 1]
    """
    img = img.copy().astype(np.float32)
    for c in range(3):
        p1, p99 = np.percentile(img[:, :, c], [1, 99])
        if p99 > p1:
            img[:, :, c] = np.clip((img[:, :, c] - p1) / (p99 - p1), 0, 1)
        else:
            # Handle constant channel (shouldn't happen in normal images)
            img[:, :, c] = np.clip(img[:, :, c], 0, 1)
    return img


def apply_color_balance_shift(img):
    """
    Simulate different white balance / color calibration.
    Randomly scales RGB channels to simulate different camera responses
    and processing pipelines.

    Args:
        img: numpy array (H, W, 3) with values in [0, 1]
    Returns:
        Augmented image
    """
    # R and B channels vary more (0.8-1.2), G is more stable (0.9-1.1)
    r_scale = random.uniform(0.8, 1.2)
    g_scale = random.uniform(0.9, 1.1)
    b_scale = random.uniform(0.8, 1.2)

    img = img.copy()
    img[:, :, 0] = np.clip(img[:, :, 0] * r_scale, 0, 1)
    img[:, :, 1] = np.clip(img[:, :, 1] * g_scale, 0, 1)
    img[:, :, 2] = np.clip(img[:, :, 2] * b_scale, 0, 1)
    return img


def apply_contrast_augmentation(img):
    """
    Simulate different stretching/contrast adjustments.
    Models the effect of different stretch parameters in image processing.

    Args:
        img: numpy array (H, W, 3) with values in [0, 1]
    Returns:
        Augmented image
    """
    contrast_factor = random.uniform(0.7, 1.3)
    # Apply contrast around 0.5 midpoint
    img = (img - 0.5) * contrast_factor + 0.5
    return np.clip(img, 0, 1)


def apply_gamma_augmentation(img):
    """
    Simulate different transfer functions / gamma curves.
    Models different display gamma and processing transfer functions.

    Args:
        img: numpy array (H, W, 3) with values in [0, 1]
    Returns:
        Augmented image
    """
    gamma = random.uniform(0.7, 1.3)
    # Avoid numerical issues with zero values
    img = np.clip(img, 1e-8, 1.0)
    return np.power(img, gamma)


def apply_saturation_augmentation(img):
    """
    Vary color saturation to simulate different processing styles.
    Converts to HSV, scales saturation, converts back.

    Args:
        img: numpy array (H, W, 3) with values in [0, 1]
    Returns:
        Augmented image
    """
    saturation_scale = random.uniform(0.8, 1.2)

    # Vectorized HSV conversion for efficiency
    img = img.copy()

    # Get min/max for HSV calculation
    c_max = img.max(axis=2)
    c_min = img.min(axis=2)
    delta = c_max - c_min

    # Value (V) = max
    v = c_max

    # Saturation (S) = delta / max (avoid div by zero)
    s = np.where(c_max > 0, delta / (c_max + 1e-8), 0)

    # Scale saturation
    s_new = np.clip(s * saturation_scale, 0, 1)

    # Reconstruct: new_img = v - s_new * v + original_ratio * s_new * v
    # Simplified: scale the color deviation from grayscale
    gray = v[:, :, np.newaxis]
    color_diff = img - gray

    # Scale color difference by saturation ratio
    sat_ratio = np.where(s > 0, s_new / (s + 1e-8), 1.0)
    sat_ratio = sat_ratio[:, :, np.newaxis]

    img_new = gray + color_diff * sat_ratio
    return np.clip(img_new, 0, 1)


def apply_stretch_variation(img, scale_range=(0.05, 0.2)):
    """
    Apply arcsinh stretch with random scale parameter.
    This simulates different preprocessing pipelines.

    The standard arcsinh stretch is: arcsinh(x * scale) / arcsinh(scale)

    Args:
        img: numpy array (H, W, 3) with values in [0, 1]
        scale_range: tuple of (min_scale, max_scale) for random selection
    Returns:
        Stretched image with values in [0, 1]
    """
    scale = np.random.uniform(*scale_range)
    # Apply arcsinh with random scale
    stretched = np.arcsinh(img * scale) / np.arcsinh(scale)
    return np.clip(stretched, 0, 1).astype(np.float32)


def is_rgb_image(img):
    """
    Detect if image is RGB (color) vs monochrome/narrowband.

    Args:
        img: numpy array (H, W, 3) with values in [0, 1]
    Returns:
        True if image appears to be RGB color
    """
    # Check standard deviation between channel means
    channel_means = [img[:, :, c].mean() for c in range(3)]
    channel_std = np.std(channel_means)

    # Also check if any channel pair has significant difference
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    max_channel_diff = max(
        np.abs(r - g).mean(),
        np.abs(g - b).mean(),
        np.abs(r - b).mean()
    )

    # Image is RGB if channels are sufficiently different
    return channel_std > 0.01 or max_channel_diff > 0.02


class ManifestDataset(Dataset):
    """
    Dataset that loads from training_manifest.json.

    Supports enhanced RGB augmentation (v7) for better domain generalization
    across different image processing styles and color calibrations.
    """

    def __init__(self, pairs, image_size=512, augment=True, enhanced_augment=True,
                 use_percentile_norm=True, synthetic_color_prob=0.7, stretch_augment=True):
        """
        Args:
            pairs: List of {'image': path, 'mask': path} dicts
            image_size: Target size for images
            augment: Enable basic geometric augmentation
            enhanced_augment: Enable RGB-specific color augmentation (v7)
            use_percentile_norm: Use percentile-based normalization
            synthetic_color_prob: Probability of applying synthetic color to mono images (0-1)
            stretch_augment: Enable arcsinh stretch variation augmentation (v7)
        """
        self.pairs = pairs
        self.image_size = image_size
        self.augment = augment
        self.enhanced_augment = enhanced_augment
        self.use_percentile_norm = use_percentile_norm
        self.synthetic_color_prob = synthetic_color_prob
        self.stretch_augment = stretch_augment

        # Track synthetic color application stats (for logging)
        self._synthetic_color_applied = 0
        self._mono_images_seen = 0
        self._stretch_augment_applied = 0

    def get_synthetic_color_stats(self) -> dict:
        """Return statistics on synthetic color augmentation."""
        return {
            'mono_images_seen': self._mono_images_seen,
            'synthetic_color_applied': self._synthetic_color_applied,
            'application_rate': (
                self._synthetic_color_applied / max(self._mono_images_seen, 1)
            )
        }

    def get_stretch_augment_stats(self) -> dict:
        """Return statistics on stretch variation augmentation."""
        return {
            'stretch_augment_applied': self._stretch_augment_applied
        }

    def reset_synthetic_color_stats(self):
        """Reset synthetic color statistics (call at epoch start)."""
        self._synthetic_color_applied = 0
        self._mono_images_seen = 0

    def reset_stretch_augment_stats(self):
        """Reset stretch augmentation statistics (call at epoch start)."""
        self._stretch_augment_applied = 0

    def __len__(self):
        return len(self.pairs)

    def __getitem__(self, idx):
        pair = self.pairs[idx]

        # Load image and mask
        img = Image.open(pair['image']).convert('RGB')
        mask = Image.open(pair['mask'])

        # Resize
        img = img.resize((self.image_size, self.image_size), Image.BILINEAR)
        mask = mask.resize((self.image_size, self.image_size), Image.NEAREST)

        # Convert to numpy
        img = np.array(img).astype(np.float32)
        mask = np.array(mask).astype(np.int64)

        # Normalization: either percentile-based (v7) or simple /255
        if self.use_percentile_norm:
            # Percentile-based normalization handles varying dynamic ranges
            img = normalize_image_percentile(img)
        else:
            # Legacy normalization (backward compat)
            img = img / 255.0

        # Geometric Augmentation (applied to both RGB and mono)
        if self.augment:
            # Random horizontal flip
            if random.random() > 0.5:
                img = np.flip(img, axis=1).copy()
                mask = np.flip(mask, axis=1).copy()

            # Random vertical flip
            if random.random() > 0.5:
                img = np.flip(img, axis=0).copy()
                mask = np.flip(mask, axis=0).copy()

            # Random 90-degree rotation
            if random.random() > 0.5:
                k = random.randint(1, 3)
                img = np.rot90(img, k=k).copy()
                mask = np.rot90(mask, k=k).copy()

            # Basic brightness augmentation (legacy, always applied)
            if random.random() > 0.5:
                brightness = 0.8 + random.random() * 0.4
                img = np.clip(img * brightness, 0, 1)

        # Enhanced RGB Augmentation (v7) - only for color images during training
        if self.augment and self.enhanced_augment:
            # Check if this is an RGB image (not mono/narrowband)
            img_is_rgb = is_rgb_image(img)

            if img_is_rgb:
                # Color balance shift (simulate different white balance)
                # Apply 50% of the time to maintain some original distribution
                if random.random() > 0.5:
                    img = apply_color_balance_shift(img)

                # Contrast augmentation (simulate different stretching)
                if random.random() > 0.5:
                    img = apply_contrast_augmentation(img)

                # Gamma augmentation (simulate different transfer functions)
                if random.random() > 0.5:
                    img = apply_gamma_augmentation(img)

                # Saturation variation
                if random.random() > 0.5:
                    img = apply_saturation_augmentation(img)

        # Synthetic Color Augmentation for Monochrome Images (v7)
        # This converts mono images to synthetic RGB so the model learns color features
        # Must happen BEFORE color contrast channel computation
        if self.augment and self.synthetic_color_prob > 0:
            # Use the more accurate detect_mono_image from synthetic_color module
            is_mono = detect_mono_image(img)

            if is_mono:
                self._mono_images_seen += 1
                # Apply synthetic coloring with configured probability
                # The apply_color_augmentation function handles the probability internally
                img_before = img
                img = apply_color_augmentation(img, mask, p=self.synthetic_color_prob)

                # Check if color was actually applied (for logging)
                if not np.allclose(img, img_before):
                    self._synthetic_color_applied += 1
                    logger.debug(f"Applied synthetic color to mono image (idx={idx})")

        # Arcsinh Stretch Variation Augmentation (v7)
        # Simulates different preprocessing pipelines by applying arcsinh stretch
        # with random scale parameter. Applied AFTER synthetic color, BEFORE color contrast.
        if self.augment and self.stretch_augment:
            if random.random() < 0.3:  # Apply with 30% probability
                img = apply_stretch_variation(img, scale_range=(0.05, 0.2))
                self._stretch_augment_applied += 1
                logger.debug(f"Applied stretch variation to image (idx={idx})")

        # Add color contrast channel: (Blue - Red) normalized to [-1, 1]
        # This helps distinguish blue reflection nebulae from red emission nebulae
        blue = img[:, :, 2]  # B channel
        red = img[:, :, 0]   # R channel
        green = img[:, :, 1]  # G channel

        # Detect narrowband/monochrome images where all channels are nearly identical
        # For these, the B-R channel gives no useful info, so zero it out
        channel_std = np.std([red.mean(), green.mean(), blue.mean()])
        is_narrowband = channel_std < 0.01  # Channels within 1% of each other

        if is_narrowband:
            # For narrowband, use zero color contrast - let model use spatial features only
            color_contrast = np.zeros_like(blue)
        else:
            color_contrast = (blue - red)  # Range: [-1, 1]

        # Stack as 4th channel: RGB + color_contrast
        img_4ch = np.concatenate([img, color_contrast[:, :, np.newaxis]], axis=2)

        # Convert to tensor (C, H, W)
        img = torch.from_numpy(img_4ch).permute(2, 0, 1).float()
        mask = torch.from_numpy(mask).long()

        return img, mask


class DiceLoss(nn.Module):
    """Dice loss for segmentation."""
    def __init__(self, smooth=1.0):
        super().__init__()
        self.smooth = smooth

    def forward(self, pred, target):
        pred = torch.softmax(pred, dim=1)
        target_one_hot = torch.zeros_like(pred)
        target_one_hot.scatter_(1, target.unsqueeze(1), 1)

        intersection = (pred * target_one_hot).sum(dim=(2, 3))
        union = pred.sum(dim=(2, 3)) + target_one_hot.sum(dim=(2, 3))

        dice = (2 * intersection + self.smooth) / (union + self.smooth)
        return 1 - dice.mean()


class CombinedLoss(nn.Module):
    """Combined Cross-Entropy + Dice loss with class weights."""
    def __init__(self, class_weights=None, ce_weight=0.5, dice_weight=0.5):
        super().__init__()
        self.ce_weight = ce_weight
        self.dice_weight = dice_weight
        self.ce = nn.CrossEntropyLoss(weight=class_weights)
        self.dice = DiceLoss()

    def forward(self, pred, target):
        return self.ce_weight * self.ce(pred, target) + self.dice_weight * self.dice(pred, target)


def train_epoch(model, loader, criterion, optimizer, scaler, device):
    model.train()
    total_loss = 0

    pbar = tqdm(loader, desc="Training", leave=False)
    for images, masks in pbar:
        images = images.to(device, non_blocking=True)
        masks = masks.to(device, non_blocking=True)

        optimizer.zero_grad(set_to_none=True)

        with autocast('cuda'):
            outputs = model(images)
            loss = criterion(outputs, masks)

        scaler.scale(loss).backward()
        scaler.step(optimizer)
        scaler.update()

        total_loss += loss.item()
        pbar.set_postfix({'loss': f'{loss.item():.4f}'})

    return total_loss / len(loader)


def validate(model, loader, criterion, device, compute_color_metrics=True):
    """
    Validate model and compute metrics.

    Args:
        model: The model to validate
        loader: Validation DataLoader
        criterion: Loss function
        device: torch device
        compute_color_metrics: If True, compute emission/reflection confusion metrics

    Returns:
        val_loss, val_acc, class_acc, color_metrics (optional)
    """
    model.eval()
    total_loss = 0
    correct = 0
    total = 0

    # Per-class metrics
    class_correct = torch.zeros(NUM_CLASSES)
    class_total = torch.zeros(NUM_CLASSES)

    # For IoU calculation (intersection and union per class)
    class_intersection = torch.zeros(NUM_CLASSES)
    class_union = torch.zeros(NUM_CLASSES)

    # Color discrimination metrics: emission vs reflection confusion
    # Tracks when emission is predicted as reflection and vice versa
    emission_as_reflection = 0  # GT=emission, pred=reflection
    reflection_as_emission = 0  # GT=reflection, pred=emission
    emission_total = 0  # Total emission pixels
    reflection_total = 0  # Total reflection pixels

    with torch.no_grad():
        for images, masks in tqdm(loader, desc="Validating", leave=False):
            images = images.to(device, non_blocking=True)
            masks = masks.to(device, non_blocking=True)

            with autocast('cuda'):
                outputs = model(images)
                loss = criterion(outputs, masks)

            total_loss += loss.item()

            preds = outputs.argmax(dim=1)
            correct += (preds == masks).sum().item()
            total += masks.numel()

            # Per-class accuracy and IoU
            for c in range(NUM_CLASSES):
                class_mask = (masks == c)
                pred_mask = (preds == c)

                class_total[c] += class_mask.sum().item()
                class_correct[c] += ((preds == c) & class_mask).sum().item()

                # IoU: intersection / union
                intersection = (pred_mask & class_mask).sum().item()
                union = (pred_mask | class_mask).sum().item()
                class_intersection[c] += intersection
                class_union[c] += union

            # Color discrimination metrics (emission vs reflection confusion)
            if compute_color_metrics:
                # Emission nebula confusion
                emission_mask = (masks == CLASS_EMISSION_NEBULA)
                emission_total += emission_mask.sum().item()
                emission_as_reflection += ((preds == CLASS_REFLECTION_NEBULA) & emission_mask).sum().item()

                # Reflection nebula confusion
                reflection_mask = (masks == CLASS_REFLECTION_NEBULA)
                reflection_total += reflection_mask.sum().item()
                reflection_as_emission += ((preds == CLASS_EMISSION_NEBULA) & reflection_mask).sum().item()

    # Calculate per-class accuracy
    class_acc = {}
    for c in range(NUM_CLASSES):
        if class_total[c] > 0:
            class_acc[CLASS_NAMES[c]] = class_correct[c] / class_total[c]

    # Calculate per-class IoU
    class_iou = {}
    for c in range(NUM_CLASSES):
        if class_union[c] > 0:
            class_iou[CLASS_NAMES[c]] = class_intersection[c] / class_union[c]

    # Color discrimination metrics
    color_metrics = {}
    if compute_color_metrics:
        # Emission nebula IoU
        if class_union[CLASS_EMISSION_NEBULA] > 0:
            color_metrics['emission_iou'] = (
                class_intersection[CLASS_EMISSION_NEBULA] /
                class_union[CLASS_EMISSION_NEBULA]
            )
        else:
            color_metrics['emission_iou'] = 0.0

        # Reflection nebula IoU
        if class_union[CLASS_REFLECTION_NEBULA] > 0:
            color_metrics['reflection_iou'] = (
                class_intersection[CLASS_REFLECTION_NEBULA] /
                class_union[CLASS_REFLECTION_NEBULA]
            )
        else:
            color_metrics['reflection_iou'] = 0.0

        # Confusion rates
        color_metrics['emission_as_reflection_rate'] = (
            emission_as_reflection / max(emission_total, 1)
        )
        color_metrics['reflection_as_emission_rate'] = (
            reflection_as_emission / max(reflection_total, 1)
        )
        color_metrics['total_color_confusion_rate'] = (
            (emission_as_reflection + reflection_as_emission) /
            max(emission_total + reflection_total, 1)
        )

    return total_loss / len(loader), correct / total, class_acc, class_iou, color_metrics


# =============================================================================
# CURRICULUM LEARNING SUPPORT (v7)
# =============================================================================

def classify_samples_by_type(pairs):
    """
    Classify training samples as RGB or monochrome based on filename heuristics.

    This is a heuristic classification - we look for keywords in the path that
    indicate the image type. For perfect classification, we'd need to load and
    analyze each image, but that's too slow for large datasets.

    Args:
        pairs: List of {'image': path, 'mask': path} dicts

    Returns:
        rgb_pairs: List of pairs likely to be RGB images
        mono_pairs: List of pairs likely to be mono/narrowband images
    """
    rgb_keywords = ['rgb', 'color', 'lrgb', 'osc', 'dslr', 'oneshot', 'broadband']
    mono_keywords = ['ha', 'oiii', 'sii', 'lum', 'mono', 'narrowband', 'nb', 'luminance']

    rgb_pairs = []
    mono_pairs = []

    for pair in pairs:
        path_lower = pair['image'].lower()

        # Check for RGB indicators
        is_rgb = any(kw in path_lower for kw in rgb_keywords)
        is_mono = any(kw in path_lower for kw in mono_keywords)

        if is_rgb and not is_mono:
            rgb_pairs.append(pair)
        elif is_mono and not is_rgb:
            mono_pairs.append(pair)
        else:
            # Ambiguous - treat as RGB to favor color learning
            rgb_pairs.append(pair)

    return rgb_pairs, mono_pairs


def apply_curriculum_sampling(pairs, phase, epoch=None, total_epochs=None):
    """
    Apply curriculum learning sampling based on phase.

    Phase 1: 80% RGB, 20% mono - Focus on color learning
    Phase 2: 50% RGB, 50% mono - Balanced training
    Phase 3: All data         - Fine-tuning on full distribution

    If epoch/total_epochs provided with phase=0 (auto), automatically
    determines phase based on training progress.

    Args:
        pairs: All training pairs
        phase: 1, 2, 3 (manual) or 0 (auto-curriculum)
        epoch: Current epoch (for auto-curriculum)
        total_epochs: Total epochs (for auto-curriculum)

    Returns:
        Sampled pairs according to curriculum
    """
    rgb_pairs, mono_pairs = classify_samples_by_type(pairs)

    # Auto-curriculum: determine phase from epoch
    if phase == 0 and epoch is not None and total_epochs is not None:
        # First third: phase 1 (RGB focus)
        # Second third: phase 2 (balanced)
        # Final third: phase 3 (all data)
        progress = epoch / total_epochs
        if progress < 0.33:
            phase = 1
        elif progress < 0.66:
            phase = 2
        else:
            phase = 3

    # Apply curriculum based on phase
    if phase == 1:
        # 80% RGB, 20% mono
        n_total = len(pairs)
        n_rgb = int(0.8 * n_total)
        n_mono = n_total - n_rgb

        # Sample with replacement if needed
        sampled_rgb = random.choices(rgb_pairs, k=min(n_rgb, len(rgb_pairs) * 2))
        sampled_mono = random.choices(mono_pairs, k=min(n_mono, len(mono_pairs)))

        curriculum_pairs = sampled_rgb[:n_rgb] + sampled_mono[:n_mono]
        random.shuffle(curriculum_pairs)
        return curriculum_pairs, phase

    elif phase == 2:
        # 50% RGB, 50% mono
        n_total = len(pairs)
        n_each = n_total // 2

        sampled_rgb = random.choices(rgb_pairs, k=min(n_each, len(rgb_pairs) * 2))
        sampled_mono = random.choices(mono_pairs, k=min(n_each, len(mono_pairs) * 2))

        curriculum_pairs = sampled_rgb[:n_each] + sampled_mono[:n_each]
        random.shuffle(curriculum_pairs)
        return curriculum_pairs, phase

    else:
        # Phase 3 or default: all data
        return pairs.copy(), 3


def main():
    parser = argparse.ArgumentParser(description='Train 21-class segmentation model')
    parser.add_argument('--manifest', default='/home/scarter4work/projects/NukeX/training_data/unified_manifest.json')
    parser.add_argument('--weights', default='/home/scarter4work/projects/NukeX/training_data/class_weights_v5.npy')
    parser.add_argument('--output', default='/home/scarter4work/projects/NukeX/training_data/models')
    parser.add_argument('--batch-size', type=int, default=64, help='Batch size (64 for 16GB VRAM)')
    parser.add_argument('--epochs', type=int, default=100)
    parser.add_argument('--lr', type=float, default=1e-4)
    parser.add_argument('--image-size', type=int, default=512)
    parser.add_argument('--num-workers', type=int, default=4)
    parser.add_argument('--resume', help='Resume from checkpoint')

    # Enhanced RGB augmentation options (v7)
    parser.add_argument('--enhanced-augment', action='store_true', default=True,
                        help='Enable enhanced RGB augmentation (default: enabled)')
    parser.add_argument('--no-enhanced-augment', action='store_true',
                        help='Disable enhanced RGB augmentation for backward compat')
    parser.add_argument('--no-percentile-norm', action='store_true',
                        help='Use legacy /255 normalization instead of percentile')

    # Curriculum learning options (v7)
    parser.add_argument('--curriculum-phase', type=int, default=0, choices=[0, 1, 2, 3],
                        help='Curriculum phase: 1=80%% RGB, 2=50/50, 3=all, 0=auto')
    parser.add_argument('--auto-curriculum', action='store_true',
                        help='Automatically switch curriculum phases during training')

    # Synthetic color augmentation (v7 - fixes mono->RGB learning)
    parser.add_argument('--synthetic-color-prob', type=float, default=0.7,
                        help='Probability of applying synthetic color to mono images (default: 0.7)')
    parser.add_argument('--no-synthetic-color', action='store_true',
                        help='Disable synthetic color augmentation')

    # Stretch variation augmentation (v7 - handles inconsistent stretching)
    parser.add_argument('--stretch-augment', action='store_true', default=True,
                        help='Enable arcsinh stretch variation augmentation (default: enabled)')
    parser.add_argument('--no-stretch-augment', action='store_true',
                        help='Disable arcsinh stretch variation augmentation')

    args = parser.parse_args()

    # Handle augmentation flags
    enhanced_augment = args.enhanced_augment and not args.no_enhanced_augment
    use_percentile_norm = not args.no_percentile_norm
    synthetic_color_prob = 0.0 if args.no_synthetic_color else args.synthetic_color_prob
    stretch_augment = args.stretch_augment and not args.no_stretch_augment

    # Setup
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Device: {device}")
    if device.type == 'cuda':
        print(f"GPU: {torch.cuda.get_device_name()}")
        print(f"VRAM: {torch.cuda.get_device_properties(0).total_memory / 1e9:.1f} GB")

    # Load manifest
    print(f"\nLoading manifest: {args.manifest}")
    with open(args.manifest) as f:
        manifest = json.load(f)

    pairs = manifest['pairs']
    print(f"Total image-mask pairs: {len(pairs)}")

    # Load class weights
    print(f"Loading class weights: {args.weights}")
    class_weights = np.load(args.weights)
    class_weights = torch.tensor(class_weights, dtype=torch.float32).to(device)
    print("Class weights loaded")

    # Create datasets
    random.seed(42)
    random.shuffle(pairs)

    train_size = int(0.9 * len(pairs))
    train_pairs = pairs[:train_size]
    val_pairs = pairs[train_size:]

    # Classify samples for curriculum learning info
    rgb_pairs, mono_pairs = classify_samples_by_type(train_pairs)
    print(f"Training data composition: {len(rgb_pairs)} RGB, {len(mono_pairs)} mono")

    # Determine curriculum mode
    use_curriculum = args.curriculum_phase > 0 or args.auto_curriculum
    curriculum_phase = args.curriculum_phase if args.curriculum_phase > 0 else 0

    # For non-curriculum mode, use all pairs
    if not use_curriculum:
        current_train_pairs = train_pairs
    else:
        # Initial curriculum sampling (will be updated each epoch for auto-curriculum)
        current_train_pairs, curriculum_phase = apply_curriculum_sampling(
            train_pairs, curriculum_phase, epoch=0, total_epochs=args.epochs
        )

    train_dataset = ManifestDataset(
        current_train_pairs, args.image_size, augment=True,
        enhanced_augment=enhanced_augment, use_percentile_norm=use_percentile_norm,
        synthetic_color_prob=synthetic_color_prob, stretch_augment=stretch_augment
    )
    val_dataset = ManifestDataset(
        val_pairs, args.image_size, augment=False,
        enhanced_augment=False, use_percentile_norm=use_percentile_norm,
        synthetic_color_prob=0.0, stretch_augment=False  # No augmentation during validation
    )

    print(f"Training samples: {len(train_dataset)}")
    print(f"Validation samples: {len(val_dataset)}")

    # DataLoaders
    train_loader = DataLoader(
        train_dataset, batch_size=args.batch_size, shuffle=True,
        num_workers=args.num_workers, pin_memory=True, persistent_workers=True
    )
    val_loader = DataLoader(
        val_dataset, batch_size=args.batch_size, shuffle=False,
        num_workers=args.num_workers, pin_memory=True, persistent_workers=True
    )

    # Model - 4 input channels: RGB + color contrast (B-R)
    model = AstroUNet(in_channels=4, num_classes=NUM_CLASSES, base_features=32)
    model = model.to(device)
    print(f"Model parameters: {count_parameters(model):,}")

    # Loss, optimizer, scheduler
    criterion = CombinedLoss(class_weights=class_weights, ce_weight=0.5, dice_weight=0.5)
    optimizer = optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs, eta_min=1e-6)
    scaler = GradScaler('cuda')

    # Resume
    start_epoch = 0
    best_loss = float('inf')

    if args.resume:
        print(f"Resuming from {args.resume}")
        checkpoint = torch.load(args.resume, map_location=device)
        model.load_state_dict(checkpoint['model_state_dict'])
        optimizer.load_state_dict(checkpoint['optimizer_state_dict'])
        start_epoch = checkpoint['epoch'] + 1
        best_loss = checkpoint.get('best_loss', float('inf'))

    # Output directory
    output_path = Path(args.output)
    output_path.mkdir(parents=True, exist_ok=True)

    # Training loop
    print(f"\n{'='*60}")
    print(f"Training 21-class segmentation model (v7 RGB-enhanced)")
    print(f"Batch size: {args.batch_size}, Image size: {args.image_size}")
    print(f"Epochs: {args.epochs}, LR: {args.lr}")
    print(f"Enhanced augmentation: {enhanced_augment}")
    print(f"Percentile normalization: {use_percentile_norm}")
    print(f"Synthetic color augmentation: {synthetic_color_prob:.0%} probability")
    print(f"Stretch variation augmentation: {stretch_augment}")
    if use_curriculum:
        if args.auto_curriculum:
            print(f"Curriculum: AUTO (phases switch automatically)")
        else:
            print(f"Curriculum: Phase {curriculum_phase}")
    print(f"{'='*60}\n")

    # Track best color metrics for model selection
    best_color_confusion = float('inf')

    for epoch in range(start_epoch, args.epochs):
        # Auto-curriculum: update training data each epoch if enabled
        if args.auto_curriculum:
            new_pairs, new_phase = apply_curriculum_sampling(
                train_pairs, phase=0, epoch=epoch, total_epochs=args.epochs
            )
            if new_phase != curriculum_phase:
                print(f"  [Curriculum] Switching to phase {new_phase}")
                curriculum_phase = new_phase

            # Recreate dataset and loader with new sampling
            train_dataset = ManifestDataset(
                new_pairs, args.image_size, augment=True,
                enhanced_augment=enhanced_augment, use_percentile_norm=use_percentile_norm,
                synthetic_color_prob=synthetic_color_prob, stretch_augment=stretch_augment
            )
            train_loader = DataLoader(
                train_dataset, batch_size=args.batch_size, shuffle=True,
                num_workers=args.num_workers, pin_memory=True, persistent_workers=False
            )

        train_loss = train_epoch(model, train_loader, criterion, optimizer, scaler, device)
        val_loss, val_acc, class_acc, class_iou, color_metrics = validate(
            model, val_loader, criterion, device, compute_color_metrics=True
        )

        scheduler.step()
        lr = optimizer.param_groups[0]['lr']

        # Main epoch summary
        print(f"Epoch {epoch+1:3d}/{args.epochs} | "
              f"Train: {train_loss:.4f} | Val: {val_loss:.4f} | "
              f"Acc: {val_acc:.4f} | LR: {lr:.2e}")

        # Color discrimination metrics (always show for RGB-focused training)
        emission_iou = color_metrics.get('emission_iou', 0)
        reflection_iou = color_metrics.get('reflection_iou', 0)
        color_confusion = color_metrics.get('total_color_confusion_rate', 0)
        print(f"  Color: Emission IoU={emission_iou:.4f}, Reflection IoU={reflection_iou:.4f}, "
              f"Confusion={color_confusion:.4f}")

        # Print per-class accuracy every 10 epochs
        if (epoch + 1) % 10 == 0:
            print("  Per-class accuracy (top 10):")
            for name, acc in sorted(class_acc.items(), key=lambda x: -x[1])[:10]:
                print(f"    {name}: {acc:.4f}")

            # Also show IoU for key classes
            print("  Per-class IoU (nebula classes):")
            for name in ['nebula_emission', 'nebula_reflection', 'nebula_dark', 'nebula_planetary']:
                if name in class_iou:
                    print(f"    {name}: {class_iou[name]:.4f}")

            # Detailed color confusion breakdown
            print("  Color confusion details:")
            print(f"    Emission->Reflection: {color_metrics.get('emission_as_reflection_rate', 0):.4f}")
            print(f"    Reflection->Emission: {color_metrics.get('reflection_as_emission_rate', 0):.4f}")

        # Save checkpoint
        checkpoint = {
            'epoch': epoch,
            'model_state_dict': model.state_dict(),
            'optimizer_state_dict': optimizer.state_dict(),
            'train_loss': train_loss,
            'val_loss': val_loss,
            'val_acc': val_acc,
            'best_loss': best_loss,
            'color_metrics': color_metrics,
            'class_iou': class_iou,
            'enhanced_augment': enhanced_augment,
            'curriculum_phase': curriculum_phase if use_curriculum else None
        }

        # Save best model (by validation loss)
        if val_loss < best_loss:
            best_loss = val_loss
            checkpoint['best_loss'] = best_loss
            torch.save(checkpoint, output_path / 'best_model.pth')
            print(f"  -> Saved best model (loss: {best_loss:.4f})")

        # Also save best color discrimination model (lowest confusion rate)
        if color_confusion < best_color_confusion:
            best_color_confusion = color_confusion
            torch.save(checkpoint, output_path / 'best_color_model.pth')
            print(f"  -> Saved best color model (confusion: {best_color_confusion:.4f})")

        # Save periodic checkpoint
        if (epoch + 1) % 10 == 0:
            torch.save(checkpoint, output_path / f'checkpoint_epoch{epoch+1}.pth')

    # Save final model
    torch.save(checkpoint, output_path / 'final_model.pth')

    print(f"\nTraining complete!")
    print(f"Best model saved to: {output_path / 'best_model.pth'}")
    print(f"Best validation loss: {best_loss:.4f}")
    print(f"Best color model saved to: {output_path / 'best_color_model.pth'}")
    print(f"Best color confusion rate: {best_color_confusion:.4f}")


if __name__ == '__main__':
    main()
