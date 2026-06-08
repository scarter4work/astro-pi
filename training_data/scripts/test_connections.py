#!/usr/bin/env python3
"""
Test archive connections before running full downloads.
"""

import sys

def test_simbad():
    """Test SIMBAD name resolution."""
    print("Testing SIMBAD name resolution...", end=" ")
    try:
        from astroquery.simbad import Simbad

        result = Simbad.query_object("M42")
        if result is not None and len(result) > 0:
            # New SIMBAD returns lowercase columns in degrees
            ra = float(result['ra'][0])
            dec = float(result['dec'][0])
            print(f"OK (M42 at {ra:.2f}, {dec:.2f})")
            return True
    except Exception as e:
        print(f"FAILED: {e}")
    return False


def test_eso():
    """Test ESO archive connection."""
    print("Testing ESO archive...", end=" ")
    try:
        from astroquery.eso import Eso
        eso = Eso()
        instruments = eso.list_instruments()
        if instruments and len(instruments) > 0:
            print(f"OK ({len(instruments)} instruments available)")
            return True
    except Exception as e:
        print(f"FAILED: {e}")
    return False


def test_eso_query():
    """Test ESO query functionality."""
    print("Testing ESO query (M42)...", end=" ")
    try:
        from astroquery.eso import Eso
        eso = Eso()
        # Use query_main which is more reliable
        table = eso.query_main(
            column_filters={
                'target': '%M42%',
            },
            columns=['target_name', 'dp_id'],
            top=5
        )
        if table is not None:
            print(f"OK ({len(table)} results)")
            return True
        else:
            print("OK (0 results, but query worked)")
            return True
    except Exception as e:
        print(f"FAILED: {e}")
    return False


def test_mast():
    """Test MAST archive connection."""
    print("Testing MAST/HST archive...", end=" ")
    try:
        from astroquery.mast import Observations
        from astropy.coordinates import SkyCoord
        from astropy import units as u

        # Use coordinate search (more reliable than target name)
        coord = SkyCoord(83.8201, -5.3876, unit=u.deg)  # M42 coords
        obs_table = Observations.query_criteria(
            obs_collection="HST",
            coordinates=coord,
            radius=5*u.arcmin,
            dataRights="PUBLIC",
            dataproduct_type="image"
        )
        if obs_table is not None:
            print(f"OK ({len(obs_table)} HST observations near M42)")
            return True
    except Exception as e:
        print(f"FAILED: {e}")
    return False


def test_mast_products():
    """Test MAST product listing."""
    print("Testing MAST product access...", end=" ")
    try:
        from astroquery.mast import Observations
        from astropy.coordinates import SkyCoord
        from astropy import units as u

        coord = SkyCoord(83.8201, -5.3876, unit=u.deg)
        obs_table = Observations.query_criteria(
            obs_collection="HST",
            coordinates=coord,
            radius=2*u.arcmin,
            dataRights="PUBLIC",
            dataproduct_type="image"
        )
        if obs_table is not None and len(obs_table) > 0:
            products = Observations.get_product_list(obs_table[:1])
            if products is not None:
                print(f"OK ({len(products)} products for first observation)")
                return True
    except Exception as e:
        print(f"FAILED: {e}")
    return False


def main():
    print("=" * 50)
    print("Archive Connection Tests")
    print("=" * 50)
    print()

    tests = [
        ("SIMBAD", test_simbad),
        ("ESO Archive", test_eso),
        ("ESO Query", test_eso_query),
        ("MAST Archive", test_mast),
        ("MAST Products", test_mast_products),
    ]

    results = []
    for name, test_func in tests:
        try:
            results.append((name, test_func()))
        except Exception as e:
            print(f"  {name}: ERROR - {e}")
            results.append((name, False))

    print()
    print("=" * 50)
    print("Summary")
    print("=" * 50)

    passed = sum(1 for _, r in results if r)
    failed = len(results) - passed

    for name, result in results:
        status = "PASS" if result else "FAIL"
        print(f"  {name}: {status}")

    print()
    print(f"Passed: {passed}/{len(results)}")

    if failed > 0:
        print("\nSome tests failed. Check your internet connection and dependencies.")
        return 1

    print("\nAll tests passed! Ready to download training data.")
    print("\nRun: python download_all.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
