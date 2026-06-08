#!/usr/bin/env python3
"""
HST/MAST Archive Data Downloader for NukeX Segmentation Training

Downloads Hubble Space Telescope optical imaging data for training the
21-class segmentation model. HST provides high-resolution reference data
for clean labeling.
"""

import os
import sys
import time
import logging
from pathlib import Path
from datetime import datetime

from astroquery.mast import Observations
from astroquery.simbad import Simbad
from astropy.coordinates import SkyCoord
from astropy import units as u

from targets import TARGETS, PRIORITY_TARGETS, HST_INSTRUMENTS

# Configuration
OUTPUT_DIR = Path(__file__).parent.parent / "hst"
LOG_FILE = OUTPUT_DIR / "download_log.txt"
MAX_FILES_PER_TARGET = 5  # HST files are large
MAX_TOTAL_GB = 30  # HST data limit
SEARCH_RADIUS_ARCMIN = 5.0

# Preferred filters (optical broadband)
OPTICAL_FILTERS = [
    'F555W', 'F606W', 'F814W',  # Broadband optical
    'F435W', 'F475W', 'F625W',  # Blue to red
    'F550M', 'F658N',           # Medium/narrow useful for nebulae
    'CLEAR',                     # Unfiltered
]

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


def resolve_target_coords(target_name):
    """Resolve target name to coordinates using SIMBAD."""
    try:
        result = Simbad.query_object(target_name)
        if result is not None and len(result) > 0:
            # New SIMBAD returns lowercase columns in degrees
            ra = float(result['ra'][0])
            dec = float(result['dec'][0])
            return ra, dec
    except Exception as e:
        logger.warning(f"Could not resolve {target_name}: {e}")
    return None, None


def query_hst_by_name(target_name):
    """Query MAST for HST observations by target name."""
    try:
        obs_table = Observations.query_criteria(
            obs_collection="HST",
            instrument_name=HST_INSTRUMENTS,
            target_name=target_name,
            dataRights="PUBLIC",
            intentType="science",
            dataproduct_type="image",
        )
        return obs_table
    except Exception as e:
        logger.warning(f"Name query failed for {target_name}: {e}")
        return None


def query_hst_by_coords(ra, dec, radius_arcmin=5.0):
    """Query MAST for HST observations by coordinates."""
    try:
        coord = SkyCoord(ra, dec, unit=u.deg)
        obs_table = Observations.query_criteria(
            obs_collection="HST",
            instrument_name=HST_INSTRUMENTS,
            coordinates=coord,
            radius=radius_arcmin * u.arcmin,
            dataRights="PUBLIC",
            intentType="science",
            dataproduct_type="image",
        )
        return obs_table
    except Exception as e:
        logger.warning(f"Coordinate query failed for ({ra}, {dec}): {e}")
        return None


def filter_observations(obs_table):
    """Filter observations to prefer optical broadband filters."""
    if obs_table is None or len(obs_table) == 0:
        return obs_table

    # Try to filter by optical filters
    filtered = []
    for i, row in enumerate(obs_table):
        filters = str(row.get('filters', ''))
        # Check if any optical filter matches
        for opt_filt in OPTICAL_FILTERS:
            if opt_filt in filters:
                filtered.append(i)
                break

    if filtered:
        return obs_table[filtered]

    # If no optical filters found, return all
    return obs_table


