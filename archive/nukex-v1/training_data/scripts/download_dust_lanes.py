#!/usr/bin/env python3
"""
Download HST data for edge-on galaxies with prominent dust lanes.
"""

import os
import time
from pathlib import Path

try:
    from astroquery.mast import Observations
    import astropy.units as u
except ImportError:
    os.system("pip install astroquery astropy")
    from astroquery.mast import Observations
    import astropy.units as u

OUTPUT_DIR = Path(__file__).parent.parent / "dust_lanes"

# Edge-on galaxies with prominent dust lanes
TARGETS = [
    ('NGC 891', 'Classic edge-on spiral'),
    ('NGC 4565', 'Needle Galaxy'),
    ('NGC 4631', 'Whale Galaxy'),
    ('NGC 5907', 'Knife Edge Galaxy'),
    ('NGC 4244', 'Silver Needle'),
    ('NGC 5866', 'Spindle Galaxy'),
    ('NGC 4013', 'Edge-on with warp'),
    ('NGC 7814', 'Little Sombrero'),
    ('NGC 4526', 'Edge-on lenticular'),
    ('NGC 4710', 'Edge-on with X-structure'),
    ('NGC 4762', 'Thin edge-on'),
    ('NGC 5746', 'Edge-on spiral'),
    ('M104', 'Sombrero Galaxy'),
    ('Centaurus A', 'NGC 5128 with dust lane'),
    ('NGC 3628', 'Hamburger Galaxy'),
]


def download_target(target_name, description, max_products=10):
    """Download HST data for an edge-on galaxy."""
    print(f"\n  {target_name} ({description})")

    try:
        obs = Observations.query_object(target_name, radius=5*u.arcmin)

        if len(obs) == 0:
            print(f"    No observations")
            return []

        mask = (
            (obs['obs_collection'] == 'HST') &
            (obs['dataproduct_type'] == 'image') &
            (obs['calib_level'] >= 2)
        )
        filtered = obs[mask]

        if len(filtered) == 0:
            return []

        optical_insts = ['ACS/WFC', 'WFC3/UVIS', 'WFPC2']
        inst_mask = [any(inst in str(row['instrument_name']) for inst in optical_insts)
                    for row in filtered]
        filtered = filtered[inst_mask]

        if len(filtered) == 0:
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
    print("Edge-on Galaxies (Dust Lanes) HST Download")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    for target, desc in TARGETS:
        download_target(target, desc)
        time.sleep(1)

    fits_count = len(list(OUTPUT_DIR.rglob('*.fits')))
    print(f"\nTotal FITS: {fits_count}")


if __name__ == "__main__":
    main()
