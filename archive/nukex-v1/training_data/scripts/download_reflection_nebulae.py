#!/usr/bin/env python3
"""
Download HST data for reflection nebulae to boost nebula_reflection class.
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

OUTPUT_DIR = Path(__file__).parent.parent / "reflection_nebulae"

# Reflection nebulae targets
TARGETS = [
    ('M78', 'Reflection nebula in Orion'),
    ('NGC 1999', 'Reflection nebula with keyhole'),
    ('NGC 7023', 'Iris Nebula'),
    ('IC 2118', 'Witch Head Nebula'),
    ('NGC 1435', 'Merope Nebula in Pleiades'),
    ('NGC 2068', 'M78 region'),
    ('NGC 2071', 'Reflection nebula near M78'),
    ('vdB 142', 'Elephants Trunk reflection'),
    ('IC 349', 'Barnards Merope Nebula'),
    ('NGC 6726', 'Reflection nebula in Corona Australis'),
]


def download_target(target_name, description, max_products=10):
    """Download HST data for a specific reflection nebula."""
    print(f"\n  {target_name} ({description})")

    try:
        obs = Observations.query_object(target_name, radius=5*u.arcmin)

        if len(obs) == 0:
            print(f"    No observations found")
            return []

        mask = (
            (obs['obs_collection'] == 'HST') &
            (obs['dataproduct_type'] == 'image') &
            (obs['calib_level'] >= 2)
        )
        filtered = obs[mask]

        if len(filtered) == 0:
            print(f"    No HST images")
            return []

        optical_insts = ['ACS/WFC', 'WFC3/UVIS', 'WFPC2', 'ACS/HRC']
        inst_mask = [any(inst in str(row['instrument_name']) for inst in optical_insts)
                    for row in filtered]
        filtered = filtered[inst_mask]

        if len(filtered) == 0:
            print(f"    No optical data")
            return []

        print(f"    Found {len(filtered)} observations")

        if len(filtered) > max_products:
            filtered = filtered[:max_products]

        products = Observations.get_product_list(filtered)
        drz_mask = [('drz' in str(p['productFilename']).lower() or
                    'drc' in str(p['productFilename']).lower())
                   for p in products]
        products = products[drz_mask]

        if len(products) == 0:
            print(f"    No DRZ/DRC products")
            return []

        if len(products) > max_products:
            products = products[:max_products]

        print(f"    Downloading {len(products)} products...")

        safe_name = target_name.replace(' ', '_')
        out_dir = OUTPUT_DIR / safe_name
        out_dir.mkdir(parents=True, exist_ok=True)

        manifest = Observations.download_products(products, download_dir=str(out_dir), cache=True)
        downloaded = [str(f) for f in manifest['Local Path'] if f is not None]
        print(f"    Downloaded {len(downloaded)} files")
        return downloaded

    except Exception as e:
        print(f"    Error: {e}")
        return []


def main():
    print("=" * 60)
    print("Reflection Nebulae HST Download")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    total_files = []

    for target_name, description in TARGETS:
        files = download_target(target_name, description)
        total_files.extend(files)
        time.sleep(1)

    print(f"\nTotal files: {len(total_files)}")
    print(f"FITS count: {len(list(OUTPUT_DIR.rglob('*.fits')))}")


if __name__ == "__main__":
    main()
