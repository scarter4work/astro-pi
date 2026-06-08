#!/usr/bin/env python3
"""
Visualization script for evaluating NukeX 21-class segmentation model.

Creates visualizations for:
- Side-by-side comparison (original, ground truth, prediction, overlay)
- Confusion matrix
- Per-class IoU bar chart

Usage:
    # Single image
    python visualize_predictions.py --model best_model.pth --images test.png --output vis_results/

    # Batch mode (directory of images)
    python visualize_predictions.py --model best_model.pth --images test_images/ --output vis_results/ --batch

    # With ground truth masks for metrics
    python visualize_predictions.py --model best_model.pth --images test_images/ --masks test_masks/ --output vis_results/ --batch

    # Generate confusion matrix and IoU chart
    python visualize_predictions.py --model best_model.pth --images test_images/ --masks test_masks/ --output vis_results/ --batch --metrics

Copyright (c) 2026 Scott Carter
"""

import argparse
import json
import numpy as np
import torch
from PIL import Image
from pathlib import Path
import sys
from tqdm import tqdm
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap
import seaborn as sns

# Add model path
sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet

# Import unified palette
from segmentation_palette import (
    NUM_CLASSES,
    CLASS_NAMES,
    CLASS_DISPLAY_NAMES,
    CLASS_COLORS_RGB,
    CLASS_COLORS_NORM,
    apply_colormap,
    create_legend_image,
    get_matplotlib_cmap,
)


def load_model(model_path, device, in_channels=4):
    """
    Load trained model from checkpoint.

    Args:
        model_path: Path to .pth checkpoint file
        device: torch device
        in_channels: Number of input channels (4 for RGB + color contrast)

    Returns:
        Loaded model in eval mode
    """
    model = AstroUNet(in_channels=in_channels, num_classes=NUM_CLASSES, base_features=32)

    checkpoint = torch.load(model_path, map_location=device)

    # Handle different checkpoint formats
    if 'model_state_dict' in checkpoint:
        model.load_state_dict(checkpoint['model_state_dict'])
        print(f"Loaded checkpoint from epoch {checkpoint.get('epoch', 'unknown')}")
        if 'val_acc' in checkpoint:
            print(f"  Validation accuracy: {checkpoint['val_acc']:.4f}")
        if 'val_loss' in checkpoint:
            print(f"  Validation loss: {checkpoint['val_loss']:.4f}")
        if 'color_metrics' in checkpoint:
            cm = checkpoint['color_metrics']
            print(f"  Emission IoU: {cm.get('emission_iou', 'N/A')}")
            print(f"  Reflection IoU: {cm.get('reflection_iou', 'N/A')}")
    else:
        model.load_state_dict(checkpoint)

    model = model.to(device)
    model.eval()
    return model


def preprocess_image(image_path, size=512):
    """
    Load and preprocess image for inference.

    Matches the preprocessing in train_21class.py:
    - Resize to target size
    - Percentile normalization
    - Add color contrast channel (B-R)

    Args:
        image_path: Path to input image
        size: Target size for processing

    Returns:
        img_tensor: Preprocessed tensor (1, 4, H, W)
        original_img: Original PIL image
        original_size: (width, height) of original
    """
    img = Image.open(image_path).convert('RGB')
    original_size = img.size
    original_img = img.copy()

    img = img.resize((size, size), Image.BILINEAR)
    img_np = np.array(img).astype(np.float32)

    # Percentile normalization (matches v7 training)
    for c in range(3):
        p1, p99 = np.percentile(img_np[:, :, c], [1, 99])
        if p99 > p1:
            img_np[:, :, c] = np.clip((img_np[:, :, c] - p1) / (p99 - p1), 0, 1)
        else:
            img_np[:, :, c] = np.clip(img_np[:, :, c] / 255.0, 0, 1)

    # Add color contrast channel: (Blue - Red)
    blue = img_np[:, :, 2]
    red = img_np[:, :, 0]
    green = img_np[:, :, 1]

    # Detect narrowband/monochrome images
    channel_std = np.std([red.mean(), green.mean(), blue.mean()])
    is_narrowband = channel_std < 0.01

    if is_narrowband:
        color_contrast = np.zeros_like(blue)[:, :, np.newaxis]
    else:
        color_contrast = (blue - red)[:, :, np.newaxis]

    # Stack as 4 channels: RGB + color_contrast
    img_4ch = np.concatenate([img_np, color_contrast], axis=2)

    img_tensor = torch.from_numpy(img_4ch).permute(2, 0, 1).unsqueeze(0)
    return img_tensor, original_img, original_size


