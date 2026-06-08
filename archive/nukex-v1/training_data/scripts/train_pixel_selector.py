#!/usr/bin/env python3
"""
Train the ML Pixel Selector model for NukeX intelligent stacking.

This model learns to select the optimal pixel value from a stack of N aligned
subframes, replacing the hand-crafted heuristics in PixelStackAnalyzer::SelectBestValue.

Architecture: SetTransformer-lite
  - Global features (distribution stats + segmentation + stack summary) are
    projected to an embedding space.
  - Per-frame candidate features (value, z-score, weight) are independently
    embedded, then conditioned on global context via cross-attention.
  - A per-frame scoring head outputs softmax-normalized frame selection scores.

See docs/plans/2026-02-19-ml-pixel-selector-design.md for full design context.

Usage:
    # Training
    python train_pixel_selector.py \
        --data-dir /path/to/training_data \
        --output-dir /path/to/models \
        --epochs 50 --batch-size 4096

    # ONNX export only (from checkpoint)
    python train_pixel_selector.py \
        --export-only --checkpoint /path/to/best_model.pt \
        --output-dir /path/to/models
"""

import argparse
import logging
import os
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader, random_split

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants (must match generate_selection_training_data.py)
# ---------------------------------------------------------------------------

N_MAX = 64              # Maximum number of frames
GLOBAL_FEATURE_DIM = 28 # Distribution(10) + Segmentation(12) + Summary(6)
PER_FRAME_DIM = 3       # value, z_score, frame_weight
TOTAL_FEATURE_DIM = GLOBAL_FEATURE_DIM + N_MAX * PER_FRAME_DIM + N_MAX  # 284


# ---------------------------------------------------------------------------
# Dataset
# ---------------------------------------------------------------------------

class PixelSelectionDataset(Dataset):
    """Dataset of (feature_vector, soft_label) pairs for pixel selection training.

    Expects numpy files produced by generate_selection_training_data.py:
      - features.npy: float32 (num_samples, 284)
      - labels.npy:   float32 (num_samples, 64) -- soft label distributions
    """

    def __init__(self, data_dir: str):
        features_path = os.path.join(data_dir, "features.npy")
        labels_path = os.path.join(data_dir, "labels.npy")

        if not os.path.exists(features_path):
            raise FileNotFoundError(f"Features not found: {features_path}")
        if not os.path.exists(labels_path):
            raise FileNotFoundError(f"Labels not found: {labels_path}")

        logger.info(f"Loading features from {features_path}")
        self.features = np.load(features_path)
        logger.info(f"Loading labels from {labels_path}")
        self.labels = np.load(labels_path)

        assert self.features.shape[0] == self.labels.shape[0], \
            f"Feature/label count mismatch: {self.features.shape[0]} vs {self.labels.shape[0]}"
        assert self.features.shape[1] == TOTAL_FEATURE_DIM, \
            f"Feature dim mismatch: {self.features.shape[1]} vs {TOTAL_FEATURE_DIM}"
        assert self.labels.shape[1] == N_MAX, \
            f"Label dim mismatch: {self.labels.shape[1]} vs {N_MAX}"

        logger.info(f"Loaded {len(self)} samples")

    def __len__(self):
        return self.features.shape[0]

    def __getitem__(self, idx):
        features = torch.from_numpy(self.features[idx])
        label = torch.from_numpy(self.labels[idx])
        return features, label


# ---------------------------------------------------------------------------
# Model: SetTransformer-lite for Pixel Selection
# ---------------------------------------------------------------------------

