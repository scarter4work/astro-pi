#!/usr/bin/env python3
"""
Execute the 5-point data improvement plan.

Problem 1: Emission Nebula vs Galaxy Core Confusion
Problem 2: Reflection Nebula Missed
Problem 3: Narrowband Data (SHO)
Problem 4: Linear Sub-frame Noise
Problem 5: Dark Nebula Under-detected
"""

import subprocess
import sys
from pathlib import Path

# Output directories
BASE_OUTPUT = Path('/home/scarter4work/projects/NukeX/training_data')

# QNAP data paths - verified directories
QNAP_BASE = Path('/mnt/qnap/astro_data')

# Verified object directories with Light subfolders
QNAP_OBJECTS = {
    'IC1396': QNAP_BASE / 'IC1396' / 'Light',           # Elephant Trunk - emission + dark
    'SH2-129': QNAP_BASE / 'SH2-129' / 'Light',         # Flying Bat - emission Ha/OIII
    'NGC281': QNAP_BASE / 'NGC281' / 'Light',           # Pacman - emission
    'NGC7822': QNAP_BASE / 'NGC7822' / 'Light',         # emission
    'NGC7635': QNAP_BASE / '9_5_2025' / 'NGC7635' / 'Light',  # Bubble - emission
    'NGC6888': QNAP_BASE / '7_24_2025' / 'NGC6888' / 'Light', # Crescent - emission
    'M45': QNAP_BASE / 'M45' / 'Light',                 # Pleiades - reflection
    'M78': QNAP_BASE / '10_23_2025' / 'M78' / 'Light',  # reflection
    'NGC7293': QNAP_BASE / 'NGC7293' / 'Light',         # Helix - planetary
    'M27': QNAP_BASE / 'M27' / 'Light',                 # Dumbbell - planetary
    'M57': QNAP_BASE / '8_12_2025' / 'M57' / 'Light',   # Ring - planetary
    'M33': QNAP_BASE / 'M33' / 'Light',                 # Triangulum - galaxy
    'NGC6946': QNAP_BASE / 'NGC6946' / 'Light',         # Fireworks - galaxy
}

def run_cmd(cmd, desc):
    """Run a command and print status."""
    print(f"\n{'='*60}")
    print(f">>> {desc}")
    print(f"{'='*60}")
    result = subprocess.run(cmd, shell=True)
    return result.returncode == 0


def problem1_emission_nebula():
    """
    Problem 1: More emission nebula data to distinguish from galaxy cores.

    Sources: IC1396, NGC281, NGC7635, NGC7822, SH2-129, NGC6888
    """
    print("\n" + "="*70)
    print("PROBLEM 1: EMISSION NEBULA DATA")
    print("="*70)

    output_dir = BASE_OUTPUT / 'labeled_emission_qnap'
    output_dir.mkdir(parents=True, exist_ok=True)

    emission_objects = ['IC1396', 'NGC281', 'NGC7822', 'SH2-129', 'NGC7635', 'NGC6888']

    for obj_name in emission_objects:
        obj_path = QNAP_OBJECTS.get(obj_name)
        if obj_path and obj_path.exists():
            run_cmd(
                f"python3 process_qnap_data.py '{obj_path}' --output '{output_dir}/{obj_name.lower()}' --type emission --limit 15",
                f"Processing {obj_name} emission nebula data"
            )
        else:
            print(f"  Skipping {obj_name} - path not found")

    print(f"\nEmission nebula data saved to: {output_dir}")


def problem2_reflection_nebula():
    """
    Problem 2: More reflection nebula data.

    Sources: M78, M45 (Pleiades)
    """
    print("\n" + "="*70)
    print("PROBLEM 2: REFLECTION NEBULA DATA")
    print("="*70)

    output_dir = BASE_OUTPUT / 'labeled_reflection_qnap'
    output_dir.mkdir(parents=True, exist_ok=True)

    reflection_objects = ['M78', 'M45']

    for obj_name in reflection_objects:
        obj_path = QNAP_OBJECTS.get(obj_name)
        if obj_path and obj_path.exists():
            run_cmd(
                f"python3 process_qnap_data.py '{obj_path}' --output '{output_dir}/{obj_name.lower()}' --type reflection --limit 15",
                f"Processing {obj_name} reflection nebula data"
            )
        else:
            print(f"  Skipping {obj_name} - path not found")

    print(f"\nReflection nebula data saved to: {output_dir}")


