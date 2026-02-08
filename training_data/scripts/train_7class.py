#!/usr/bin/env python3
"""
Train 7-class segmentation model for NukeX astrophotography.

Simplified taxonomy that merges the original 21 classes into 7 semantically
coherent groups:

  0 - Background       (Background, ArtifactGradient, ArtifactNoise)
  1 - BrightCompact    (StarBright, StarSaturated, ArtifactDiffraction)
  2 - FaintCompact     (StarMedium, StarFaint, StarClusterOpen, StarClusterGlobular)
  3 - BrightExtended   (NebulaEmission, NebulaReflection, NebulaPlanetary, Galaxies, GalacticCirrus)
  4 - DarkExtended     (NebulaDark, DustLane)
  5 - Artifact         (ArtifactHotPixel, ArtifactSatellite)
  6 - StarHalo         (StarHalo)

Model: AstroUNet(in_channels=3, num_classes=7, base_features=32)
Input: 3-channel RGB only (no B-R color contrast channel)

Training augmentations include synthetic vignetting, light pollution gradients,
hot pixel injection, and amplifier glow to improve robustness on uncalibrated
data.  Polynomial background subtraction is applied during preprocessing to
match the C++ inference pipeline.

Usage:
    python train_7class.py --data-dir /path/to/tiles --epochs 100 --batch-size 64
"""

import argparse
import logging
import os
import random
import sys
from datetime import datetime
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.amp import autocast, GradScaler
from torch.utils.data import Dataset, DataLoader

from tqdm import tqdm

# ---------------------------------------------------------------------------
# Model definition (self-contained to avoid external path dependencies)
# ---------------------------------------------------------------------------

class DoubleConv(nn.Module):
    """Two consecutive Conv2d -> BatchNorm -> ReLU blocks."""

    def __init__(self, in_ch, out_ch):
        super().__init__()
        self.block = nn.Sequential(
            nn.Conv2d(in_ch, out_ch, 3, padding=1, bias=False),
            nn.BatchNorm2d(out_ch),
            nn.ReLU(inplace=True),
            nn.Conv2d(out_ch, out_ch, 3, padding=1, bias=False),
            nn.BatchNorm2d(out_ch),
            nn.ReLU(inplace=True),
        )

    def forward(self, x):
        return self.block(x)


class Down(nn.Module):
    """MaxPool followed by DoubleConv."""

    def __init__(self, in_ch, out_ch):
        super().__init__()
        self.pool_conv = nn.Sequential(nn.MaxPool2d(2), DoubleConv(in_ch, out_ch))

    def forward(self, x):
        return self.pool_conv(x)


