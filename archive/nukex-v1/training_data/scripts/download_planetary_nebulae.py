#!/usr/bin/env python3
"""
Download HST data for planetary nebulae to boost the nebula_planetary class.
Focus on M27 (Dumbbell), plus other well-known planetary nebulae.
"""

import os
import time
from pathlib import Path
from datetime import datetime

try:
    from astroquery.mast import Observations
    import astropy.units as u
except ImportError:
    os.system("pip install astroquery astropy")
    from astroquery.mast import Observations
    import astropy.units as u

OUTPUT_DIR = Path(__file__).parent.parent / "planetary_nebulae"

# Planetary nebulae targets
TARGETS = [
    ('M27', 'Dumbbell Nebula'),
    ('NGC 7293', 'Helix Nebula'),
    ('M57', 'Ring Nebula'),
    ('NGC 6543', 'Cat\'s Eye Nebula'),
    ('NGC 7027', 'Jewel Bug Nebula'),
    ('NGC 6826', 'Blinking Nebula'),
    ('NGC 3132', 'Eight-Burst Nebula'),
    ('NGC 2392', 'Eskimo Nebula'),
    ('NGC 6720', 'Ring Nebula alt'),
    ('NGC 6853', 'M27 alt name'),
]


def download_target(target_name, description, max_products=15):
    """Download HST data for a specific planetary nebula."""
    print(f"\n{'='*60}")
    print(f"Downloading: {target_name} ({description})")
    print(f"{'='*60}")

    try:
        # Query by target name
        obs = Observations.query_object(target_name, radius=5*u.arcmin)

        if len(obs) == 0:
            print(f"  No observations found")
            return []

        # Filter for HST optical images
        mask = (
            (obs['obs_collection'] == 'HST') &
            (obs['dataproduct_type'] == 'image') &
            (obs['calib_level'] >= 2)
        )
        filtered = obs[mask]

        if len(filtered) == 0:
            print(f"  No HST images found")
            return []

        # Filter for optical instruments
        optical_insts = ['ACS/WFC', 'WFC3/UVIS', 'WFPC2', 'ACS/HRC', 'WFC3/IR']
        inst_mask = [any(inst in str(row['instrument_name']) for inst in optical_insts)
                    for row in filtered]
        filtered = filtered[inst_mask]

        if len(filtered) == 0:
            print(f"  No optical instrument data")
            return []

        print(f"  Found {len(filtered)} observations")

        # Limit
        if len(filtered) > max_products:
            filtered = filtered[:max_products]

        # Get data products
        products = Observations.get_product_list(filtered)

        # Filter for DRZ/DRC files (calibrated)
        drz_mask = [('drz' in str(p['productFilename']).lower() or
                    'drc' in str(p['productFilename']).lower())
                   for p in products]
        products = products[drz_mask]

        if len(products) == 0:
            print(f"  No DRZ/DRC products")
            return []

        # Limit products
        if len(products) > max_products:
            products = products[:max_products]

        print(f"  Downloading {len(products)} products...")

        # Create output directory
        safe_name = target_name.replace(' ', '_')
        out_dir = OUTPUT_DIR / safe_name
        out_dir.mkdir(parents=True, exist_ok=True)

        # Download
        manifest = Observations.download_products(
            products,
            download_dir=str(out_dir),
            cache=True
        )

        downloaded = [str(f) for f in manifest['Local Path'] if f is not None]
        print(f"  Downloaded {len(downloaded)} files")
        return downloaded

    except Exception as e:
        print(f"  Error: {e}")
        return []


def main():
    print("=" * 60)
    print("Planetary Nebulae HST Data Download")
    print(f"Started: {datetime.now().isoformat()}")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    total_files = []

    for target_name, description in TARGETS:
        files = download_target(target_name, description, max_products=15)
        total_files.extend(files)
        time.sleep(2)  # Rate limiting

    print(f"\n{'='*60}")
    print(f"DOWNLOAD COMPLETE")
    print(f"Total files: {len(total_files)}")
    print(f"{'='*60}")

    # Count FITS files
    fits_count = len(list(OUTPUT_DIR.rglob("*.fits")))
    print(f"Total FITS files: {fits_count}")


if __name__ == "__main__":
    main()
