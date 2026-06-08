#!/usr/bin/env python3
"""
Download HST data for star clusters (both open and globular).
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

OUTPUT_DIR = Path(__file__).parent.parent / "star_clusters"

# Globular clusters
GLOBULAR_TARGETS = [
    ('47 Tucanae', 'NGC 104'),
    ('Omega Centauri', 'NGC 5139'),
    ('M13', 'Hercules Cluster'),
    ('M22', 'Sagittarius Cluster'),
    ('M5', 'NGC 5904'),
    ('M15', 'Pegasus Cluster'),
    ('M80', 'NGC 6093'),
    ('NGC 6752', 'Bright globular'),
    ('M92', 'NGC 6341'),
    ('M4', 'NGC 6121'),
]

# Open clusters
OPEN_TARGETS = [
    ('M45', 'Pleiades'),
    ('NGC 869', 'Double Cluster h'),
    ('NGC 884', 'Double Cluster chi'),
    ('M35', 'NGC 2168'),
    ('M37', 'NGC 2099'),
    ('M67', 'NGC 2682'),
    ('NGC 6231', 'Table of Scorpius'),
    ('M11', 'Wild Duck Cluster'),
    ('NGC 3603', 'Starburst cluster'),
    ('Westerlund 2', 'Young massive cluster'),
]


def download_target(target_name, description, out_subdir, max_products=8):
    """Download HST data for a cluster."""
    print(f"\n  {target_name} ({description})")

    try:
        obs = Observations.query_object(target_name, radius=3*u.arcmin)

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
        out_dir = OUTPUT_DIR / out_subdir / safe_name
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
    print("Star Clusters HST Download")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    print("\n--- GLOBULAR CLUSTERS ---")
    for target, desc in GLOBULAR_TARGETS:
        download_target(target, desc, 'globular')
        time.sleep(1)

    print("\n--- OPEN CLUSTERS ---")
    for target, desc in OPEN_TARGETS:
        download_target(target, desc, 'open')
        time.sleep(1)

    glob_count = len(list((OUTPUT_DIR / 'globular').rglob('*.fits'))) if (OUTPUT_DIR / 'globular').exists() else 0
    open_count = len(list((OUTPUT_DIR / 'open').rglob('*.fits'))) if (OUTPUT_DIR / 'open').exists() else 0
    print(f"\nGlobular FITS: {glob_count}")
    print(f"Open FITS: {open_count}")


if __name__ == "__main__":
    main()