def run_inference(model, img_tensor, device):
    """
    Run model inference on preprocessed image.

    Args:
        model: Loaded model
        img_tensor: Preprocessed tensor (1, 4, H, W)
        device: torch device

    Returns:
        pred_mask: Predicted class mask (H, W)
        pred_probs: Class probabilities (NUM_CLASSES, H, W)
    """
    img_tensor = img_tensor.to(device)

    with torch.no_grad():
        output = model(img_tensor)
        pred_probs = torch.softmax(output, dim=1).squeeze().cpu().numpy()
        pred_mask = output.argmax(dim=1).squeeze().cpu().numpy()

    return pred_mask, pred_probs


def create_overlay(original_img, mask, alpha=0.5):
    """
    Create overlay of segmentation mask on original image.

    Args:
        original_img: PIL Image or numpy array
        mask: Segmentation mask (H, W)
        alpha: Blend factor (0=original only, 1=mask only)

    Returns:
        Blended RGB numpy array (H, W, 3)
    """
    if isinstance(original_img, Image.Image):
        h, w = mask.shape
        original_np = np.array(original_img.resize((w, h)))
    else:
        original_np = original_img

    colored_mask = apply_colormap(mask)
    overlay = (alpha * colored_mask + (1 - alpha) * original_np).astype(np.uint8)
    return overlay


def create_comparison_figure(original_img, pred_mask, gt_mask=None, save_path=None, title=None):
    """
    Create a side-by-side comparison visualization.

    Creates a figure with:
    - Original image
    - Ground truth mask (if available)
    - Predicted mask
    - Overlay of prediction on original

    Args:
        original_img: PIL Image
        pred_mask: Predicted segmentation mask (H, W)
        gt_mask: Ground truth mask (optional, H, W)
        save_path: Path to save figure
        title: Figure title

    Returns:
        matplotlib figure
    """
    h, w = pred_mask.shape
    original_resized = original_img.resize((w, h))
    original_np = np.array(original_resized)

    # Colorize masks
    pred_colored = apply_colormap(pred_mask)
    overlay = create_overlay(original_resized, pred_mask, alpha=0.5)

    if gt_mask is not None:
        # 4 panels: original, GT, prediction, overlay
        fig, axes = plt.subplots(1, 4, figsize=(20, 5))
        gt_colored = apply_colormap(gt_mask)

        axes[0].imshow(original_np)
        axes[0].set_title('Original')
        axes[0].axis('off')

        axes[1].imshow(gt_colored)
        axes[1].set_title('Ground Truth')
        axes[1].axis('off')

        axes[2].imshow(pred_colored)
        axes[2].set_title('Prediction')
        axes[2].axis('off')

        axes[3].imshow(overlay)
        axes[3].set_title('Overlay')
        axes[3].axis('off')
    else:
        # 3 panels: original, prediction, overlay
        fig, axes = plt.subplots(1, 3, figsize=(15, 5))

        axes[0].imshow(original_np)
        axes[0].set_title('Original')
        axes[0].axis('off')

        axes[1].imshow(pred_colored)
        axes[1].set_title('Prediction')
        axes[1].axis('off')

        axes[2].imshow(overlay)
        axes[2].set_title('Overlay')
        axes[2].axis('off')

    if title:
        fig.suptitle(title, fontsize=14)

    plt.tight_layout()

    if save_path:
        fig.savefig(save_path, dpi=150, bbox_inches='tight')
        plt.close(fig)

    return fig


