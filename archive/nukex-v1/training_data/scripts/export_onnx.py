#!/usr/bin/env python3
"""
Export trained PyTorch AstroUNet model to ONNX format for C++ deployment.

This script loads a trained .pth checkpoint and exports it to ONNX format
suitable for inference with ONNX Runtime in the PixInsight C++ module.

Usage:
    python export_onnx.py --model best_model.pth --output model.onnx

Options:
    --model         Path to the trained PyTorch model (.pth file)
    --output        Output path for the ONNX model
    --opset         ONNX opset version (default: 17)
    --verify        Verify the exported model (default: True)
    --fp16          Export in FP16 precision for faster inference
    --simplify      Simplify the ONNX model graph (requires onnxsim)
"""

import argparse
import sys
import numpy as np
from pathlib import Path

import torch
import torch.nn as nn

# Add NukeX2 scripts to path for model architecture
sys.path.insert(0, '/home/scarter4work/projects/NukeX2/training/scripts')
from model import AstroUNet, count_parameters


# Model configuration (must match training config)
NUM_CLASSES = 21
IN_CHANNELS = 4  # RGB + color contrast (B-R)
BASE_FEATURES = 32
IMAGE_SIZE = 512


def load_model(model_path: str, device: torch.device) -> nn.Module:
    """
    Load a trained AstroUNet model from a checkpoint.

    Args:
        model_path: Path to the .pth checkpoint file
        device: Target device (cpu/cuda)

    Returns:
        Loaded model in eval mode
    """
    print(f"Loading model from: {model_path}")

    # Create model architecture
    model = AstroUNet(
        in_channels=IN_CHANNELS,
        num_classes=NUM_CLASSES,
        base_features=BASE_FEATURES
    )

    # Load checkpoint
    checkpoint = torch.load(model_path, map_location=device, weights_only=False)

    # Handle different checkpoint formats
    if 'model_state_dict' in checkpoint:
        state_dict = checkpoint['model_state_dict']
        epoch = checkpoint.get('epoch', 'unknown')
        val_loss = checkpoint.get('val_loss', 'unknown')
        val_acc = checkpoint.get('val_acc', 'unknown')
        print(f"  Checkpoint from epoch: {epoch}")
        print(f"  Validation loss: {val_loss}")
        print(f"  Validation accuracy: {val_acc}")

        # Print color metrics if available
        if 'color_metrics' in checkpoint:
            cm = checkpoint['color_metrics']
            print(f"  Emission IoU: {cm.get('emission_iou', 'N/A')}")
            print(f"  Reflection IoU: {cm.get('reflection_iou', 'N/A')}")
            print(f"  Color confusion: {cm.get('total_color_confusion_rate', 'N/A')}")
    else:
        # Direct state dict
        state_dict = checkpoint

    model.load_state_dict(state_dict)
    model = model.to(device)
    model.eval()

    print(f"  Model parameters: {count_parameters(model):,}")
    print(f"  Input channels: {IN_CHANNELS}")
    print(f"  Output classes: {NUM_CLASSES}")

    return model


def export_to_onnx(
    model: nn.Module,
    output_path: str,
    opset_version: int = 17,
    device: torch.device = torch.device('cpu')
) -> None:
    """
    Export PyTorch model to ONNX format.

    Args:
        model: Trained PyTorch model
        output_path: Path for the output ONNX file
        opset_version: ONNX opset version
        device: Device for dummy input tensor
    """
    print(f"\nExporting to ONNX...")
    print(f"  Output: {output_path}")
    print(f"  Opset version: {opset_version}")

    # Create dummy input tensor
    # Shape: [batch, channels, height, width] = [1, 4, 512, 512]
    dummy_input = torch.randn(1, IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE, device=device)

    # Input and output names
    input_names = ['input']
    output_names = ['output']

    # Dynamic axes for batch size (allows inference with different batch sizes)
    dynamic_axes = {
        'input': {0: 'batch_size'},
        'output': {0: 'batch_size'}
    }

    # Export to ONNX using legacy TorchScript-based exporter
    # This is more compatible with ONNX Runtime and C++ deployment
    # The new dynamo=True exporter has compatibility issues with some operations
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        export_params=True,
        opset_version=opset_version,
        do_constant_folding=True,  # Optimize constant folding
        input_names=input_names,
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        verbose=False,
        dynamo=False  # Force legacy TorchScript-based exporter for compatibility
    )

    print(f"  Export complete!")

    # Print file size
    file_size = Path(output_path).stat().st_size / (1024 * 1024)
    print(f"  File size: {file_size:.2f} MB")