class PixelSelectorNet(nn.Module):
    """SetTransformer-lite architecture for per-pixel frame selection.

    The model takes:
      - global_features: (B, 28) distribution + segmentation + summary stats
      - frame_features:  (B, N_MAX, 3) per-frame candidate features
      - frame_mask:      (B, N_MAX) binary mask (1 = valid frame, 0 = padding)

    And outputs:
      - frame_scores: (B, N_MAX) softmax-normalized selection scores

    Total parameters: ~16,500 (designed for sub-microsecond batched inference)
    """

    def __init__(self, embed_dim: int = 64, dropout: float = 0.1):
        super().__init__()

        self.embed_dim = embed_dim

        # Global feature encoder
        self.global_encoder = nn.Sequential(
            nn.Linear(GLOBAL_FEATURE_DIM, embed_dim),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout),
        )

        # Per-frame feature encoder (shared across frames)
        self.frame_encoder = nn.Sequential(
            nn.Linear(PER_FRAME_DIM, embed_dim),
            nn.ReLU(inplace=True),
        )

        # Cross-attention: frames attend to global context
        # Single-head attention for minimal parameter count
        self.q_proj = nn.Linear(embed_dim, embed_dim, bias=False)
        self.k_proj = nn.Linear(embed_dim, embed_dim, bias=False)
        self.v_proj = nn.Linear(embed_dim, embed_dim, bias=False)
        self.attn_scale = embed_dim ** -0.5

        # Layer norm after attention
        self.attn_norm = nn.LayerNorm(embed_dim)

        # Per-frame scoring head
        self.score_head = nn.Sequential(
            nn.Linear(embed_dim, embed_dim // 2),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout),
            nn.Linear(embed_dim // 2, 1),
        )

    def forward(
        self,
        global_features: torch.Tensor,
        frame_features: torch.Tensor,
        frame_mask: torch.Tensor,
    ) -> torch.Tensor:
        """
        Args:
            global_features: (B, 28) global context features
            frame_features:  (B, N_MAX, 3) per-frame candidate features
            frame_mask:      (B, N_MAX) binary mask (1=valid, 0=padding)

        Returns:
            frame_scores: (B, N_MAX) masked softmax scores
        """
        B = global_features.shape[0]

        # Encode global features -> (B, embed_dim)
        global_embed = self.global_encoder(global_features)

        # Encode per-frame features -> (B, N_MAX, embed_dim)
        frame_embed = self.frame_encoder(frame_features)

        # Cross-attention: each frame attends to global context
        # Q from frame embeddings, K/V from global embedding (tiled)
        Q = self.q_proj(frame_embed)                     # (B, N_MAX, D)
        K = self.k_proj(global_embed).unsqueeze(1)       # (B, 1, D)
        V = self.v_proj(global_embed).unsqueeze(1)       # (B, 1, D)

        # Attention scores: (B, N_MAX, 1)
        attn_scores = torch.bmm(Q, K.transpose(1, 2)) * self.attn_scale
        attn_weights = torch.softmax(attn_scores, dim=-1)

        # Attended values: (B, N_MAX, D)
        attn_output = torch.bmm(attn_weights, V)

        # Residual connection + layer norm
        conditioned = self.attn_norm(frame_embed + attn_output)

        # Per-frame score: (B, N_MAX, 1) -> (B, N_MAX)
        scores = self.score_head(conditioned).squeeze(-1)

        # Mask invalid frames with large negative value before softmax
        mask_value = torch.tensor(-1e9, device=scores.device, dtype=scores.dtype)
        scores = torch.where(frame_mask.bool(), scores, mask_value)

        # Softmax over valid frames
        frame_scores = torch.softmax(scores, dim=-1)

        # Zero out padding positions (for clean gradients)
        frame_scores = frame_scores * frame_mask

        return frame_scores

    @staticmethod
    def split_features(features: torch.Tensor):
        """Split a flat feature vector into the three model inputs.

        Args:
            features: (B, 284) flat feature vector from data generator

        Returns:
            global_features: (B, 28)
            frame_features:  (B, N_MAX, 3)
            frame_mask:      (B, N_MAX)
        """
        global_features = features[:, :GLOBAL_FEATURE_DIM]

        frame_flat = features[:, GLOBAL_FEATURE_DIM:GLOBAL_FEATURE_DIM + N_MAX * PER_FRAME_DIM]
        frame_features = frame_flat.reshape(-1, N_MAX, PER_FRAME_DIM)

        frame_mask = features[:, GLOBAL_FEATURE_DIM + N_MAX * PER_FRAME_DIM:]

        return global_features, frame_features, frame_mask


# ---------------------------------------------------------------------------
# MLP Fallback Model (simpler, potentially faster)
# ---------------------------------------------------------------------------

class PixelSelectorMLP(nn.Module):
    """Pure MLP fallback for pixel selection.

    Candidates are sorted by value before input, making the MLP effectively
    permutation-invariant. This is the backup if the attention model is too slow.

    Input: [global_features(28), sorted_frame_features(N_MAX*3), frame_mask(N_MAX)] = 284
    Output: frame_scores (N_MAX)
    """

    def __init__(self, dropout: float = 0.1):
        super().__init__()

        self.net = nn.Sequential(
            nn.Linear(TOTAL_FEATURE_DIM, 128),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout),
            nn.Linear(128, 64),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout),
            nn.Linear(64, N_MAX),
        )

    def forward(
        self,
        global_features: torch.Tensor,
        frame_features: torch.Tensor,
        frame_mask: torch.Tensor,
    ) -> torch.Tensor:
        # Flatten and concatenate
        B = global_features.shape[0]
        frame_flat = frame_features.reshape(B, -1)
        x = torch.cat([global_features, frame_flat, frame_mask], dim=-1)

        scores = self.net(x)

        # Mask and softmax
        mask_value = torch.tensor(-1e9, device=scores.device, dtype=scores.dtype)
        scores = torch.where(frame_mask.bool(), scores, mask_value)
        frame_scores = torch.softmax(scores, dim=-1)
        frame_scores = frame_scores * frame_mask

        return frame_scores

    @staticmethod
    def split_features(features: torch.Tensor):
        """Same interface as PixelSelectorNet."""
        return PixelSelectorNet.split_features(features)


