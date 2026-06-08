#!/usr/bin/env python3
"""
Master Training Data Downloader for NukeX Segmentation Model

Orchestrates downloads from ESO and HST archives.
Run this script to collect training data for the 21-class segmentation model.

DATA SOURCE PRIORITY (see DATA_SOURCES.md):
  Priority 1 (HIGHEST): QNAP mount data - use process_qnap_data.py instead
  Priority 2: This script - HST/ESO archives (high-quality external sources)
  Priority 3 (LOWEST): General web sources

NOTE: Always prefer QNAP data first! This script is for supplementing
      training data when QNAP sources are insufficient.

Usage:
    python download_all.py          # Download from both archives
    python download_all.py --eso    # ESO only
    python download_all.py --hst    # HST only
    python download_all.py --test   # Test mode (1 target each)
"""

import argparse
import subprocess
import sys
from pathlib import Path
from datetime import datetime

SCRIPT_DIR = Path(__file__).parent


def run_downloader(script_name, test_mode=False):
    """Run a downloader script."""
    script_path = SCRIPT_DIR / script_name

    print(f"\n{'='*60}")
    print(f"Running: {script_name}")
    print(f"{'='*60}\n")

    env = None
    if test_mode:
        import os
        env = os.environ.copy()
        env['TEST_MODE'] = '1'

    result = subprocess.run(
        [sys.executable, str(script_path)],
        cwd=str(SCRIPT_DIR),
        env=env
    )

    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(
        description="Download training data for NukeX segmentation model"
    )
    parser.add_argument('--eso', action='store_true',
                        help='Download from ESO only')
    parser.add_argument('--hst', action='store_true',
                        help='Download from HST/MAST only')
    parser.add_argument('--test', action='store_true',
                        help='Test mode - download minimal data')
    args = parser.parse_args()

    # Default to both if neither specified
    if not args.eso and not args.hst:
        args.eso = True
        args.hst = True

    print("=" * 60)
    print("NukeX Training Data Collection")
    print(f"Started: {datetime.now().isoformat()}")
    print("=" * 60)

    if args.test:
        print("\n*** TEST MODE - Minimal downloads ***\n")

    success = True

    if args.eso:
        if not run_downloader("download_eso.py", args.test):
            print("WARNING: ESO download had errors")
            success = False

    if args.hst:
        if not run_downloader("download_hst.py", args.test):
            print("WARNING: HST download had errors")
            success = False

    print("\n" + "=" * 60)
    print("COLLECTION COMPLETE")
    print(f"Finished: {datetime.now().isoformat()}")
    print("=" * 60)

    # Print summary of what was downloaded
    training_dir = SCRIPT_DIR.parent

    for archive in ['eso', 'hst']:
        archive_dir = training_dir / archive
        if archive_dir.exists():
            # Count files
            fits_files = list(archive_dir.rglob("*.fits")) + list(archive_dir.rglob("*.fits.gz"))
            total_size = sum(f.stat().st_size for f in fits_files)
            print(f"\n{archive.upper()}: {len(fits_files)} FITS files, {total_size/1e9:.2f} GB")

    print(f"\nData location: {training_dir}")

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
