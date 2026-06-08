#!/usr/bin/env python3
"""
Export a trained 7-class PyTorch AstroUNet model to ONNX format.

Loads best_model_7class.pth, exports to ONNX with:
  - Input:  [batch, 3, 512, 512]  (RGB, no B-R channel)
  - Output: [batch, 7, 512, 512]  (7 class logits)
  - Dynamic batch axis
  - opset_version = 17

After export the script verifies that ONNX Runtime produces the same
predictions as the original PyTorch model.

Usage:
    python export_onnx_7class.py --model models/best_model_7class.pth --output nukex_7class.onnx
    python export_onnx_7class.py --model models/best_model_7class.pth --output nukex_7class.onnx --benchmark
"""

import argparse
import logging
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Model constants (must match train_7class.py)
# ---------------------------------------------------------------------------

NUM_CLASSES = 7
IN_CHANNELS = 3  # RGB only
BASE_FEATURES = 32
IMAGE_SIZE = 512

CLASS_NAMES = [
    "Background",
    "BrightCompact",
    "FaintCompact",
    "BrightExtended",
    "DarkExtended",
    "Artifact",
    "StarHalo",
]


# ---------------------------------------------------------------------------
# Model architecture (must match train_7class.py exactly)
# ---------------------------------------------------------------------------

class DoubleConv(nn.Module):
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
    def __init__(self, in_ch, out_ch):
        super().__init__()
        self.pool_conv = nn.Sequential(nn.MaxPool2d(2), DoubleConv(in_ch, out_ch))

    def forward(self, x):
        return self.pool_conv(x)