class Up(nn.Module):
    """Bilinear upsample + skip concatenation + DoubleConv."""

    def __init__(self, in_ch, out_ch):
        super().__init__()
        self.up = nn.Upsample(scale_factor=2, mode="bilinear", align_corners=True)
        self.conv = DoubleConv(in_ch, out_ch)

    def forward(self, x1, x2):
        x1 = self.up(x1)
        # Pad to match skip connection spatial dims
        dy = x2.size(2) - x1.size(2)
        dx = x2.size(3) - x1.size(3)
        x1 = nn.functional.pad(x1, [dx // 2, dx - dx // 2, dy // 2, dy - dy // 2])
        return self.conv(torch.cat([x2, x1], dim=1))


class AstroUNet(nn.Module):
    """Standard U-Net for astrophotography segmentation."""

    def __init__(self, in_channels=3, num_classes=7, base_features=32):
        super().__init__()
        bf = base_features
        self.inc = DoubleConv(in_channels, bf)
        self.down1 = Down(bf, bf * 2)
        self.down2 = Down(bf * 2, bf * 4)
        self.down3 = Down(bf * 4, bf * 8)
        self.down4 = Down(bf * 8, bf * 8)  # bottleneck (bilinear halves channels)
        self.up1 = Up(bf * 16, bf * 4)
        self.up2 = Up(bf * 8, bf * 2)
        self.up3 = Up(bf * 4, bf)
        self.up4 = Up(bf * 2, bf)
        self.outc = nn.Conv2d(bf, num_classes, kernel_size=1)

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
        return self.outc(x)

    def predict(self, x):
        return torch.argmax(self.forward(x), dim=1)


def count_parameters(model):
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

NUM_CLASSES = 7
IN_CHANNELS = 3  # RGB only, no B-R channel

CLASS_NAMES = [
    "Background",       # 0
    "BrightCompact",    # 1
    "FaintCompact",     # 2
    "BrightExtended",   # 3
    "DarkExtended",     # 4
    "Artifact",         # 5
    "StarHalo",         # 6
]

# Class weights for CrossEntropyLoss.
# Background is down-weighted; rarer classes are boosted.
DEFAULT_CLASS_WEIGHTS = [
    0.3,   # Background
    1.5,   # BrightCompact
    1.5,   # FaintCompact
    1.2,   # BrightExtended
    2.0,   # DarkExtended
    3.0,   # Artifact
    2.0,   # StarHalo
]

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# GPU optimizations
torch.backends.cudnn.benchmark = True
torch.backends.cuda.matmul.allow_tf32 = True
torch.backends.cudnn.allow_tf32 = True

# ---------------------------------------------------------------------------
# Polynomial background subtraction (matches C++ Compositor preprocessing)
# ---------------------------------------------------------------------------

def polynomial_background_subtract(img: np.ndarray) -> np.ndarray:
    """
    Subtract a 2nd-order polynomial background surface from each channel.

    The procedure mirrors the C++ implementation:
      - Sample every 16th pixel per channel
      - Determine 30th percentile as background threshold
      - Use background pixels to fit: a + bx + cy + dxy + ex^2 + fy^2
      - Subtract the fitted surface and clip to [0, 1]

    Args:
        img: float32 array of shape (H, W, 3) in [0, 1]

    Returns:
        Background-subtracted image, float32, [0, 1]
    """
    h, w, c = img.shape
    result = img.copy()

    # Normalised coordinate grids
    ys = np.arange(h, dtype=np.float64) / max(h - 1, 1)
    xs = np.arange(w, dtype=np.float64) / max(w - 1, 1)
    xg, yg = np.meshgrid(xs, ys)  # (H, W) each

    for ch in range(c):
        channel = img[:, :, ch].astype(np.float64)

        # Sub-sample every 16th pixel
        sample_y = np.arange(0, h, 16)
        sample_x = np.arange(0, w, 16)
        sy, sx = np.meshgrid(sample_y, sample_x, indexing="ij")
        sy = sy.ravel()
        sx = sx.ravel()
        vals = channel[sy, sx]

        # 30th percentile background threshold
        threshold = np.percentile(vals, 30)
        bg_mask = vals <= threshold
        if bg_mask.sum() < 6:
            # Not enough background pixels for a 6-parameter fit; skip
            continue

        bg_x = sx[bg_mask].astype(np.float64) / max(w - 1, 1)
        bg_y = sy[bg_mask].astype(np.float64) / max(h - 1, 1)
        bg_v = vals[bg_mask]

        # Design matrix: [1, x, y, xy, x^2, y^2]
        A = np.column_stack([
            np.ones_like(bg_x),
            bg_x,
            bg_y,
            bg_x * bg_y,
            bg_x ** 2,
            bg_y ** 2,
        ])

        # Least-squares fit
        try:
            coeffs, _, _, _ = np.linalg.lstsq(A, bg_v, rcond=None)
        except np.linalg.LinAlgError:
            continue

        # Evaluate surface over full image
        surface = (
            coeffs[0]
            + coeffs[1] * xg
            + coeffs[2] * yg
            + coeffs[3] * xg * yg
            + coeffs[4] * xg ** 2
            + coeffs[5] * yg ** 2
        )

        result[:, :, ch] = np.clip(channel - surface, 0.0, 1.0).astype(np.float32)

    return result


# ---------------------------------------------------------------------------
# Training augmentations
# ---------------------------------------------------------------------------

def augment_vignetting(img: np.ndarray) -> np.ndarray:
    """
    Synthetic vignetting: radial quadratic falloff with 15-45% strength
    and a random center offset.
    """
    h, w = img.shape[:2]
    strength = random.uniform(0.15, 0.45)
    cx = 0.5 + random.uniform(-0.1, 0.1)
    cy = 0.5 + random.uniform(-0.1, 0.1)

    ys = np.linspace(0, 1, h, dtype=np.float32)
    xs = np.linspace(0, 1, w, dtype=np.float32)
    xg, yg = np.meshgrid(xs, ys)
    r2 = (xg - cx) ** 2 + (yg - cy) ** 2
    max_r2 = max(cx, 1 - cx) ** 2 + max(cy, 1 - cy) ** 2
    vignette = 1.0 - strength * (r2 / max(max_r2, 1e-6))
    vignette = np.clip(vignette, 0, 1)[:, :, np.newaxis]

    return np.clip(img * vignette, 0, 1).astype(np.float32)


def augment_light_pollution(img: np.ndarray) -> np.ndarray:
    """
    Linear gradient in a random direction with sodium-lamp tint
    (warm orange: ~[1.0, 0.7, 0.3]).
    """
    h, w = img.shape[:2]
    angle = random.uniform(0, 2 * np.pi)
    strength = random.uniform(0.03, 0.15)

    ys = np.linspace(0, 1, h, dtype=np.float32)
    xs = np.linspace(0, 1, w, dtype=np.float32)
    xg, yg = np.meshgrid(xs, ys)
    gradient = (np.cos(angle) * (xg - 0.5) + np.sin(angle) * (yg - 0.5)) + 0.5
    gradient = np.clip(gradient, 0, 1)

    # Sodium tint in RGB
    tint = np.array([1.0, 0.7, 0.3], dtype=np.float32)
    pollution = strength * gradient[:, :, np.newaxis] * tint[np.newaxis, np.newaxis, :]

    return np.clip(img + pollution, 0, 1).astype(np.float32)


def augment_hot_pixels(
    img: np.ndarray, mask: np.ndarray, artifact_class: int = 5
) -> tuple:
    """
    Inject random bright single-pixel artifacts and label them as
    class *artifact_class* (Artifact).

    Returns:
        (augmented_img, augmented_mask)
    """
    h, w = img.shape[:2]
    n_hot = random.randint(5, 50)
    img = img.copy()
    mask = mask.copy()

    for _ in range(n_hot):
        y = random.randint(0, h - 1)
        x = random.randint(0, w - 1)
        brightness = random.uniform(0.8, 1.0)
        # Hot pixels tend to be single-channel or white
        if random.random() < 0.5:
            img[y, x, :] = brightness
        else:
            ch = random.randint(0, 2)
            img[y, x, ch] = brightness
        mask[y, x] = artifact_class

    return img, mask


def augment_amp_glow(img: np.ndarray) -> np.ndarray:
    """
    Exponential corner gradient simulating amplifier glow.

    Picks a random corner and applies an exponential ramp that decays
    toward the centre of the image.
    """
    h, w = img.shape[:2]
    corner_x = random.choice([0.0, 1.0])
    corner_y = random.choice([0.0, 1.0])
    strength = random.uniform(0.02, 0.12)
    decay = random.uniform(3.0, 6.0)

    ys = np.linspace(0, 1, h, dtype=np.float32)
    xs = np.linspace(0, 1, w, dtype=np.float32)
    xg, yg = np.meshgrid(xs, ys)
    dist = np.sqrt((xg - corner_x) ** 2 + (yg - corner_y) ** 2)
    glow = strength * np.exp(-decay * dist)

    # Amp glow is warm (reddish)
    tint = np.array([1.0, 0.6, 0.2], dtype=np.float32)
    glow_rgb = glow[:, :, np.newaxis] * tint[np.newaxis, np.newaxis, :]

    return np.clip(img + glow_rgb, 0, 1).astype(np.float32)


# ---------------------------------------------------------------------------
# Dataset
# ---------------------------------------------------------------------------

class TileDataset(Dataset):
    """
    Loads tile_*.npy / mask_*.npy pairs from a directory.

    Each tile is expected to be float32 (H, W, 3) in [0, 1].
    Each mask is int64/uint8 (H, W) with class IDs in [0, NUM_CLASSES).
    """

    def __init__(self, data_dir: str, augment: bool = True):
        self.data_dir = Path(data_dir)
        self.augment = augment

        # Discover all tile/mask pairs
        tile_files = sorted(self.data_dir.glob("tile_*.npy"))
        self.pairs = []
        for tf in tile_files:
            idx = tf.stem.replace("tile_", "")
            mf = self.data_dir / f"mask_{idx}.npy"
            if mf.exists():
                self.pairs.append((tf, mf))

        if not self.pairs:
            raise FileNotFoundError(
                f"No tile_*.npy / mask_*.npy pairs found in {data_dir}"
            )

        logger.info("Found %d tile/mask pairs in %s", len(self.pairs), data_dir)

    def __len__(self):
        return len(self.pairs)

    def __getitem__(self, idx):
        tile_path, mask_path = self.pairs[idx]
        img = np.load(tile_path).astype(np.float32)
        mask = np.load(mask_path).astype(np.int64)

        # Ensure [0, 1] range
        if img.max() > 1.5:
            img = img / 255.0
        img = np.clip(img, 0, 1)

        # Polynomial background subtraction (matches C++ pipeline)
        img = polynomial_background_subtract(img)

        # Geometric augmentations
        if self.augment:
            if random.random() > 0.5:
                img = np.flip(img, axis=1).copy()
                mask = np.flip(mask, axis=1).copy()
            if random.random() > 0.5:
                img = np.flip(img, axis=0).copy()
                mask = np.flip(mask, axis=0).copy()
            if random.random() > 0.5:
                k = random.randint(1, 3)
                img = np.rot90(img, k=k).copy()
                mask = np.rot90(mask, k=k).copy()

            # Brightness jitter
            if random.random() > 0.5:
                img = np.clip(img * random.uniform(0.8, 1.2), 0, 1)

        # Domain-specific augmentations
        if self.augment:
            if random.random() < 0.4:
                img = augment_vignetting(img)
            if random.random() < 0.3:
                img = augment_light_pollution(img)
            if random.random() < 0.25:
                img, mask = augment_hot_pixels(img, mask)
            if random.random() < 0.2:
                img = augment_amp_glow(img)

        # Validate mask range
        mask = np.clip(mask, 0, NUM_CLASSES - 1)

        # Convert to tensors (C, H, W)
        img_tensor = torch.from_numpy(img.transpose(2, 0, 1).copy()).float()
        mask_tensor = torch.from_numpy(mask.copy()).long()

        return img_tensor, mask_tensor


# ---------------------------------------------------------------------------
# Training helpers
# ---------------------------------------------------------------------------

class DiceLoss(nn.Module):
    """Soft Dice loss across all classes."""

    def __init__(self, smooth: float = 1.0):
        super().__init__()
        self.smooth = smooth

    def forward(self, pred, target):
        pred = torch.softmax(pred, dim=1)
        target_oh = torch.zeros_like(pred)
        target_oh.scatter_(1, target.unsqueeze(1), 1)
        inter = (pred * target_oh).sum(dim=(2, 3))
        union = pred.sum(dim=(2, 3)) + target_oh.sum(dim=(2, 3))
        dice = (2 * inter + self.smooth) / (union + self.smooth)
        return 1 - dice.mean()


class CombinedLoss(nn.Module):
    """Weighted sum of CrossEntropyLoss and DiceLoss."""

    def __init__(self, class_weights=None, ce_weight=0.5, dice_weight=0.5):
        super().__init__()
        self.ce_weight = ce_weight
        self.dice_weight = dice_weight
        self.ce = nn.CrossEntropyLoss(weight=class_weights)
        self.dice = DiceLoss()

    def forward(self, pred, target):
        return self.ce_weight * self.ce(pred, target) + self.dice_weight * self.dice(
            pred, target
        )


def train_epoch(model, loader, criterion, optimizer, scaler, device):
    model.train()
    total_loss = 0.0
    pbar = tqdm(loader, desc="Training", leave=False)
    for images, masks in pbar:
        images = images.to(device, non_blocking=True)
        masks = masks.to(device, non_blocking=True)

        optimizer.zero_grad(set_to_none=True)

        with autocast("cuda"):
            outputs = model(images)
            loss = criterion(outputs, masks)

        scaler.scale(loss).backward()
        scaler.step(optimizer)
        scaler.update()

        total_loss += loss.item()
        pbar.set_postfix({"loss": f"{loss.item():.4f}"})

    return total_loss / max(len(loader), 1)


@torch.no_grad()
def validate(model, loader, criterion, device):
    model.eval()
    total_loss = 0.0
    correct = 0
    total = 0

    class_correct = torch.zeros(NUM_CLASSES)
    class_total = torch.zeros(NUM_CLASSES)
    class_inter = torch.zeros(NUM_CLASSES)
    class_union = torch.zeros(NUM_CLASSES)

    for images, masks in tqdm(loader, desc="Validating", leave=False):
        images = images.to(device, non_blocking=True)
        masks = masks.to(device, non_blocking=True)

        with autocast("cuda"):
            outputs = model(images)
            loss = criterion(outputs, masks)

        total_loss += loss.item()
        preds = outputs.argmax(dim=1)
        correct += (preds == masks).sum().item()
        total += masks.numel()

        for c in range(NUM_CLASSES):
            gt_mask = masks == c
            pr_mask = preds == c
            class_total[c] += gt_mask.sum().item()
            class_correct[c] += ((preds == c) & gt_mask).sum().item()
            class_inter[c] += (pr_mask & gt_mask).sum().item()
            class_union[c] += (pr_mask | gt_mask).sum().item()

    acc = correct / max(total, 1)
    val_loss = total_loss / max(len(loader), 1)

    per_class_acc = {}
    per_class_iou = {}
    for c in range(NUM_CLASSES):
        if class_total[c] > 0:
            per_class_acc[CLASS_NAMES[c]] = class_correct[c].item() / class_total[c].item()
        if class_union[c] > 0:
            per_class_iou[CLASS_NAMES[c]] = class_inter[c].item() / class_union[c].item()

    return val_loss, acc, per_class_acc, per_class_iou


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Train 7-class NukeX segmentation model"
    )
    parser.add_argument(
        "--data-dir",
        type=str,
        required=True,
        help="Directory containing tile_*.npy and mask_*.npy files",
    )
    parser.add_argument(
        "--output",
        type=str,
        default="models_7class",
        help="Output directory for checkpoints (default: models_7class)",
    )
    parser.add_argument("--epochs", type=int, default=100, help="Number of epochs")
    parser.add_argument(
        "--batch-size",
        type=int,
        default=64,
        help="Batch size (64 recommended for RTX 5070 Ti 16GB)",
    )
    parser.add_argument("--lr", type=float, default=1e-4, help="Initial learning rate")
    parser.add_argument("--image-size", type=int, default=512, help="Tile size")
    parser.add_argument("--num-workers", type=int, default=4, help="DataLoader workers")
    parser.add_argument("--resume", type=str, default=None, help="Resume from checkpoint")
    parser.add_argument(
        "--class-weights",
        type=str,
        default=None,
        help="Path to .npy class weight file (overrides defaults)",
    )
    args = parser.parse_args()

    # ------------------------------------------------------------------
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    logger.info("Device: %s", device)
    if device.type == "cuda":
        logger.info("GPU: %s", torch.cuda.get_device_name())
        vram = torch.cuda.get_device_properties(0).total_mem / 1e9
        logger.info("VRAM: %.1f GB", vram)

    # ------------------------------------------------------------------
    # Dataset
    # ------------------------------------------------------------------
    full_dataset = TileDataset(args.data_dir, augment=True)

    n_total = len(full_dataset)
    n_train = int(0.9 * n_total)
    n_val = n_total - n_train

    generator = torch.Generator().manual_seed(42)
    train_set, val_set = torch.utils.data.random_split(
        full_dataset, [n_train, n_val], generator=generator
    )
    # Disable augmentation on the validation split (wrap with a thin dataset)
    val_set.dataset = TileDataset(args.data_dir, augment=False)

    logger.info("Training samples: %d  |  Validation samples: %d", n_train, n_val)

    train_loader = DataLoader(
        train_set,
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.num_workers,
        pin_memory=True,
        persistent_workers=True,
    )
    val_loader = DataLoader(
        val_set,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.num_workers,
        pin_memory=True,
        persistent_workers=True,
    )

    # ------------------------------------------------------------------
    # Model
    # ------------------------------------------------------------------
    model = AstroUNet(in_channels=IN_CHANNELS, num_classes=NUM_CLASSES, base_features=32)
    model = model.to(device)
    logger.info("Model parameters: %s", f"{count_parameters(model):,}")

    # ------------------------------------------------------------------
    # Class weights
    # ------------------------------------------------------------------
    if args.class_weights and Path(args.class_weights).exists():
        cw = np.load(args.class_weights).astype(np.float32)
        assert len(cw) == NUM_CLASSES, (
            f"Weight file has {len(cw)} entries, expected {NUM_CLASSES}"
        )
        class_weights = torch.tensor(cw, dtype=torch.float32).to(device)
        logger.info("Loaded class weights from %s", args.class_weights)
    else:
        class_weights = torch.tensor(DEFAULT_CLASS_WEIGHTS, dtype=torch.float32).to(device)
        logger.info("Using default class weights: %s", DEFAULT_CLASS_WEIGHTS)

    # ------------------------------------------------------------------
    # Loss / optimiser / scheduler
    # ------------------------------------------------------------------
    criterion = CombinedLoss(class_weights=class_weights, ce_weight=0.5, dice_weight=0.5)
    optimizer = optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=args.epochs, eta_min=1e-6
    )
    scaler = GradScaler("cuda")

    # ------------------------------------------------------------------
    # Resume
    # ------------------------------------------------------------------
    start_epoch = 0
    best_loss = float("inf")

    if args.resume:
        logger.info("Resuming from %s", args.resume)
        ckpt = torch.load(args.resume, map_location=device, weights_only=False)
        model.load_state_dict(ckpt["model_state_dict"])
        optimizer.load_state_dict(ckpt["optimizer_state_dict"])
        start_epoch = ckpt.get("epoch", -1) + 1
        best_loss = ckpt.get("best_loss", float("inf"))

    # ------------------------------------------------------------------
    # Output directory
    # ------------------------------------------------------------------
    output_path = Path(args.output)
    output_path.mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------
    # Training loop
    # ------------------------------------------------------------------
    logger.info("=" * 60)
    logger.info("Training 7-class segmentation model")
    logger.info(
        "Batch=%d  ImageSize=%d  Epochs=%d  LR=%s",
        args.batch_size,
        args.image_size,
        args.epochs,
        args.lr,
    )
    logger.info("=" * 60)

    for epoch in range(start_epoch, args.epochs):
        train_loss = train_epoch(model, train_loader, criterion, optimizer, scaler, device)
        val_loss, val_acc, class_acc, class_iou = validate(
            model, val_loader, criterion, device
        )
        scheduler.step()
        lr = optimizer.param_groups[0]["lr"]

        logger.info(
            "Epoch %3d/%d | Train %.4f | Val %.4f | Acc %.4f | LR %.2e",
            epoch + 1,
            args.epochs,
            train_loss,
            val_loss,
            val_acc,
            lr,
        )

        # Per-class details every 10 epochs
        if (epoch + 1) % 10 == 0:
            logger.info("  Per-class accuracy:")
            for name, acc in sorted(class_acc.items(), key=lambda x: -x[1]):
                logger.info("    %-18s %.4f", name, acc)
            logger.info("  Per-class IoU:")
            for name, iou in sorted(class_iou.items(), key=lambda x: -x[1]):
                logger.info("    %-18s %.4f", name, iou)

        # Checkpoint
        ckpt = {
            "epoch": epoch,
            "model_state_dict": model.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
            "train_loss": train_loss,
            "val_loss": val_loss,
            "val_acc": val_acc,
            "best_loss": best_loss,
            "class_acc": class_acc,
            "class_iou": class_iou,
            "num_classes": NUM_CLASSES,
            "in_channels": IN_CHANNELS,
        }

        if val_loss < best_loss:
            best_loss = val_loss
            ckpt["best_loss"] = best_loss
            torch.save(ckpt, output_path / "best_model_7class.pth")
            logger.info("  -> Saved best model (loss %.4f)", best_loss)

        if (epoch + 1) % 10 == 0:
            torch.save(ckpt, output_path / f"checkpoint_epoch{epoch+1}.pth")

    # Final save
    torch.save(ckpt, output_path / "final_model_7class.pth")
    logger.info("Training complete.  Best val loss: %.4f", best_loss)
    logger.info("Best model: %s", output_path / "best_model_7class.pth")


if __name__ == "__main__":
    main()