def problem3_narrowband():
    """
    Problem 3: Narrowband (SHO) training data.

    Process Ha, OIII, and SII data separately for narrowband characteristics.
    """
    print("\n" + "="*70)
    print("PROBLEM 3: NARROWBAND (SHO) DATA")
    print("="*70)

    output_dir = BASE_OUTPUT / 'labeled_narrowband'
    output_dir.mkdir(parents=True, exist_ok=True)

    # Find Ha files
    ha_sources = [
        ('IC1396', QNAP_OBJECTS.get('IC1396')),
        ('NGC281', QNAP_OBJECTS.get('NGC281')),
        ('SH2-129', QNAP_OBJECTS.get('SH2-129')),
        ('NGC7822', QNAP_OBJECTS.get('NGC7822')),
        ('NGC6888', QNAP_OBJECTS.get('NGC6888')),
    ]

    for obj_name, obj_path in ha_sources:
        if obj_path and obj_path.exists():
            run_cmd(
                f"python3 process_qnap_data.py '{obj_path}' --output '{output_dir}/{obj_name.lower()}_ha' --type emission --limit 10",
                f"Processing {obj_name} Ha narrowband"
            )

    # Process OIII data from planetary nebulae
    oiii_sources = [
        ('NGC7293', QNAP_OBJECTS.get('NGC7293')),  # Helix
        ('M27', QNAP_OBJECTS.get('M27')),          # Dumbbell
        ('M57', QNAP_OBJECTS.get('M57')),          # Ring
    ]

    for obj_name, obj_path in oiii_sources:
        if obj_path and obj_path.exists():
            run_cmd(
                f"python3 process_qnap_data.py '{obj_path}' --output '{output_dir}/{obj_name.lower()}_oiii' --type planetary --limit 10",
                f"Processing {obj_name} OIII narrowband"
            )

    print(f"\nNarrowband data saved to: {output_dir}")


def problem4_linear_noise():
    """
    Problem 4: Real linear sub-frame noise characteristics.

    Use various QNAP sub-frames to capture real camera noise patterns.
    This data trains the model on real sensor noise, gradients, and artifacts.
    """
    print("\n" + "="*70)
    print("PROBLEM 4: LINEAR SUB-FRAME NOISE DATA")
    print("="*70)

    output_dir = BASE_OUTPUT / 'labeled_linear_noise'
    output_dir.mkdir(parents=True, exist_ok=True)

    # Process a variety of objects for noise diversity
    noise_objects = [
        ('M33', 'emission'),      # Galaxy
        ('NGC6946', 'emission'),  # Fireworks Galaxy
        ('M27', 'planetary'),     # Dumbbell planetary nebula
        ('M57', 'planetary'),     # Ring planetary nebula
        ('NGC7822', 'emission'),  # Emission with faint regions
    ]

    for obj_name, obj_type in noise_objects:
        obj_path = QNAP_OBJECTS.get(obj_name)
        if obj_path and obj_path.exists():
            run_cmd(
                f"python3 process_qnap_data.py '{obj_path}' --output '{output_dir}/{obj_name.lower()}' --type {obj_type} --limit 10",
                f"Processing {obj_name} for noise characteristics"
            )
        else:
            print(f"  Skipping {obj_name} - path not found")

    print(f"\nLinear noise data saved to: {output_dir}")


def problem5_dark_nebula():
    """
    Problem 5: Dark nebula data.

    IC1396 has the Elephant Trunk dark nebula.
    M78 region has LDN dark nebulae nearby.
    """
    print("\n" + "="*70)
    print("PROBLEM 5: DARK NEBULA DATA")
    print("="*70)

    output_dir = BASE_OUTPUT / 'labeled_dark_nebula'
    output_dir.mkdir(parents=True, exist_ok=True)

    # Objects known to contain dark nebulae
    dark_objects = ['IC1396', 'M78']

    for obj_name in dark_objects:
        obj_path = QNAP_OBJECTS.get(obj_name)
        if obj_path and obj_path.exists():
            run_cmd(
                f"python3 process_qnap_data.py '{obj_path}' --output '{output_dir}/{obj_name.lower()}_dark' --type dark --limit 15",
                f"Processing {obj_name} for dark nebula"
            )
        else:
            print(f"  Skipping {obj_name} - path not found")

    print(f"\nDark nebula data saved to: {output_dir}")


def main():
    print("="*70)
    print("EXECUTING 5-POINT DATA IMPROVEMENT PLAN")
    print("="*70)

    # Change to scripts directory
    import os
    os.chdir('/home/scarter4work/projects/NukeX/training_data/scripts')

    # Execute each problem fix
    problem1_emission_nebula()
    problem2_reflection_nebula()
    problem3_narrowband()
    problem4_linear_noise()
    problem5_dark_nebula()

    print("\n" + "="*70)
    print("5-POINT PLAN EXECUTION COMPLETE")
    print("="*70)

    # Summary
    print("\nNew data directories created:")
    for d in ['labeled_emission_qnap', 'labeled_reflection_qnap', 'labeled_narrowband',
              'labeled_linear_noise', 'labeled_dark_nebula']:
        path = BASE_OUTPUT / d
        if path.exists():
            count = len(list(path.rglob('*_img.png')))
            print(f"  {d}: {count} images")


if __name__ == '__main__':
    main()