class Up(nn.Module):
    def __init__(self, in_ch, out_ch):
        super().__init__()
        self.up = nn.Upsample(scale_factor=2, mode="bilinear", align_corners=True)
        self.conv = DoubleConv(in_ch, out_ch)

    def forward(self, x1, x2):
        x1 = self.up(x1)
        dy = x2.size(2) - x1.size(2)
        dx = x2.size(3) - x1.size(3)
        x1 = nn.functional.pad(x1, [dx // 2, dx - dx // 2, dy // 2, dy - dy // 2])
        return self.conv(torch.cat([x2, x1], dim=1))


class AstroUNet(nn.Module):
    def __init__(self, in_channels=3, num_classes=7, base_features=32):
        super().__init__()
        bf = base_features
        self.inc = DoubleConv(in_channels, bf)
        self.down1 = Down(bf, bf * 2)
        self.down2 = Down(bf * 2, bf * 4)
        self.down3 = Down(bf * 4, bf * 8)
        self.down4 = Down(bf * 8, bf * 8)
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


def count_parameters(model):
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


# ---------------------------------------------------------------------------
# Load model
# ---------------------------------------------------------------------------

def load_model(model_path: str, device: torch.device) -> nn.Module:
    """
    Load a trained 7-class AstroUNet from a checkpoint file.

    Handles both full checkpoint dicts and raw state_dicts.
    """
    logger.info("Loading model from: %s", model_path)

    model = AstroUNet(
        in_channels=IN_CHANNELS,
        num_classes=NUM_CLASSES,
        base_features=BASE_FEATURES,
    )

    checkpoint = torch.load(model_path, map_location=device, weights_only=False)

    if isinstance(checkpoint, dict) and "model_state_dict" in checkpoint:
        state_dict = checkpoint["model_state_dict"]
        epoch = checkpoint.get("epoch", "?")
        val_loss = checkpoint.get("val_loss", "?")
        val_acc = checkpoint.get("val_acc", "?")
        logger.info("  Checkpoint epoch: %s", epoch)
        logger.info("  Validation loss:  %s", val_loss)
        logger.info("  Validation acc:   %s", val_acc)

        # Print per-class IoU if available
        class_iou = checkpoint.get("class_iou", {})
        if class_iou:
            logger.info("  Per-class IoU:")
            for name, iou in sorted(class_iou.items(), key=lambda x: -x[1]):
                logger.info("    %-18s %.4f", name, iou)
    elif isinstance(checkpoint, dict):
        state_dict = checkpoint
    else:
        state_dict = checkpoint

    model.load_state_dict(state_dict)
    model = model.to(device)
    model.eval()

    logger.info("  Parameters: %s", f"{count_parameters(model):,}")
    logger.info("  Input:  [batch, %d, %d, %d]", IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE)
    logger.info("  Output: [batch, %d, %d, %d]", NUM_CLASSES, IMAGE_SIZE, IMAGE_SIZE)

    return model


# ---------------------------------------------------------------------------
# ONNX export
# ---------------------------------------------------------------------------

def export_to_onnx(
    model: nn.Module,
    output_path: str,
    opset_version: int = 17,
    device: torch.device = torch.device("cpu"),
) -> None:
    """Export the model to ONNX format with dynamic batch axis."""
    logger.info("Exporting to ONNX...")
    logger.info("  Output: %s", output_path)
    logger.info("  Opset:  %d", opset_version)

    dummy = torch.randn(1, IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE, device=device)

    torch.onnx.export(
        model,
        dummy,
        output_path,
        export_params=True,
        opset_version=opset_version,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes={
            "input": {0: "batch_size"},
            "output": {0: "batch_size"},
        },
        verbose=False,
        dynamo=False,  # Legacy exporter for maximum C++ compatibility
    )

    file_mb = Path(output_path).stat().st_size / (1024 * 1024)
    logger.info("  Export complete (%.2f MB)", file_mb)


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

def verify_basic(model_path: str) -> bool:
    """Structure-only verification using the onnx library (no runtime needed)."""
    try:
        import onnx
    except ImportError:
        logger.warning("onnx package not installed; skipping basic verification")
        return False

    logger.info("Running basic ONNX structure check...")
    onnx_model = onnx.load(model_path)
    onnx.checker.check_model(onnx_model)

    graph = onnx_model.graph

    def dims(tensor_info):
        return [
            d.dim_value if d.dim_value else d.dim_param
            for d in tensor_info.type.tensor_type.shape.dim
        ]

    inp = graph.input[0]
    out = graph.output[0]
    inp_dims = dims(inp)
    out_dims = dims(out)

    logger.info("  Input  '%s': %s", inp.name, inp_dims)
    logger.info("  Output '%s': %s", out.name, out_dims)

    # Verify non-batch dimensions
    ok = True
    expected_in = [IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE]
    expected_out = [NUM_CLASSES, IMAGE_SIZE, IMAGE_SIZE]
    if len(inp_dims) == 4 and inp_dims[1:] != expected_in:
        logger.error("Input shape mismatch: got %s, expected [batch, %s]", inp_dims, expected_in)
        ok = False
    if len(out_dims) == 4 and out_dims[1:] != expected_out:
        logger.error("Output shape mismatch: got %s, expected [batch, %s]", out_dims, expected_out)
        ok = False

    if ok:
        logger.info("  Basic verification PASSED")
    return ok


def verify_full(
    onnx_path: str,
    pytorch_model: nn.Module,
    device: torch.device,
) -> bool:
    """
    Full numerical verification: compare PyTorch and ONNX Runtime outputs.

    Falls back to basic verification if onnxruntime is not installed.
    """
    try:
        import onnx
        import onnxruntime as ort
    except ImportError as e:
        logger.warning("Full verification unavailable (%s); using basic check", e)
        return verify_basic(onnx_path)

    logger.info("Running full ONNX verification (PyTorch vs ONNX Runtime)...")

    # Basic structure check first
    onnx_model = onnx.load(onnx_path)
    onnx.checker.check_model(onnx_model)

    # Create ORT session
    providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    session = ort.InferenceSession(onnx_path, providers=providers)
    logger.info("  ORT providers: %s", session.get_providers())

    # Random test input
    test_input = torch.randn(1, IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE, device=device)
    test_np = test_input.cpu().numpy()

    # PyTorch forward
    with torch.no_grad():
        pt_out = pytorch_model(test_input).cpu().numpy()

    # ORT forward
    ort_out = session.run(None, {session.get_inputs()[0].name: test_np})[0]

    logger.info("  PyTorch output shape: %s", pt_out.shape)
    logger.info("  ONNX    output shape: %s", ort_out.shape)

    if pt_out.shape != ort_out.shape:
        logger.error("Output shapes differ!")
        return False

    max_diff = np.max(np.abs(pt_out - ort_out))
    mean_diff = np.mean(np.abs(pt_out - ort_out))
    logger.info("  Max  absolute diff: %.6e", max_diff)
    logger.info("  Mean absolute diff: %.6e", mean_diff)

    # Check prediction agreement
    pt_preds = np.argmax(pt_out, axis=1)
    ort_preds = np.argmax(ort_out, axis=1)
    match_rate = np.mean(pt_preds == ort_preds)
    logger.info("  Prediction match rate: %.4f%%", match_rate * 100)

    tolerance = 1e-4
    if max_diff < tolerance:
        logger.info("  Verification PASSED (max diff < %.0e)", tolerance)
        return True

    if match_rate > 0.99:
        logger.info(
            "  Verification PASSED (predictions match despite numerical diff)"
        )
        return True

    logger.error("  Verification FAILED")
    return False


# ---------------------------------------------------------------------------
# Optional benchmark
# ---------------------------------------------------------------------------

def benchmark(onnx_path: str, num_runs: int = 100) -> None:
    try:
        import onnxruntime as ort
    except ImportError:
        logger.warning("onnxruntime not installed; skipping benchmark")
        return

    logger.info("Benchmarking ONNX Runtime inference (%d runs)...", num_runs)

    providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    session = ort.InferenceSession(onnx_path, providers=providers)
    logger.info("  Provider: %s", session.get_providers()[0])

    dummy = np.random.randn(1, IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE).astype(np.float32)

    # Warmup
    for _ in range(10):
        session.run(None, {"input": dummy})

    t0 = time.perf_counter()
    for _ in range(num_runs):
        session.run(None, {"input": dummy})
    elapsed = time.perf_counter() - t0

    avg_ms = elapsed / num_runs * 1000
    fps = num_runs / elapsed
    logger.info("  Avg per image: %.2f ms (%.1f images/sec)", avg_ms, fps)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Export 7-class AstroUNet to ONNX for C++ deployment"
    )
    parser.add_argument(
        "--model",
        "-m",
        type=str,
        required=True,
        help="Path to trained .pth checkpoint (e.g. best_model_7class.pth)",
    )
    parser.add_argument(
        "--output",
        "-o",
        type=str,
        default="nukex_7class.onnx",
        help="Output ONNX file path (default: nukex_7class.onnx)",
    )
    parser.add_argument(
        "--opset",
        type=int,
        default=17,
        help="ONNX opset version (default: 17)",
    )
    parser.add_argument(
        "--no-verify",
        action="store_true",
        help="Skip post-export verification",
    )
    parser.add_argument(
        "--benchmark",
        action="store_true",
        help="Run inference benchmark after export",
    )
    parser.add_argument(
        "--cpu",
        action="store_true",
        help="Force CPU for export even if CUDA is available",
    )
    parser.add_argument(
        "--simplify",
        action="store_true",
        help="Simplify ONNX graph (requires onnx-simplifier)",
    )
    parser.add_argument(
        "--fp16",
        action="store_true",
        help="Also produce FP16 variant (requires onnxconverter-common)",
    )
    args = parser.parse_args()

    # Validate input
    if not Path(args.model).exists():
        sys.exit(f"ERROR: Model file not found: {args.model}")

    # Device
    if args.cpu or not torch.cuda.is_available():
        device = torch.device("cpu")
    else:
        device = torch.device("cuda")
    logger.info("Device: %s", device)

    # Load model
    model = load_model(args.model, device)

    # Export
    export_to_onnx(model, args.output, args.opset, device)

    # Optional simplification
    if args.simplify:
        try:
            import onnx
            from onnxsim import simplify as onnx_simplify

            logger.info("Simplifying ONNX graph...")
            onnx_model = onnx.load(args.output)
            simplified, ok = onnx_simplify(onnx_model)
            if ok:
                simp_path = args.output.replace(".onnx", "_simplified.onnx")
                onnx.save(simplified, simp_path)
                orig_mb = Path(args.output).stat().st_size / (1024 * 1024)
                simp_mb = Path(simp_path).stat().st_size / (1024 * 1024)
                logger.info("  Original: %.2f MB -> Simplified: %.2f MB", orig_mb, simp_mb)
            else:
                logger.warning("Simplification check failed; keeping original")
        except ImportError:
            logger.warning("onnx-simplifier not installed; skipping")

    # Optional FP16
    if args.fp16:
        try:
            import onnx
            from onnxconverter_common import float16

            logger.info("Converting to FP16...")
            onnx_model = onnx.load(args.output)
            fp16_model = float16.convert_float_to_float16(onnx_model)
            fp16_path = args.output.replace(".onnx", "_fp16.onnx")
            onnx.save(fp16_model, fp16_path)
            fp16_mb = Path(fp16_path).stat().st_size / (1024 * 1024)
            logger.info("  FP16 model: %.2f MB -> %s", fp16_mb, fp16_path)
        except ImportError:
            logger.warning("onnxconverter-common not installed; skipping FP16")

    # Verify
    if not args.no_verify:
        verify_full(args.output, model, device)

    # Benchmark
    if args.benchmark:
        benchmark(args.output)

    logger.info("")
    logger.info("Export complete: %s", args.output)
    logger.info("")
    logger.info("To use in C++ with ONNX Runtime:")
    logger.info('  1. Load: Ort::Session session(env, "%s", opts);', args.output)
    logger.info('  2. Input tensor "input":  [batch, %d, %d, %d]', IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE)
    logger.info('  3. Output tensor "output": [batch, %d, %d, %d]', NUM_CLASSES, IMAGE_SIZE, IMAGE_SIZE)


if __name__ == "__main__":
    main()