# ---------------------------------------------------------------------------
# Training utilities
# ---------------------------------------------------------------------------

def compute_metrics(
    predicted_scores: torch.Tensor,
    soft_labels: torch.Tensor,
    frame_mask: torch.Tensor,
) -> dict:
    """Compute training and validation metrics.

    Args:
        predicted_scores: (B, N_MAX) model output
        soft_labels: (B, N_MAX) ground truth soft labels
        frame_mask: (B, N_MAX) valid frame mask

    Returns:
        Dict of metric name -> value
    """
    with torch.no_grad():
        # Hard label = argmax of soft label
        hard_labels = soft_labels.argmax(dim=-1)  # (B,)

        # Predicted frame = argmax of predicted scores
        predicted_frames = predicted_scores.argmax(dim=-1)  # (B,)

        # Frame selection accuracy
        accuracy = (predicted_frames == hard_labels).float().mean().item()

        # Top-3 accuracy
        _, top3_idx = predicted_scores.topk(3, dim=-1)
        top3_match = (top3_idx == hard_labels.unsqueeze(-1)).any(dim=-1)
        top3_accuracy = top3_match.float().mean().item()

        # KL divergence (our training loss)
        log_pred = torch.log(predicted_scores + 1e-10)
        kl_div = F.kl_div(log_pred, soft_labels, reduction="batchmean").item()

        return {
            "accuracy": accuracy,
            "top3_accuracy": top3_accuracy,
            "kl_divergence": kl_div,
        }


# ---------------------------------------------------------------------------
# Training loop
# ---------------------------------------------------------------------------

def train_epoch(
    model: nn.Module,
    dataloader: DataLoader,
    optimizer: optim.Optimizer,
    device: torch.device,
    epoch: int,
) -> dict:
    """Train for one epoch.

    Returns dict of average metrics.
    """
    model.train()
    total_loss = 0.0
    total_accuracy = 0.0
    total_top3 = 0.0
    num_batches = 0

    for features, labels in dataloader:
        features = features.to(device)
        labels = labels.to(device)

        # Split feature vector into model inputs
        global_feat, frame_feat, frame_mask = model.split_features(features)

        # Forward pass
        predicted_scores = model(global_feat, frame_feat, frame_mask)

        # KL divergence loss
        log_pred = torch.log(predicted_scores + 1e-10)
        loss = F.kl_div(log_pred, labels, reduction="batchmean")

        # Backward pass
        optimizer.zero_grad()
        loss.backward()

        # Gradient clipping
        torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)

        optimizer.step()

        # Metrics
        metrics = compute_metrics(predicted_scores, labels, frame_mask)
        total_loss += loss.item()
        total_accuracy += metrics["accuracy"]
        total_top3 += metrics["top3_accuracy"]
        num_batches += 1

    return {
        "loss": total_loss / max(num_batches, 1),
        "accuracy": total_accuracy / max(num_batches, 1),
        "top3_accuracy": total_top3 / max(num_batches, 1),
    }