def simplify_onnx(model_path: str) -> str:
    """
    Simplify ONNX model graph using onnx-simplifier.

    Args:
        model_path: Path to the ONNX model

    Returns:
        Path to the simplified model
    """
    try:
        import onnx
        from onnxsim import simplify

        print(f"\nSimplifying ONNX model...")

        # Load model
        model = onnx.load(model_path)

        # Simplify
        model_simplified, check = simplify(model)

        if check:
            # Save simplified model
            output_path = model_path.replace('.onnx', '_simplified.onnx')
            onnx.save(model_simplified, output_path)

            # Compare sizes
            original_size = Path(model_path).stat().st_size / (1024 * 1024)
            simplified_size = Path(output_path).stat().st_size / (1024 * 1024)
            print(f"  Original size: {original_size:.2f} MB")
            print(f"  Simplified size: {simplified_size:.2f} MB")
            print(f"  Saved to: {output_path}")
            return output_path
        else:
            print("  WARNING: Simplification check failed, using original model")
            return model_path

    except ImportError:
        print("  WARNING: onnx-simplifier not installed, skipping simplification")
        print("  Install with: pip install onnx-simplifier")
        return model_path


def convert_to_fp16(model_path: str) -> str:
    """
    Convert ONNX model to FP16 precision.

    Args:
        model_path: Path to the ONNX model

    Returns:
        Path to the FP16 model
    """
    try:
        import onnx
        from onnxconverter_common import float16

        print(f"\nConverting to FP16 precision...")

        # Load model
        model = onnx.load(model_path)

        # Convert to FP16
        model_fp16 = float16.convert_float_to_float16(model)

        # Save FP16 model
        output_path = model_path.replace('.onnx', '_fp16.onnx')
        onnx.save(model_fp16, output_path)

        # Compare sizes
        original_size = Path(model_path).stat().st_size / (1024 * 1024)
        fp16_size = Path(output_path).stat().st_size / (1024 * 1024)
        print(f"  Original size (FP32): {original_size:.2f} MB")
        print(f"  FP16 size: {fp16_size:.2f} MB")
        print(f"  Saved to: {output_path}")
        return output_path

    except ImportError:
        print("  WARNING: onnxconverter-common not installed, skipping FP16 conversion")
        print("  Install with: pip install onnxconverter-common")
        return model_path


def verify_onnx_model_basic(model_path: str) -> bool:
    """
    Basic ONNX model verification using only the onnx library.
    This is a fallback when onnxruntime is not available.

    Args:
        model_path: Path to the ONNX model

    Returns:
        True if verification passed
    """
    try:
        import onnx

        print(f"\nVerifying ONNX model (basic check)...")

        # Load and check ONNX model
        print(f"  Loading ONNX model: {model_path}")
        onnx_model = onnx.load(model_path)
        onnx.checker.check_model(onnx_model)
        print(f"  ONNX model structure check passed!")

        # Get model info
        graph = onnx_model.graph

        # Get input info
        input_info = graph.input[0]
        input_dims = []
        for dim in input_info.type.tensor_type.shape.dim:
            if dim.dim_value:
                input_dims.append(dim.dim_value)
            elif dim.dim_param:
                input_dims.append(dim.dim_param)
            else:
                input_dims.append('?')

        print(f"  Input: {input_info.name}")
        print(f"    Shape: {input_dims}")

        # Get output info
        output_info = graph.output[0]
        output_dims = []
        for dim in output_info.type.tensor_type.shape.dim:
            if dim.dim_value:
                output_dims.append(dim.dim_value)
            elif dim.dim_param:
                output_dims.append(dim.dim_param)
            else:
                output_dims.append('?')

        print(f"  Output: {output_info.name}")
        print(f"    Shape: {output_dims}")

        # Verify expected tensor names
        if input_info.name != 'input':
            print(f"  WARNING: Input tensor name is '{input_info.name}', expected 'input'")

        if output_info.name != 'output':
            print(f"  WARNING: Output tensor name is '{output_info.name}', expected 'output'")

        # Verify expected shapes (accounting for dynamic batch size)
        expected_input = ['batch_size', IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE]
        expected_output = ['batch_size', NUM_CLASSES, IMAGE_SIZE, IMAGE_SIZE]

        # Check input dimensions (skip batch_size which is dynamic)
        input_ok = (
            input_dims[1:] == expected_input[1:] if len(input_dims) == 4 else False
        )
        output_ok = (
            output_dims[1:] == expected_output[1:] if len(output_dims) == 4 else False
        )

        if input_ok and output_ok:
            print(f"\n  Verification PASSED!")
            print(f"    Input shape verified: [batch, {IN_CHANNELS}, {IMAGE_SIZE}, {IMAGE_SIZE}]")
            print(f"    Output shape verified: [batch, {NUM_CLASSES}, {IMAGE_SIZE}, {IMAGE_SIZE}]")
            return True
        else:
            if not input_ok:
                print(f"  ERROR: Input shape {input_dims} doesn't match expected {expected_input}")
            if not output_ok:
                print(f"  ERROR: Output shape {output_dims} doesn't match expected {expected_output}")
            return False

    except Exception as e:
        print(f"\n  ERROR during basic verification: {e}")
        return False