def download_target_data(target_name, category, downloaded_bytes):
    """Download HST data for a single target."""
    logger.info(f"Processing target: {target_name} (category: {category})")

    target_dir = OUTPUT_DIR / category / target_name.replace(" ", "_")
    target_dir.mkdir(parents=True, exist_ok=True)

    # Track downloads
    target_files = []

    # Use coordinate search first (more reliable - HST target names vary)
    ra, dec = resolve_target_coords(target_name)
    obs_table = None

    if ra is not None:
        logger.info(f"  Searching HST at ({ra:.2f}, {dec:.2f})...")
        obs_table = query_hst_by_coords(ra, dec, SEARCH_RADIUS_ARCMIN)

    # Fallback to name search if coords failed
    if obs_table is None or len(obs_table) == 0:
        logger.info(f"  Trying name search for {target_name}...")
        obs_table = query_hst_by_name(target_name)

    if obs_table is None or len(obs_table) == 0:
        logger.info(f"  No HST observations found for {target_name}")
        return target_files, downloaded_bytes

    logger.info(f"  Found {len(obs_table)} HST observations")

    # Filter for optical
    obs_table = filter_observations(obs_table)
    logger.info(f"  After filter selection: {len(obs_table)} observations")

    # Limit number of observations
    obs_table = obs_table[:MAX_FILES_PER_TARGET]

    try:
        # Get product list
        products = Observations.get_product_list(obs_table)

        if products is None or len(products) == 0:
            logger.info(f"  No products available")
            return target_files, downloaded_bytes

        logger.info(f"  Found {len(products)} data products")

        # Filter to calibrated science FITS only
        filtered = Observations.filter_products(
            products,
            extension="fits",
            productType=["SCIENCE"],
            calib_level=[2, 3],  # Calibrated or combined
            productSubGroupDescription=["DRZ", "DRC", "FLC", "FLT"]  # Prefer drizzled
        )

        if filtered is None or len(filtered) == 0:
            # Try less restrictive filter
            filtered = Observations.filter_products(
                products,
                extension="fits",
                productType=["SCIENCE"],
                calib_level=[2, 3]
            )

        if filtered is None or len(filtered) == 0:
            logger.info(f"  No suitable products after filtering")
            return target_files, downloaded_bytes

        logger.info(f"  Downloading {len(filtered)} products...")

        # Download
        manifest = Observations.download_products(
            filtered,
            download_dir=str(target_dir),
            cache=True
        )

        if manifest is not None:
            for row in manifest:
                local_path = row['Local Path']
                if local_path and os.path.exists(local_path):
                    size = os.path.getsize(local_path)
                    downloaded_bytes += size
                    target_files.append(local_path)
                    logger.info(f"    Downloaded: {os.path.basename(local_path)} ({size/1e6:.1f} MB)")

    except Exception as e:
        logger.error(f"  Download failed: {e}")

    # Check total size limit
    if downloaded_bytes > MAX_TOTAL_GB * 1e9:
        logger.warning("Reached maximum download size limit")

    time.sleep(1)  # Be nice to the server

    return target_files, downloaded_bytes


def main():
    """Main download routine."""
    logger.info("=" * 60)
    logger.info("HST/MAST Training Data Downloader")
    logger.info(f"Started: {datetime.now().isoformat()}")
    logger.info("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Track downloads
    total_files = []
    downloaded_bytes = 0

    # Process priority targets first
    logger.info("\n=== PRIORITY TARGETS ===")
    for target in PRIORITY_TARGETS:
        # Find category
        category = "misc"
        for cat, targets in TARGETS.items():
            if target in targets:
                category = cat
                break

        files, downloaded_bytes = download_target_data(
            target, category, downloaded_bytes
        )
        total_files.extend(files)

        if downloaded_bytes > MAX_TOTAL_GB * 1e9:
            break

    # Process remaining targets
    if downloaded_bytes < MAX_TOTAL_GB * 1e9:
        logger.info("\n=== CATEGORY TARGETS ===")
        for category, targets in TARGETS.items():
            logger.info(f"\n--- Category: {category} ---")

            for target in targets:
                if target in PRIORITY_TARGETS:
                    continue

                files, downloaded_bytes = download_target_data(
                    target, category, downloaded_bytes
                )
                total_files.extend(files)

                if downloaded_bytes > MAX_TOTAL_GB * 1e9:
                    break

            if downloaded_bytes > MAX_TOTAL_GB * 1e9:
                break

    # Summary
    logger.info("\n" + "=" * 60)
    logger.info("DOWNLOAD COMPLETE")
    logger.info(f"Total files: {len(total_files)}")
    logger.info(f"Total size: {downloaded_bytes/1e9:.2f} GB")
    logger.info(f"Output directory: {OUTPUT_DIR}")
    logger.info("=" * 60)

    # Write manifest
    manifest_file = OUTPUT_DIR / "manifest.txt"
    with open(manifest_file, 'w') as f:
        f.write(f"# HST Training Data Manifest\n")
        f.write(f"# Generated: {datetime.now().isoformat()}\n")
        f.write(f"# Total files: {len(total_files)}\n")
        f.write(f"# Total size: {downloaded_bytes/1e9:.2f} GB\n\n")
        for filepath in total_files:
            f.write(f"{filepath}\n")

    logger.info(f"Manifest written to: {manifest_file}")


if __name__ == "__main__":
    main()