def compute_confusion_matrix(pred_masks, gt_masks):
    """
    Compute confusion matrix from predictions and ground truth.

    Args:
        pred_masks: List of predicted masks
        gt_masks: List of ground truth masks

    Returns:
        Confusion matrix (NUM_CLASSES, NUM_CLASSES)
    """
    confusion = np.zeros((NUM_CLASSES, NUM_CLASSES), dtype=np.int64)

    for pred, gt in zip(pred_masks, gt_masks):
        for true_class in range(NUM_CLASSES):
            for pred_class in range(NUM_CLASSES):
                confusion[true_class, pred_class] += np.sum(
                    (gt == true_class) & (pred == pred_class)
                )

    return confusion


def compute_iou_per_class(pred_masks, gt_masks):
    """
    Compute Intersection over Union (IoU) for each class.

    Args:
        pred_masks: List of predicted masks
        gt_masks: List of ground truth masks

    Returns:
        Dictionary mapping class names to IoU values
    """
    intersection = np.zeros(NUM_CLASSES)
    union = np.zeros(NUM_CLASSES)

    for pred, gt in zip(pred_masks, gt_masks):
        for c in range(NUM_CLASSES):
            pred_c = (pred == c)
            gt_c = (gt == c)
            intersection[c] += np.sum(pred_c & gt_c)
            union[c] += np.sum(pred_c | gt_c)

    iou = {}
    for c in range(NUM_CLASSES):
        if union[c] > 0:
            iou[CLASS_NAMES[c]] = intersection[c] / union[c]
        else:
            iou[CLASS_NAMES[c]] = np.nan  # Class not present

    return iou


def plot_confusion_matrix(confusion, save_path=None, normalize=True):
    """
    Plot confusion matrix as a heatmap.

    Args:
        confusion: Confusion matrix (NUM_CLASSES, NUM_CLASSES)
        save_path: Path to save figure
        normalize: If True, normalize rows to show percentages

    Returns:
        matplotlib figure
    """
    if normalize:
        # Normalize by true class (row-wise)
        row_sums = confusion.sum(axis=1, keepdims=True)
        row_sums[row_sums == 0] = 1  # Avoid division by zero
        confusion_norm = confusion.astype(np.float64) / row_sums
        fmt = '.2f'
        title = 'Confusion Matrix (Normalized)'
    else:
        confusion_norm = confusion
        fmt = 'd'
        title = 'Confusion Matrix'

    # Filter to show only classes with data
    class_has_data = (confusion.sum(axis=1) > 0) | (confusion.sum(axis=0) > 0)
    active_classes = np.where(class_has_data)[0]

    if len(active_classes) < NUM_CLASSES:
        print(f"Showing {len(active_classes)} classes with data (out of {NUM_CLASSES})")
        confusion_norm = confusion_norm[np.ix_(active_classes, active_classes)]
        labels = [CLASS_DISPLAY_NAMES[i] for i in active_classes]
    else:
        labels = CLASS_DISPLAY_NAMES

    # Create figure
    fig_size = max(10, len(labels) * 0.5)
    fig, ax = plt.subplots(figsize=(fig_size, fig_size))

    # Plot heatmap
    sns.heatmap(
        confusion_norm,
        annot=True,
        fmt=fmt,
        cmap='Blues',
        xticklabels=labels,
        yticklabels=labels,
        ax=ax,
        cbar_kws={'label': 'Proportion' if normalize else 'Count'}
    )

    ax.set_xlabel('Predicted Class')
    ax.set_ylabel('True Class')
    ax.set_title(title)

    plt.xticks(rotation=45, ha='right')
    plt.yticks(rotation=0)
    plt.tight_layout()

    if save_path:
        fig.savefig(save_path, dpi=150, bbox_inches='tight')
        plt.close(fig)

    return fig