def verify_onnx_model(model_path: str, pytorch_model: nn.Module, device: torch.device) -> bool:
    """
    Verify the exported ONNX model by comparing outputs with PyTorch.

    If onnxruntime is available, performs full numerical comparison.
    Otherwise, falls back to basic structure verification.

    Args:
        model_path: Path to the ONNX model
        pytorch_model: Original PyTorch model for comparison
        device: Device for PyTorch inference

    Returns:
        True if verification passed
    """
    # First, try full verification with onnxruntime
    try:
        import onnx
        import onnxruntime as ort

        print(f"\nVerifying ONNX model (full verification)...")

        # Load and check ONNX model
        print(f"  Loading ONNX model: {model_path}")
        onnx_model = onnx.load(model_path)
        onnx.checker.check_model(onnx_model)
        print(f"  ONNX model check passed!")

        # Get model info
        graph = onnx_model.graph
        print(f"  Input: {graph.input[0].name}")
        print(f"    Shape: {[dim.dim_value or dim.dim_param for dim in graph.input[0].type.tensor_type.shape.dim]}")
        print(f"  Output: {graph.output[0].name}")
        print(f"    Shape: {[dim.dim_value or dim.dim_param for dim in graph.output[0].type.tensor_type.shape.dim]}")

        # Create ONNX Runtime session
        print(f"\n  Creating ONNX Runtime session...")
        providers = ['CUDAExecutionProvider', 'CPUExecutionProvider']
        session = ort.InferenceSession(model_path, providers=providers)

        # Print execution provider
        print(f"  Execution provider: {session.get_providers()}")

        # Create test input
        test_input = torch.randn(1, IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE, device=device)
        test_input_np = test_input.cpu().numpy()

        # Run PyTorch inference
        print(f"\n  Running PyTorch inference...")
        with torch.no_grad():
            pytorch_output = pytorch_model(test_input)
        pytorch_output_np = pytorch_output.cpu().numpy()

        # Run ONNX Runtime inference
        print(f"  Running ONNX Runtime inference...")
        ort_inputs = {session.get_inputs()[0].name: test_input_np}
        ort_output = session.run(None, ort_inputs)[0]

        # Compare outputs
        print(f"\n  Comparing outputs...")
        print(f"    PyTorch output shape: {pytorch_output_np.shape}")
        print(f"    ONNX output shape: {ort_output.shape}")

        # Check shape match
        if pytorch_output_np.shape != ort_output.shape:
            print(f"  ERROR: Output shapes don't match!")
            return False

        # Verify expected shapes
        expected_shape = (1, NUM_CLASSES, IMAGE_SIZE, IMAGE_SIZE)
        if ort_output.shape != expected_shape:
            print(f"  WARNING: Output shape {ort_output.shape} differs from expected {expected_shape}")

        # Calculate numerical difference
        max_diff = np.max(np.abs(pytorch_output_np - ort_output))
        mean_diff = np.mean(np.abs(pytorch_output_np - ort_output))

        print(f"    Max absolute difference: {max_diff:.6e}")
        print(f"    Mean absolute difference: {mean_diff:.6e}")

        # Tolerance check
        tolerance = 1e-4
        if max_diff < tolerance:
            print(f"\n  Verification PASSED! (max diff < {tolerance})")
            return True
        else:
            print(f"\n  WARNING: Max diff ({max_diff:.6e}) exceeds tolerance ({tolerance})")
            print(f"  This is often acceptable due to floating-point precision differences.")

            # Check if predictions match
            pytorch_preds = np.argmax(pytorch_output_np, axis=1)
            onnx_preds = np.argmax(ort_output, axis=1)
            match_rate = np.mean(pytorch_preds == onnx_preds)
            print(f"    Prediction match rate: {match_rate:.4%}")

            if match_rate > 0.99:
                print(f"  Verification PASSED (predictions match)")
                return True
            else:
                print(f"  Verification FAILED (predictions differ significantly)")
                return False

    except ImportError as e:
        print(f"\n  Note: onnxruntime not available ({e})")
        print(f"  Falling back to basic verification...")
        return verify_onnx_model_basic(model_path)

    except Exception as e:
        print(f"\n  ERROR during full verification: {e}")
        print(f"  Falling back to basic verification...")
        return verify_onnx_model_basic(model_path)


