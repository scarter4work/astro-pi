#!/usr/bin/env python3
"""
Targeted downloads to fill in missing/weak classes.
Focus on: star fields, elliptical galaxies, edge-on galaxies (dust lanes)
"""

import os
import sys
import time
from pathlib import Path
from datetime import datetime

try:
    from astroquery.mast import Observations
    from astropy.coordinates import SkyCoord
    import astropy.units as u
except ImportError:
    print("Installing required packages...")
    os.system("pip install astroquery astropy")
    from astroquery.mast import Observations
    from astropy.coordinates import SkyCoord
    import astropy.units as u

OUTPUT_DIR = Path(__file__).parent.parent / "targeted"


# Targets specifically chosen to fill gaps
TARGETS = {
    # Star fields - need star_bright, star_medium, star_faint
    'star_fields': [
        ('NGC 6397', 'star_field'),      # Nearby globular - resolved stars
        ('NGC 2264', 'star_field'),      # Christmas Tree cluster
        ('NGC 6633', 'star_field'),      # Open cluster
        ('NGC 752', 'star_field'),       # Old open cluster
        ('NGC 7789', 'star_field'),      # Rich open cluster
    ],

    # Elliptical galaxies - need galaxy_elliptical
    'elliptical_galaxies': [
        ('M87', 'galaxy_elliptical'),    # Giant elliptical in Virgo
        ('NGC 1316', 'galaxy_elliptical'),  # Fornax A
        ('NGC 4472', 'galaxy_elliptical'),  # M49, bright elliptical
        ('NGC 4649', 'galaxy_elliptical'),  # M60
        ('NGC 4374', 'galaxy_elliptical'),  # M84
    ],

    # Edge-on galaxies - need dust_lane
    'edge_on_galaxies': [
        ('NGC 4565', 'dust_lane'),       # Needle Galaxy - classic dust lane
        ('NGC 891', 'dust_lane'),        # Edge-on with prominent dust
        ('NGC 4631', 'dust_lane'),       # Whale Galaxy
        ('NGC 5907', 'dust_lane'),       # Knife Edge Galaxy
        ('NGC 4244', 'dust_lane'),       # Silver Needle
        ('NGC 55', 'dust_lane'),         # Edge-on irregular
    ],

    # Irregular galaxies - need galaxy_irregular
    'irregular_galaxies': [
        ('NGC 4449', 'galaxy_irregular'), # Irregular starburst
        ('NGC 1569', 'galaxy_irregular'), # Dwarf irregular
        ('NGC 4214', 'galaxy_irregular'), # Dwarf starburst
        ('IC 10', 'galaxy_irregular'),    # Irregular galaxy
    ],
}


def download_target(target_name, category, max_products=10):
    """Download HST data for a specific target."""
    print(f"\n  Downloading: {target_name} ({category})")

    try:
        # Query by target name
        obs = Observations.query_object(target_name, radius=3*u.arcmin)

        if len(obs) == 0:
            print(f"    No observations found")
            return []

        # Filter for HST optical
        mask = (
            (obs['obs_collection'] == 'HST') &
            (obs['dataproduct_type'] == 'image') &
            (obs['calib_level'] >= 2)
        )
        filtered = obs[mask]

        if len(filtered) == 0:
            print(f"    No HST images found")
            return []

        # Filter for optical instruments
        optical_insts = ['ACS/WFC', 'WFC3/UVIS', 'WFPC2', 'ACS/HRC']
        inst_mask = [any(inst in str(row['instrument_name']) for inst in optical_insts)
                    for row in filtered]
        filtered = filtered[inst_mask]

        if len(filtered) == 0:
            print(f"    No optical instrument data")
            return []

        print(f"    Found {len(filtered)} observations")

        # Limit downloads
        if len(filtered) > max_products:
            filtered = filtered[:max_products]

        # Get data products
        products = Observations.get_product_list(filtered)

        # Filter for DRZ/DRC files
        drz_mask = [('drz' in str(p['productFilename']).lower() or
                    'drc' in str(p['productFilename']).lower())
                   for p in products]
        products = products[drz_mask]

        if len(products) == 0:
            print(f"    No DRZ/DRC products")
            return []

        # Further limit
        if len(products) > max_products:
            products = products[:max_products]

        print(f"    Downloading {len(products)} products...")

        # Create output directory
        out_dir = OUTPUT_DIR / category / target_name.replace(' ', '_')
        out_dir.mkdir(parents=True, exist_ok=True)

        # Download
        manifest = Observations.download_products(
            products,
            download_dir=str(out_dir),
            cache=True
        )

        downloaded = [str(f) for f in manifest['Local Path'] if f is not None]
        print(f"    Downloaded {len(downloaded)} files")
        return downloaded

    except Exception as e:
        print(f"    Error: {e}")
        return []


def main():
    print("=" * 60)
    print("Targeted HST Downloads")
    print(f"Started: {datetime.now().isoformat()}")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    total_files = []

    for group_name, targets in TARGETS.items():
        print(f"\n{'='*60}")
        print(f"GROUP: {group_name}")
        print(f"{'='*60}")

        for target_name, category in targets:
            files = download_target(target_name, category, max_products=5)
            total_files.extend(files)
            time.sleep(1)  # Rate limiting

    print(f"\n{'='*60}")
    print(f"DOWNLOAD COMPLETE")
    print(f"Total files: {len(total_files)}")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