def plot_iou_bar_chart(iou_dict, save_path=None):
    """
    Plot per-class IoU as a horizontal bar chart.

    Args:
        iou_dict: Dictionary mapping class names to IoU values
        save_path: Path to save figure

    Returns:
        matplotlib figure
    """
    # Sort by IoU value (descending), exclude NaN values
    valid_classes = [(name, iou) for name, iou in iou_dict.items() if not np.isnan(iou)]
    sorted_classes = sorted(valid_classes, key=lambda x: x[1], reverse=True)

    names = [x[0] for x in sorted_classes]
    ious = [x[1] for x in sorted_classes]

    # Get display names
    display_names = []
    for name in names:
        idx = CLASS_NAMES.index(name) if name in CLASS_NAMES else -1
        if idx >= 0:
            display_names.append(CLASS_DISPLAY_NAMES[idx])
        else:
            display_names.append(name)

    # Get colors from palette
    colors = []
    for name in names:
        idx = CLASS_NAMES.index(name) if name in CLASS_NAMES else -1
        if idx >= 0:
            colors.append(tuple(c/255 for c in CLASS_COLORS_RGB[idx]))
        else:
            colors.append((0.5, 0.5, 0.5))

    # Create figure
    fig_height = max(6, len(names) * 0.3)
    fig, ax = plt.subplots(figsize=(10, fig_height))

    y_pos = np.arange(len(names))
    bars = ax.barh(y_pos, ious, color=colors, edgecolor='black', linewidth=0.5)

    ax.set_yticks(y_pos)
    ax.set_yticklabels(display_names)
    ax.invert_yaxis()  # Best at top
    ax.set_xlabel('IoU')
    ax.set_title('Per-Class Intersection over Union (IoU)')
    ax.set_xlim(0, 1)

    # Add value labels on bars
    for bar, iou in zip(bars, ious):
        ax.text(bar.get_width() + 0.01, bar.get_y() + bar.get_height()/2,
                f'{iou:.3f}', va='center', fontsize=9)

    # Add mean IoU line
    mean_iou = np.nanmean(ious)
    ax.axvline(x=mean_iou, color='red', linestyle='--', linewidth=2,
               label=f'Mean IoU: {mean_iou:.3f}')
    ax.legend(loc='lower right')

    plt.tight_layout()

    if save_path:
        fig.savefig(save_path, dpi=150, bbox_inches='tight')
        plt.close(fig)

    return fig


def print_class_stats(mask, header="Class Statistics"):
    """
    Print statistics about detected classes in a mask.

    Args:
        mask: Segmentation mask (H, W)
        header: Header text to print
    """
    unique, counts = np.unique(mask, return_counts=True)
    total = mask.size

    print(f"\n{header}:")
    print("-" * 50)
    for class_idx, count in sorted(zip(unique, counts), key=lambda x: -x[1]):
        pct = 100 * count / total
        if pct > 0.1:  # Only show classes with >0.1%
            display_name = CLASS_DISPLAY_NAMES[class_idx] if class_idx < len(CLASS_DISPLAY_NAMES) else f"Class {class_idx}"
            print(f"  {display_name:25s}: {pct:6.2f}% ({count:,} pixels)")


def find_matching_mask(image_path, masks_dir):
    """
    Find ground truth mask matching an image.

    Tries various naming conventions:
    - Same filename with different extension
    - _mask suffix
    - _segmentation suffix

    Args:
        image_path: Path to input image
        masks_dir: Directory containing masks

    Returns:
        Path to mask file or None if not found
    """
    image_path = Path(image_path)
    masks_dir = Path(masks_dir)

    stem = image_path.stem

    # Try various naming patterns
    patterns = [
        f"{stem}.png",
        f"{stem}_mask.png",
        f"{stem}_segmentation.png",
        f"{stem}.npy",
        f"{stem}_mask.npy",
    ]

    for pattern in patterns:
        mask_path = masks_dir / pattern
        if mask_path.exists():
            return mask_path

    return None