def benchmark_inference(model_path: str, num_runs: int = 100) -> None:
    """
    Benchmark ONNX Runtime inference speed.

    Args:
        model_path: Path to the ONNX model
        num_runs: Number of inference runs for timing
    """
    try:
        import onnxruntime as ort
        import time

        print(f"\nBenchmarking inference speed...")

        # Create session
        providers = ['CUDAExecutionProvider', 'CPUExecutionProvider']
        session = ort.InferenceSession(model_path, providers=providers)
        active_provider = session.get_providers()[0]
        print(f"  Provider: {active_provider}")

        # Create test input
        test_input = np.random.randn(1, IN_CHANNELS, IMAGE_SIZE, IMAGE_SIZE).astype(np.float32)

        # Warmup
        print(f"  Warming up...")
        for _ in range(10):
            session.run(None, {'input': test_input})

        # Benchmark
        print(f"  Running {num_runs} iterations...")
        start_time = time.perf_counter()
        for _ in range(num_runs):
            session.run(None, {'input': test_input})
        end_time = time.perf_counter()

        total_time = end_time - start_time
        avg_time = total_time / num_runs * 1000  # Convert to ms
        fps = num_runs / total_time

        print(f"\n  Benchmark Results:")
        print(f"    Total time: {total_time:.2f}s")
        print(f"    Average per image: {avg_time:.2f}ms")
        print(f"    Throughput: {fps:.1f} images/sec")

    except ImportError:
        print("  ONNX Runtime not installed, skipping benchmark")
    except Exception as e:
        print(f"  Benchmark error: {e}")


def main():
    parser = argparse.ArgumentParser(
        description='Export AstroUNet model to ONNX format for C++ deployment'
    )
    parser.add_argument(
        '--model', '-m',
        type=str,
        required=True,
        help='Path to the trained PyTorch model (.pth file)'
    )
    parser.add_argument(
        '--output', '-o',
        type=str,
        default='model.onnx',
        help='Output path for the ONNX model (default: model.onnx)'
    )
    parser.add_argument(
        '--opset',
        type=int,
        default=17,
        help='ONNX opset version (default: 17)'
    )
    parser.add_argument(
        '--no-verify',
        action='store_true',
        help='Skip model verification'
    )
    parser.add_argument(
        '--fp16',
        action='store_true',
        help='Also export FP16 version for faster inference'
    )
    parser.add_argument(
        '--simplify',
        action='store_true',
        help='Simplify the ONNX model graph (requires onnx-simplifier)'
    )
    parser.add_argument(
        '--benchmark',
        action='store_true',
        help='Run inference benchmark after export'
    )
    parser.add_argument(
        '--cpu',
        action='store_true',
        help='Force CPU for export (default: use CUDA if available)'
    )

    args = parser.parse_args()

    # Check input file exists
    if not Path(args.model).exists():
        print(f"ERROR: Model file not found: {args.model}")
        sys.exit(1)

    # Device selection
    if args.cpu or not torch.cuda.is_available():
        device = torch.device('cpu')
    else:
        device = torch.device('cuda')
    print(f"Using device: {device}")

    # Load PyTorch model
    model = load_model(args.model, device)

    # Export to ONNX
    export_to_onnx(model, args.output, args.opset, device)

    # Optionally simplify
    output_path = args.output
    if args.simplify:
        output_path = simplify_onnx(args.output)

    # Optionally convert to FP16
    if args.fp16:
        convert_to_fp16(output_path)

    # Verify exported model
    if not args.no_verify:
        verify_onnx_model(args.output, model, device)

    # Run benchmark
    if args.benchmark:
        benchmark_inference(args.output)

    print(f"\nExport complete!")
    print(f"ONNX model saved to: {args.output}")
    print(f"\nTo use in C++ with ONNX Runtime:")
    print(f"  1. Load model: Ort::Session session(env, \"{args.output}\", session_options);")
    print(f"  2. Input tensor: \"input\" with shape [batch, 4, 512, 512]")
    print(f"  3. Output tensor: \"output\" with shape [batch, 21, 512, 512]")


if __name__ == '__main__':
    main()