@torch.no_grad()
def validate(
    model: nn.Module,
    dataloader: DataLoader,
    device: torch.device,
) -> dict:
    """Validate model on a dataset.

    Returns dict of average metrics.
    """
    model.eval()
    total_loss = 0.0
    total_accuracy = 0.0
    total_top3 = 0.0
    num_batches = 0

    for features, labels in dataloader:
        features = features.to(device)
        labels = labels.to(device)

        global_feat, frame_feat, frame_mask = model.split_features(features)
        predicted_scores = model(global_feat, frame_feat, frame_mask)

        log_pred = torch.log(predicted_scores + 1e-10)
        loss = F.kl_div(log_pred, labels, reduction="batchmean")

        metrics = compute_metrics(predicted_scores, labels, frame_mask)
        total_loss += loss.item()
        total_accuracy += metrics["accuracy"]
        total_top3 += metrics["top3_accuracy"]
        num_batches += 1

    return {
        "loss": total_loss / max(num_batches, 1),
        "accuracy": total_accuracy / max(num_batches, 1),
        "top3_accuracy": total_top3 / max(num_batches, 1),
    }


def train(
    data_dir: str,
    output_dir: str,
    model_type: str = "attention",
    epochs: int = 50,
    batch_size: int = 4096,
    learning_rate: float = 1e-3,
    weight_decay: float = 1e-4,
    patience: int = 10,
    seed: int = 42,
):
    """Full training pipeline.

    Args:
        data_dir: Directory containing features.npy and labels.npy
        output_dir: Where to save checkpoints and final model
        model_type: "attention" (SetTransformer-lite) or "mlp" (fallback)
        epochs: Maximum training epochs
        batch_size: Training batch size
        learning_rate: Initial learning rate
        weight_decay: AdamW weight decay
        patience: Early stopping patience
        seed: Random seed
    """
    os.makedirs(output_dir, exist_ok=True)

    torch.manual_seed(seed)
    np.random.seed(seed)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    logger.info(f"Device: {device}")

    # Load dataset
    dataset = PixelSelectionDataset(data_dir)

    # Split: 80/10/10
    n = len(dataset)
    n_train = int(n * 0.8)
    n_val = int(n * 0.1)
    n_test = n - n_train - n_val
    train_set, val_set, test_set = random_split(
        dataset, [n_train, n_val, n_test],
        generator=torch.Generator().manual_seed(seed),
    )
    logger.info(f"Split: train={n_train}, val={n_val}, test={n_test}")

    train_loader = DataLoader(
        train_set, batch_size=batch_size, shuffle=True,
        num_workers=4, pin_memory=True, drop_last=True,
    )
    val_loader = DataLoader(
        val_set, batch_size=batch_size, shuffle=False,
        num_workers=2, pin_memory=True,
    )
    test_loader = DataLoader(
        test_set, batch_size=batch_size, shuffle=False,
        num_workers=2, pin_memory=True,
    )

    # Create model
    if model_type == "attention":
        model = PixelSelectorNet(embed_dim=64, dropout=0.1)
    elif model_type == "mlp":
        model = PixelSelectorMLP(dropout=0.1)
    else:
        raise ValueError(f"Unknown model type: {model_type}")

    model = model.to(device)

    num_params = sum(p.numel() for p in model.parameters() if p.requires_grad)
    logger.info(f"Model: {model_type}, {num_params:,} trainable parameters")

    # Optimizer and scheduler
    optimizer = optim.AdamW(
        model.parameters(), lr=learning_rate, weight_decay=weight_decay,
    )
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)

    # Training loop
    best_val_loss = float("inf")
    epochs_no_improve = 0
    best_checkpoint_path = os.path.join(output_dir, "best_model.pt")

    for epoch in range(1, epochs + 1):
        t0 = time.time()

        train_metrics = train_epoch(model, train_loader, optimizer, device, epoch)
        val_metrics = validate(model, val_loader, device)

        scheduler.step()

        elapsed = time.time() - t0
        lr = scheduler.get_last_lr()[0]

        logger.info(
            f"Epoch {epoch:3d}/{epochs} ({elapsed:.1f}s) | "
            f"lr={lr:.2e} | "
            f"train loss={train_metrics['loss']:.4f} acc={train_metrics['accuracy']:.3f} "
            f"top3={train_metrics['top3_accuracy']:.3f} | "
            f"val loss={val_metrics['loss']:.4f} acc={val_metrics['accuracy']:.3f} "
            f"top3={val_metrics['top3_accuracy']:.3f}"
        )

        # Early stopping check
        if val_metrics["loss"] < best_val_loss:
            best_val_loss = val_metrics["loss"]
            epochs_no_improve = 0
            torch.save({
                "epoch": epoch,
                "model_state_dict": model.state_dict(),
                "optimizer_state_dict": optimizer.state_dict(),
                "val_loss": best_val_loss,
                "val_accuracy": val_metrics["accuracy"],
                "model_type": model_type,
                "num_params": num_params,
            }, best_checkpoint_path)
            logger.info(f"  -> Saved best model (val_loss={best_val_loss:.4f})")
        else:
            epochs_no_improve += 1
            if epochs_no_improve >= patience:
                logger.info(f"Early stopping after {epoch} epochs (patience={patience})")
                break

    # Load best model and run on test set
    logger.info("Loading best model for test set scoring...")
    checkpoint = torch.load(best_checkpoint_path, map_location=device, weights_only=True)
    model.load_state_dict(checkpoint["model_state_dict"])

    test_metrics = validate(model, test_loader, device)
    logger.info(
        f"Test results: loss={test_metrics['loss']:.4f} "
        f"acc={test_metrics['accuracy']:.3f} "
        f"top3={test_metrics['top3_accuracy']:.3f}"
    )

    # Export to ONNX
    onnx_path = os.path.join(output_dir, "pixel_selector.onnx")
    export_to_onnx(model, onnx_path, device)

    logger.info("Training complete.")
    return test_metrics


