#!/usr/bin/env python3
"""
Simple ESO Archive Downloader - uses target name queries instead of coordinate searches.
ESO coordinate searches are slow; this uses target name patterns which are faster.
"""

import os
import sys
import time
import logging
import requests
import xml.etree.ElementTree as ET
from pathlib import Path
from datetime import datetime

from pyvo import tap

# Configuration
OUTPUT_DIR = Path(__file__).parent.parent / "eso"
LOG_FILE = OUTPUT_DIR / "download_log.txt"
MAX_FILES_PER_TARGET = 5
MAX_TOTAL_GB = 30

ESO_TAP_URL = "https://archive.eso.org/tap_obs"
OPTICAL_INSTRUMENTS = "('FORS2', 'FORS1', 'WFI', 'OMEGACAM', 'EFOSC2', 'EMMI', 'VIMOS')"

# Target name patterns to search for
# These are common names used in ESO observations
TARGET_PATTERNS = [
    # Emission nebulae
    ("bright_emission", ["Orion", "M42", "NGC2024", "Lagoon", "M8", "Omega", "M17",
                         "Trifid", "M20", "Carina", "NGC3372", "Rosette"]),
    # Dark nebulae
    ("dark_nebula", ["Horsehead", "B33", "Barnard", "LDN", "Coal"]),
    # Reflection nebulae
    ("reflection_nebula", ["M78", "NGC1999", "Iris", "NGC7023"]),
    # Galaxies
    ("galaxy_core", ["M31", "Andromeda", "M81", "M51", "Whirlpool", "M101",
                     "NGC4565", "M104", "Sombrero"]),
    ("galaxy_arm", ["NGC891", "M82", "NGC6946", "Fireworks"]),
    # Star clusters
    ("star_clusters", ["Pleiades", "M45", "M13", "M22", "Omega Cen", "47 Tuc",
                       "NGC3603", "Westerlund"]),
    # Planetary nebulae
    ("planetary_nebula", ["Ring", "M57", "Dumbbell", "M27", "Helix", "NGC7293",
                          "Cat Eye", "NGC6543"]),
]

# Setup logging
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


def query_by_target_name(pattern):
    """Query ESO using target name pattern (fast)."""
    try:
        query = f"""
        SELECT TOP 30
            target_name, obs_id, access_url, instrument_name,
            t_exptime as exptime, s_ra as ra, s_dec as dec
        FROM ivoa.ObsCore
        WHERE target_name LIKE '%{pattern}%'
        AND dataproduct_type = 'image'
        AND calib_level >= 2
        AND instrument_name IN {OPTICAL_INSTRUMENTS}
        ORDER BY t_exptime DESC
        """

        service = tap.TAPService(ESO_TAP_URL)
        result = service.search(query, maxrec=30)
        return result.to_table()

    except Exception as e:
        logger.warning(f"Query failed for pattern '{pattern}': {e}")
        return None


def resolve_datalink(datalink_url):
    """Extract direct file URL from ESO DataLink response."""
    try:
        response = requests.get(datalink_url, timeout=30)
        response.raise_for_status()

        root = ET.fromstring(response.text)
        ns = {'vot': 'http://www.ivoa.net/xml/VOTable/v1.3'}

        for tr in root.findall('.//vot:TR', ns):
            tds = tr.findall('vot:TD', ns)
            if len(tds) >= 5:
                semantics = tds[4].text if tds[4].text else ''
                if '#this' in semantics:
                    return tds[1].text
    except Exception as e:
        logger.warning(f"DataLink resolution failed: {e}")
    return None


def download_file(url, destination_dir, filename):
    """Download a file, resolving DataLink if needed."""
    try:
        if 'datalink/links' in url:
            url = resolve_datalink(url)
            if not url:
                return None

        filepath = destination_dir / filename
        if filepath.exists() and filepath.stat().st_size > 10000:
            return str(filepath)

        logger.info(f"    Downloading: {filename}...")
        response = requests.get(url, stream=True, timeout=600)
        response.raise_for_status()

        content_type = response.headers.get('content-type', '')
        if 'xml' in content_type or 'votable' in content_type:
            logger.warning(f"    Got XML instead of FITS")
            return None

        total = 0
        with open(filepath, 'wb') as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
                total += len(chunk)

        logger.info(f"    Downloaded {total/1e6:.1f} MB")
        return str(filepath)

    except Exception as e:
        logger.error(f"    Download failed: {e}")
        return None


def main():
    logger.info("=" * 60)
    logger.info("ESO Simple Downloader (target name patterns)")
    logger.info(f"Started: {datetime.now().isoformat()}")
    logger.info("=" * 60)

    total_files = []
    downloaded_bytes = 0

    for category, patterns in TARGET_PATTERNS:
        logger.info(f"\n=== Category: {category} ===")
        cat_dir = OUTPUT_DIR / category
        cat_dir.mkdir(parents=True, exist_ok=True)

        for pattern in patterns:
            if downloaded_bytes > MAX_TOTAL_GB * 1e9:
                break

            logger.info(f"Searching for: {pattern}")
            table = query_by_target_name(pattern)

            if table is None or len(table) == 0:
                logger.info(f"  No results for '{pattern}'")
                continue

            logger.info(f"  Found {len(table)} observations")

            # Download up to MAX_FILES_PER_TARGET
            downloaded = 0
            for row in table:
                if downloaded >= MAX_FILES_PER_TARGET:
                    break

                access_url = str(row.get('access_url', ''))
                if not access_url or access_url == '--':
                    continue

                obs_id = str(row.get('obs_id', ''))
                inst = str(row.get('instrument_name', 'unknown'))
                target = str(row.get('target_name', pattern)).replace(' ', '_').replace('/', '_')

                filename = f"{target}_{obs_id}_{inst}.fits"
                filepath = download_file(access_url, cat_dir, filename)

                if filepath and os.path.exists(filepath):
                    size = os.path.getsize(filepath)
                    downloaded_bytes += size
                    total_files.append(filepath)
                    downloaded += 1

                    if downloaded_bytes > MAX_TOTAL_GB * 1e9:
                        break

                time.sleep(0.5)

        if downloaded_bytes > MAX_TOTAL_GB * 1e9:
            logger.warning("Reached size limit")
            break

    # Summary
    logger.info("\n" + "=" * 60)
    logger.info("DOWNLOAD COMPLETE")
    logger.info(f"Total files: {len(total_files)}")
    logger.info(f"Total size: {downloaded_bytes/1e9:.2f} GB")
    logger.info("=" * 60)

    # Manifest
    manifest_file = OUTPUT_DIR / "manifest.txt"
    with open(manifest_file, 'w') as f:
        f.write(f"# ESO Training Data\n")
        f.write(f"# Generated: {datetime.now().isoformat()}\n")
        f.write(f"# Files: {len(total_files)}\n")
        f.write(f"# Size: {downloaded_bytes/1e9:.2f} GB\n\n")
        for filepath in total_files:
            f.write(f"{filepath}\n")


if __name__ == "__main__":
    main()