def load_mask(mask_path, size=None):
    """
    Load a segmentation mask from file.

    Args:
        mask_path: Path to mask file (.png or .npy)
        size: Target size (width, height) to resize to

    Returns:
        Mask as numpy array (H, W)
    """
    mask_path = Path(mask_path)

    if mask_path.suffix == '.npy':
        mask = np.load(mask_path)
    else:
        mask_img = Image.open(mask_path)
        if size:
            mask_img = mask_img.resize(size, Image.NEAREST)
        mask = np.array(mask_img)

    return mask.astype(np.int64)


def main():
    parser = argparse.ArgumentParser(
        description='Visualize predictions from NukeX 21-class segmentation model',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Single image
  python visualize_predictions.py --model best_model.pth --images test.png

  # Batch mode with metrics
  python visualize_predictions.py --model best_model.pth --images test_dir/ --masks masks_dir/ --batch --metrics
        """
    )

    parser.add_argument('--model', required=True, help='Path to trained model (.pth file)')
    parser.add_argument('--images', required=True, help='Input image or directory of images')
    parser.add_argument('--masks', help='Ground truth masks directory (for metrics)')
    parser.add_argument('--output', default='vis_results', help='Output directory')
    parser.add_argument('--batch', action='store_true', help='Process directory of images')
    parser.add_argument('--metrics', action='store_true', help='Generate confusion matrix and IoU chart')
    parser.add_argument('--size', type=int, default=512, help='Processing size (default: 512)')
    parser.add_argument('--overlay-alpha', type=float, default=0.5, help='Overlay blend factor (default: 0.5)')
    parser.add_argument('--save-masks', action='store_true', help='Save raw prediction masks')
    parser.add_argument('--legend', action='store_true', help='Save color legend')

    args = parser.parse_args()

    # Setup
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Device: {device}")

    # Create output directory
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Load model
    print(f"\nLoading model: {args.model}")
    model = load_model(args.model, device)
    print("Model loaded successfully")

    # Save legend if requested
    if args.legend:
        legend_path = output_dir / 'class_legend.png'
        legend = create_legend_image(cell_width=180, cell_height=35, font_size=14)
        Image.fromarray(legend).save(legend_path)
        print(f"Saved legend to: {legend_path}")

    # Collect images to process
    images_path = Path(args.images)
    if args.batch:
        if not images_path.is_dir():
            print(f"Error: {images_path} is not a directory. Use --batch only with directories.")
            return

        image_files = []
        for ext in ['*.png', '*.jpg', '*.jpeg', '*.tif', '*.tiff', '*.fits', '*.fit']:
            image_files.extend(images_path.glob(ext))
            image_files.extend(images_path.glob(ext.upper()))

        image_files = sorted(set(image_files))
        print(f"\nFound {len(image_files)} images to process")
    else:
        if not images_path.exists():
            print(f"Error: Image not found: {images_path}")
            return
        image_files = [images_path]

    # Process images
    pred_masks_all = []
    gt_masks_all = []

    for img_path in tqdm(image_files, desc="Processing images"):
        try:
            # Preprocess and run inference
            img_tensor, original_img, original_size = preprocess_image(img_path, args.size)
            pred_mask, pred_probs = run_inference(model, img_tensor, device)

            # Find ground truth if available
            gt_mask = None
            if args.masks:
                mask_path = find_matching_mask(img_path, args.masks)
                if mask_path:
                    gt_mask = load_mask(mask_path, (args.size, args.size))
                    pred_masks_all.append(pred_mask)
                    gt_masks_all.append(gt_mask)

            # Create comparison figure
            fig_path = output_dir / f"{img_path.stem}_comparison.png"
            create_comparison_figure(
                original_img, pred_mask, gt_mask,
                save_path=fig_path,
                title=img_path.name
            )

            # Save raw mask if requested
            if args.save_masks:
                mask_path = output_dir / f"{img_path.stem}_pred_mask.png"
                Image.fromarray(pred_mask.astype(np.uint8)).save(mask_path)

                # Also save colorized version
                colored_path = output_dir / f"{img_path.stem}_pred_colored.png"
                Image.fromarray(apply_colormap(pred_mask)).save(colored_path)

            # Print stats for single image mode
            if not args.batch:
                print_class_stats(pred_mask, "Predicted Classes")
                if gt_mask is not None:
                    print_class_stats(gt_mask, "Ground Truth Classes")

                    # Compute single-image metrics
                    iou = compute_iou_per_class([pred_mask], [gt_mask])
                    print("\nPer-Class IoU:")
                    for name, value in sorted(iou.items(), key=lambda x: -x[1] if not np.isnan(x[1]) else -1):
                        if not np.isnan(value):
                            print(f"  {name}: {value:.4f}")

                    mean_iou = np.nanmean(list(iou.values()))
                    print(f"\nMean IoU: {mean_iou:.4f}")

                    # Pixel accuracy
                    accuracy = np.sum(pred_mask == gt_mask) / pred_mask.size
                    print(f"Pixel Accuracy: {accuracy:.4f}")

        except Exception as e:
            print(f"Error processing {img_path}: {e}")
            continue

    # Generate metrics visualizations
    if args.metrics and len(pred_masks_all) > 0:
        print(f"\nGenerating metrics from {len(pred_masks_all)} image pairs...")

        # Confusion matrix
        confusion = compute_confusion_matrix(pred_masks_all, gt_masks_all)
        confusion_path = output_dir / 'confusion_matrix.png'
        plot_confusion_matrix(confusion, save_path=confusion_path, normalize=True)
        print(f"Saved confusion matrix: {confusion_path}")

        # Also save unnormalized
        confusion_raw_path = output_dir / 'confusion_matrix_raw.png'
        plot_confusion_matrix(confusion, save_path=confusion_raw_path, normalize=False)

        # IoU bar chart
        iou_dict = compute_iou_per_class(pred_masks_all, gt_masks_all)
        iou_path = output_dir / 'iou_per_class.png'
        plot_iou_bar_chart(iou_dict, save_path=iou_path)
        print(f"Saved IoU chart: {iou_path}")

        # Print summary metrics
        print("\n" + "="*60)
        print("METRICS SUMMARY")
        print("="*60)

        # Overall accuracy
        total_correct = sum(np.sum(p == g) for p, g in zip(pred_masks_all, gt_masks_all))
        total_pixels = sum(p.size for p in pred_masks_all)
        overall_accuracy = total_correct / total_pixels
        print(f"Overall Pixel Accuracy: {overall_accuracy:.4f}")

        # Mean IoU
        valid_ious = [v for v in iou_dict.values() if not np.isnan(v)]
        mean_iou = np.mean(valid_ious) if valid_ious else 0
        print(f"Mean IoU (mIoU): {mean_iou:.4f}")

        # Per-class IoU
        print("\nPer-Class IoU:")
        for name, value in sorted(iou_dict.items(), key=lambda x: -x[1] if not np.isnan(x[1]) else -1):
            if not np.isnan(value):
                idx = CLASS_NAMES.index(name)
                display = CLASS_DISPLAY_NAMES[idx]
                print(f"  {display:25s}: {value:.4f}")

        # Save metrics to JSON
        metrics_dict = {
            'overall_accuracy': overall_accuracy,
            'mean_iou': mean_iou,
            'per_class_iou': {k: v for k, v in iou_dict.items() if not np.isnan(v)},
            'num_images': len(pred_masks_all),
        }
        metrics_path = output_dir / 'metrics.json'
        with open(metrics_path, 'w') as f:
            json.dump(metrics_dict, f, indent=2)
        print(f"\nSaved metrics: {metrics_path}")

    elif args.metrics and len(pred_masks_all) == 0:
        print("\nWarning: --metrics specified but no ground truth masks found.")
        print("Use --masks <directory> to specify ground truth mask directory.")

    print(f"\nResults saved to: {output_dir}")


if __name__ == '__main__':
    main()
