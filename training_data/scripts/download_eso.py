#!/usr/bin/env python3
"""
ESO Archive Data Downloader for NukeX Segmentation Training

Downloads optical imaging data from ESO archive for training the 21-class
segmentation model. Uses TAP queries for reliability.
"""

import os
import sys
import time
import logging
import requests
from pathlib import Path
from datetime import datetime

from astroquery.simbad import Simbad
from pyvo import tap

from targets import TARGETS, PRIORITY_TARGETS

# Configuration
OUTPUT_DIR = Path(__file__).parent.parent / "eso"
LOG_FILE = OUTPUT_DIR / "download_log.txt"
MAX_FILES_PER_TARGET = 10
MAX_TOTAL_GB = 50

# ESO TAP service
ESO_TAP_URL = "https://archive.eso.org/tap_obs"

# Optical imaging instruments we want
OPTICAL_INSTRUMENTS = "('FORS2', 'FORS1', 'WFI', 'OMEGACAM', 'EFOSC2', 'EMMI', 'SUSI2')"

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


def resolve_target_coords(target_name):
    """Resolve target name to coordinates using SIMBAD."""
    try:
        result = Simbad.query_object(target_name)
        if result is not None and len(result) > 0:
            ra = float(result['ra'][0])
            dec = float(result['dec'][0])
            return ra, dec
    except Exception as e:
        logger.warning(f"Could not resolve {target_name}: {e}")
    return None, None


def query_eso_tap(ra, dec, radius_deg=0.25):
    """Query ESO archive using TAP for optical imaging data."""
    import signal

    def timeout_handler(signum, frame):
        raise TimeoutError("TAP query timed out")

    try:
        query = f"""
        SELECT TOP 50
            target_name, s_ra as ra, s_dec as dec,
            obs_id, access_url, access_format,
            t_exptime as exptime, instrument_name,
            dataproduct_type, s_fov
        FROM ivoa.ObsCore
        WHERE 1=CONTAINS(
            POINT('ICRS', s_ra, s_dec),
            CIRCLE('ICRS', {ra}, {dec}, {radius_deg})
        )
        AND dataproduct_type = 'image'
        AND calib_level >= 2
        AND instrument_name IN {OPTICAL_INSTRUMENTS}
        ORDER BY t_exptime DESC
        """

        # Set 60 second timeout
        signal.signal(signal.SIGALRM, timeout_handler)
        signal.alarm(60)

        try:
            service = tap.TAPService(ESO_TAP_URL)
            result = service.search(query, maxrec=50)
            table = result.to_table()
        finally:
            signal.alarm(0)  # Cancel the alarm

        return table

    except TimeoutError:
        logger.warning(f"TAP query timed out for ({ra}, {dec})")
        return None
    except Exception as e:
        logger.warning(f"TAP query failed for ({ra}, {dec}): {e}")
        return None


def resolve_datalink(datalink_url):
    """Extract direct file URL from ESO DataLink response."""
    import xml.etree.ElementTree as ET

    try:
        response = requests.get(datalink_url, timeout=60)
        response.raise_for_status()

        root = ET.fromstring(response.text)
        ns = {'vot': 'http://www.ivoa.net/xml/VOTable/v1.3'}

        for tr in root.findall('.//vot:TR', ns):
            tds = tr.findall('vot:TD', ns)
            if len(tds) >= 5:
                semantics = tds[4].text if tds[4].text else ''
                if '#this' in semantics:
                    return tds[1].text  # access_url

    except Exception as e:
        logger.warning(f"DataLink resolution failed: {e}")

    return None


