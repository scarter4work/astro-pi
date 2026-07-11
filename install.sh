#!/usr/bin/env bash
# RC-Astro PI wrappers are already in this directory. This prints the one-time
# Feature Scripts registration steps (PJSR cannot register feature dirs itself).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Scripts live in: $DIR"
echo
echo "One-time registration in PixInsight:"
echo "  1. SCRIPT menu > Feature Scripts..."
echo "  2. Add  ->  select: $DIR"
echo "  3. Done. Then find them under:  Scripts > RC-Astro > *(CLI)"
echo
echo "rc-astro binary: $(command -v rc-astro || echo 'NOT FOUND on PATH')"