# ---------------------------------------------------------------------------
# ONNX Export
# ---------------------------------------------------------------------------

def export_to_onnx(
    model: nn.Module,
    output_path: str,
    device: torch.device = torch.device("cpu"),
):
    """Export the trained model to ONNX format for C++ inference.

    Uses dynamic batch dimension for row-level batching in the C++ pipeline.
    The model expects batched input where batch_size = image_width
    (one row of pixels at a time).

    Args:
        model: Trained PixelSelectorNet or PixelSelectorMLP
        output_path: Path to save .onnx file
        device: Device for dummy input generation
    """
    model = model.to(device)
    model.train(False)

    # Dummy inputs for tracing (batch_size=1 for export, ORT handles batching)
    dummy_global = torch.randn(1, GLOBAL_FEATURE_DIM, device=device)
    dummy_frames = torch.randn(1, N_MAX, PER_FRAME_DIM, device=device)
    dummy_mask = torch.ones(1, N_MAX, device=device)

    # Export
    logger.info(f"Exporting ONNX model to {output_path}")
    torch.onnx.export(
        model,
        (dummy_global, dummy_frames, dummy_mask),
        output_path,
        input_names=["global_features", "frame_features", "frame_mask"],
        output_names=["frame_scores"],
        dynamic_axes={
            "global_features": {0: "batch_size"},
            "frame_features": {0: "batch_size"},
            "frame_mask": {0: "batch_size"},
            "frame_scores": {0: "batch_size"},
        },
        opset_version=17,
        do_constant_folding=True,
    )

    # Verify the exported model
    try:
        import onnx
        onnx_model = onnx.load(output_path)
        onnx.checker.check_model(onnx_model)
        logger.info("ONNX model verification passed")
    except ImportError:
        logger.warning("onnx package not installed, skipping verification")
    except Exception as e:
        logger.error(f"ONNX verification failed: {e}")

    # Report file size
    size_bytes = os.path.getsize(output_path)
    logger.info(f"ONNX model size: {size_bytes / 1024:.1f} KB")

    # Optional: run a test inference with ONNX Runtime
    try:
        import onnxruntime as ort

        session = ort.InferenceSession(output_path)

        # Test with batch_size=4096 (typical row width)
        test_global = np.random.randn(4096, GLOBAL_FEATURE_DIM).astype(np.float32)
        test_frames = np.random.randn(4096, N_MAX, PER_FRAME_DIM).astype(np.float32)
        test_mask = np.ones((4096, N_MAX), dtype=np.float32)

        t0 = time.time()
        for _ in range(10):
            outputs = session.run(None, {
                "global_features": test_global,
                "frame_features": test_frames,
                "frame_mask": test_mask,
            })
        elapsed = (time.time() - t0) / 10.0

        scores = outputs[0]
        logger.info(
            f"ONNX Runtime test: batch=4096, "
            f"output shape={scores.shape}, "
            f"time={elapsed*1000:.1f}ms ({elapsed/4096*1e6:.2f} us/pixel)"
        )
    except ImportError:
        logger.warning("onnxruntime not installed, skipping inference test")
    except Exception as e:
        logger.error(f"ONNX Runtime test failed: {e}")


