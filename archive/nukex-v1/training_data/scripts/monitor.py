#!/usr/bin/env python3
"""
Monitor training data download progress.
Run: python monitor.py
"""

import os
import subprocess
from pathlib import Path
from datetime import datetime

DATA_DIR = Path(__file__).parent.parent

def get_dir_stats(path):
    """Get file count and total size for a directory."""
    if not path.exists():
        return 0, 0

    total_size = 0
    file_count = 0

    for f in path.rglob("*.fits*"):
        if f.is_file():
            total_size += f.stat().st_size
            file_count += 1

    return file_count, total_size


def check_process(name):
    """Check if download process is running."""
    try:
        result = subprocess.run(
            ['pgrep', '-f', f'download_{name}.py'],
            capture_output=True, text=True
        )
        return bool(result.stdout.strip())
    except:
        return False


def tail_log(log_path, lines=5):
    """Get last N lines of log file."""
    if not log_path.exists():
        return ["(no log file)"]

    try:
        with open(log_path) as f:
            all_lines = f.readlines()
            return [l.strip() for l in all_lines[-lines:]]
    except:
        return ["(could not read log)"]


def main():
    print("=" * 60)
    print(f"Training Data Download Monitor - {datetime.now().strftime('%H:%M:%S')}")
    print("=" * 60)

    # ESO stats
    eso_dir = DATA_DIR / "eso"
    eso_files, eso_size = get_dir_stats(eso_dir)
    eso_running = check_process("eso")

    print(f"\n[ESO] {'RUNNING' if eso_running else 'STOPPED'}")
    print(f"  Files: {eso_files}")
    print(f"  Size:  {eso_size/1e9:.2f} GB")
    print(f"  Recent log:")
    for line in tail_log(DATA_DIR / "eso_download.log", 3):
        print(f"    {line[:70]}")

    # HST stats
    hst_dir = DATA_DIR / "hst"
    hst_files, hst_size = get_dir_stats(hst_dir)
    hst_running = check_process("hst")

    print(f"\n[HST] {'RUNNING' if hst_running else 'STOPPED'}")
    print(f"  Files: {hst_files}")
    print(f"  Size:  {hst_size/1e9:.2f} GB")
    print(f"  Recent log:")
    for line in tail_log(DATA_DIR / "hst_download.log", 3):
        print(f"    {line[:70]}")

    # Totals
    print(f"\n{'='*60}")
    print(f"TOTAL: {eso_files + hst_files} files, {(eso_size + hst_size)/1e9:.2f} GB")
    print("=" * 60)

    # Categories breakdown
    print("\nBy Category:")
    for archive in ['eso', 'hst']:
        archive_dir = DATA_DIR / archive
        if archive_dir.exists():
            for cat_dir in sorted(archive_dir.iterdir()):
                if cat_dir.is_dir() and not cat_dir.name.startswith('.'):
                    files, size = get_dir_stats(cat_dir)
                    if files > 0:
                        print(f"  {archive}/{cat_dir.name}: {files} files ({size/1e6:.0f} MB)")


if __name__ == "__main__":
    main()