def download_from_url(url, destination_dir, filename=None):
    """Download a file from URL (handles DataLink URLs)."""
    try:
        # Check if this is a DataLink URL
        if 'datalink/links' in url:
            direct_url = resolve_datalink(url)
            if not direct_url:
                logger.warning(f"    Could not resolve DataLink")
                return None
            url = direct_url

        if filename is None:
            filename = url.split('/')[-1]
            if '?' in filename:
                filename = filename.split('?')[0]
            filename = filename + '.fits' if not filename.endswith('.fits') else filename

        filepath = destination_dir / filename
        if filepath.exists() and filepath.stat().st_size > 10000:
            logger.info(f"    Already exists: {filename}")
            return str(filepath)

        logger.info(f"    Downloading: {filename}...")
        response = requests.get(url, stream=True, timeout=600)
        response.raise_for_status()

        # Validate content type - reject XML/VOTable responses
        content_type = response.headers.get('content-type', '')
        if 'xml' in content_type or 'votable' in content_type:
            logger.warning(f"    Got XML response instead of FITS data")
            return None

        total_size = 0
        with open(filepath, 'wb') as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
                total_size += len(chunk)

        logger.info(f"    Downloaded {total_size/1e6:.1f} MB")
        return str(filepath)

    except Exception as e:
        logger.error(f"    Download failed: {e}")
        return None


def download_target_data(target_name, category, downloaded_bytes):
    """Download ESO data for a single target."""
    logger.info(f"Processing target: {target_name} (category: {category})")

    target_dir = OUTPUT_DIR / category / target_name.replace(" ", "_").replace("/", "_")
    target_dir.mkdir(parents=True, exist_ok=True)

    target_files = []

    # Resolve coordinates
    ra, dec = resolve_target_coords(target_name)
    if ra is None:
        logger.info(f"  Could not resolve coordinates for {target_name}")
        return target_files, downloaded_bytes

    logger.info(f"  Searching ESO at ({ra:.2f}, {dec:.2f})...")

    # Query TAP
    table = query_eso_tap(ra, dec)

    if table is None or len(table) == 0:
        logger.info(f"  No ESO optical imaging data found")
        return target_files, downloaded_bytes

    logger.info(f"  Found {len(table)} observations")

    # Download files
    to_download = min(MAX_FILES_PER_TARGET, len(table))

    for i in range(to_download):
        row = table[i]

        # Check for access_url
        access_url = row.get('access_url')
        if not access_url or str(access_url) == '--':
            continue

        access_url = str(access_url)

        # Generate filename
        obs_id = str(row.get('obs_id', f'obs_{i}'))
        inst = str(row.get('instrument_name', 'unknown'))
        filename = f"{obs_id}_{inst}.fits".replace('/', '_').replace(':', '_')

        filepath = download_from_url(access_url, target_dir, filename)

        if filepath and os.path.exists(filepath):
            size = os.path.getsize(filepath)
            downloaded_bytes += size
            target_files.append(filepath)
            logger.info(f"    Saved: {os.path.basename(filepath)} ({size/1e6:.1f} MB)")

        # Check size limit
        if downloaded_bytes > MAX_TOTAL_GB * 1e9:
            logger.warning("Reached maximum download size limit")
            break

        time.sleep(1)  # Rate limiting

    return target_files, downloaded_bytes


def main():
    """Main download routine."""
    logger.info("=" * 60)
    logger.info("ESO Training Data Downloader (TAP-based)")
    logger.info(f"Started: {datetime.now().isoformat()}")
    logger.info("=" * 60)

    total_files = []
    downloaded_bytes = 0

    # Process priority targets first
    logger.info("\n=== PRIORITY TARGETS ===")
    for target in PRIORITY_TARGETS:
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
        f.write(f"# ESO Training Data Manifest\n")
        f.write(f"# Generated: {datetime.now().isoformat()}\n")
        f.write(f"# Total files: {len(total_files)}\n")
        f.write(f"# Total size: {downloaded_bytes/1e9:.2f} GB\n\n")
        for filepath in total_files:
            f.write(f"{filepath}\n")

    logger.info(f"Manifest written to: {manifest_file}")


if __name__ == "__main__":
    main()