def export_from_checkpoint(checkpoint_path: str, output_dir: str):
    """Load a saved checkpoint and export to ONNX.

    Args:
        checkpoint_path: Path to .pt checkpoint file
        output_dir: Directory to save ONNX model
    """
    os.makedirs(output_dir, exist_ok=True)

    device = torch.device("cpu")
    checkpoint = torch.load(checkpoint_path, map_location=device, weights_only=True)

    model_type = checkpoint.get("model_type", "attention")
    if model_type == "attention":
        model = PixelSelectorNet(embed_dim=64, dropout=0.0)
    elif model_type == "mlp":
        model = PixelSelectorMLP(dropout=0.0)
    else:
        raise ValueError(f"Unknown model type in checkpoint: {model_type}")

    model.load_state_dict(checkpoint["model_state_dict"])
    logger.info(
        f"Loaded {model_type} model from epoch {checkpoint.get('epoch', '?')}, "
        f"val_loss={checkpoint.get('val_loss', 0):.4f}"
    )

    onnx_path = os.path.join(output_dir, "pixel_selector.onnx")
    export_to_onnx(model, onnx_path, device)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description="Train ML Pixel Selector model for NukeX",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument(
        "--data-dir", default=None,
        help="Directory containing features.npy and labels.npy (required for training)",
    )
    parser.add_argument(
        "--output-dir", required=True,
        help="Output directory for checkpoints and ONNX model",
    )
    parser.add_argument(
        "--model-type", choices=["attention", "mlp"], default="attention",
        help="Model architecture: 'attention' (SetTransformer-lite) or 'mlp' (fallback)",
    )
    parser.add_argument(
        "--epochs", type=int, default=50,
        help="Maximum training epochs",
    )
    parser.add_argument(
        "--batch-size", type=int, default=4096,
        help="Training batch size (each sample is one pixel, so large batches are fine)",
    )
    parser.add_argument(
        "--lr", type=float, default=1e-3,
        help="Initial learning rate",
    )
    parser.add_argument(
        "--weight-decay", type=float, default=1e-4,
        help="AdamW weight decay",
    )
    parser.add_argument(
        "--patience", type=int, default=10,
        help="Early stopping patience (epochs without val improvement)",
    )
    parser.add_argument(
        "--seed", type=int, default=42,
        help="Random seed",
    )
    parser.add_argument(
        "--export-only", action="store_true",
        help="Skip training, only export ONNX from checkpoint",
    )
    parser.add_argument(
        "--checkpoint", default=None,
        help="Path to checkpoint for --export-only mode",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Enable verbose logging",
    )

    return parser.parse_args()


def main():
    args = parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    if args.export_only:
        if args.checkpoint is None:
            logger.error("--checkpoint is required with --export-only")
            sys.exit(1)
        export_from_checkpoint(args.checkpoint, args.output_dir)
    else:
        if args.data_dir is None:
            logger.error("--data-dir is required for training")
            sys.exit(1)
        train(
            data_dir=args.data_dir,
            output_dir=args.output_dir,
            model_type=args.model_type,
            epochs=args.epochs,
            batch_size=args.batch_size,
            learning_rate=args.lr,
            weight_decay=args.weight_decay,
            patience=args.patience,
            seed=args.seed,
        )


if __name__ == "__main__":
    main()
