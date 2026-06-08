#!/bin/bash
# =============================================================================
# NukeX v8 Full Training Run
# =============================================================================
#
# Training Configuration:
#   - Dataset: unified_manifest_v7.json (14,015 image-mask pairs)
#   - Class weights: class_weights_v5.npy (higher reflection nebula weight)
#   - Epochs: 150 with auto-curriculum
#   - Batch size: 32 (optimized for RTX 5070 Ti 16GB VRAM)
#   - All v7 augmentations enabled
#
# Expected Performance:
#   - VRAM usage: ~12-14 GB (batch_size=32 with 512x512 images)
#   - Training time: ~6-8 hours on RTX 5070 Ti
#   - Samples/second: ~80-100 (depending on augmentation)
#
# Checkpointing:
#   - best_model.pth: Saved when validation loss improves
#   - best_color_model.pth: Saved when emission/reflection confusion decreases
#   - checkpoint_epoch{N}.pth: Saved every 10 epochs
#   - final_model.pth: Saved at end of training
#
# Curriculum Learning Phases (auto-curriculum):
#   - Epochs 1-50 (Phase 1): 80% RGB focus - learn color discrimination
#   - Epochs 51-100 (Phase 2): 50/50 balanced - generalize to mono
#   - Epochs 101-150 (Phase 3): All data - fine-tune on full distribution
#
# Monitoring:
#   - Watch the log file: tail -f training_v8_full.log
#   - Key metrics to watch:
#     * Val loss should decrease (target: <0.3)
#     * Color confusion rate should decrease (target: <0.05)
#     * Emission IoU should increase (target: >0.5)
#     * Reflection IoU should increase (target: >0.3)
#
# Success Criteria:
#   - Val loss < 0.30
#   - Overall accuracy > 85%
#   - Emission/Reflection confusion rate < 5%
#   - Emission nebula IoU > 0.5
#   - Reflection nebula IoU > 0.3 (harder class)
#
# To resume from checkpoint:
#   python3 train_21class.py --resume /path/to/checkpoint.pth [other args]
#
# =============================================================================

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  NukeX v8 Full Training - 150 Epochs      ${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# Configuration
SCRIPT_DIR="/home/scarter4work/projects/NukeX/training_data/scripts"
DATA_DIR="/home/scarter4work/projects/NukeX/training_data"
OUTPUT_DIR="${DATA_DIR}/models_v8"
LOG_FILE="${DATA_DIR}/training_v8_full.log"

MANIFEST="${DATA_DIR}/unified_manifest_v7.json"
WEIGHTS="${DATA_DIR}/class_weights_v5.npy"

BATCH_SIZE=32
EPOCHS=150
LEARNING_RATE=1e-4
IMAGE_SIZE=512
NUM_WORKERS=4

# Verify prerequisites
echo -e "${YELLOW}Checking prerequisites...${NC}"

if [ ! -f "${MANIFEST}" ]; then
    echo -e "${RED}ERROR: Manifest not found: ${MANIFEST}${NC}"
    exit 1
fi

if [ ! -f "${WEIGHTS}" ]; then
    echo -e "${RED}ERROR: Class weights not found: ${WEIGHTS}${NC}"
    exit 1
fi

if [ ! -f "${SCRIPT_DIR}/train_21class.py" ]; then
    echo -e "${RED}ERROR: Training script not found: ${SCRIPT_DIR}/train_21class.py${NC}"
    exit 1
fi

# Check GPU
if ! nvidia-smi &> /dev/null; then
    echo -e "${RED}ERROR: NVIDIA GPU not detected${NC}"
    exit 1
fi

echo -e "${GREEN}All prerequisites satisfied${NC}"
echo ""

# Display configuration
echo -e "${YELLOW}Configuration:${NC}"
echo "  Manifest:    ${MANIFEST}"
echo "  Weights:     ${WEIGHTS}"
echo "  Output:      ${OUTPUT_DIR}"
echo "  Log:         ${LOG_FILE}"
echo ""
echo "  Batch size:  ${BATCH_SIZE}"
echo "  Epochs:      ${EPOCHS}"
echo "  LR:          ${LEARNING_RATE}"
echo "  Image size:  ${IMAGE_SIZE}x${IMAGE_SIZE}"
echo ""
echo "  Augmentations:"
echo "    - Auto-curriculum: ENABLED"
echo "    - Enhanced RGB augment: ENABLED"
echo "    - Synthetic color (mono->RGB): 70% probability"
echo "    - Stretch variation: ENABLED"
echo ""

# Estimate VRAM usage
# AstroUNet with 512x512, batch=32, FP16: ~12-14GB
echo -e "${YELLOW}Estimated Resources:${NC}"
echo "  VRAM usage:     ~12-14 GB"
echo "  Training time:  ~6-8 hours"
echo "  Disk space:     ~500 MB (checkpoints)"
echo ""

# Show GPU info
echo -e "${YELLOW}GPU Information:${NC}"
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader
echo ""

# Create output directory
mkdir -p "${OUTPUT_DIR}"

# Start training
echo -e "${GREEN}Starting training at $(date)${NC}"
echo "Log file: ${LOG_FILE}"
echo ""
echo -e "${YELLOW}To monitor progress:${NC}"
echo "  tail -f ${LOG_FILE}"
echo ""
echo "Press Ctrl+C to abort..."
echo ""

cd "${SCRIPT_DIR}"

python3 train_21class.py \
    --manifest "${MANIFEST}" \
    --weights "${WEIGHTS}" \
    --output "${OUTPUT_DIR}" \
    --batch-size ${BATCH_SIZE} \
    --epochs ${EPOCHS} \
    --lr ${LEARNING_RATE} \
    --image-size ${IMAGE_SIZE} \
    --num-workers ${NUM_WORKERS} \
    --auto-curriculum \
    --enhanced-augment \
    --synthetic-color-prob 0.7 \
    --stretch-augment \
    2>&1 | tee "${LOG_FILE}"

# Training complete
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Training Complete                        ${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo "Finished at: $(date)"
echo ""
echo "Output directory: ${OUTPUT_DIR}"
echo ""
echo "Saved models:"
ls -lh "${OUTPUT_DIR}"/*.pth 2>/dev/null || echo "No models found"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Evaluate best model: python3 evaluate_model.py --model ${OUTPUT_DIR}/best_model.pth"
echo "  2. Test on real images"
echo "  3. If satisfied, export for NukeX C++ integration"
echo ""
