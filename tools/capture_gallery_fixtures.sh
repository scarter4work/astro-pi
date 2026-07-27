#!/usr/bin/env bash
# Capture the gallery HTML fixtures used by the discovery parser tests.
#
# Fixtures are COMMITTED, so the test suite never touches the network. Re-run this only
# to refresh them deliberately (e.g. after tests/test_gallery_live.py reports drift).
# One request per second, as everywhere else in this subsystem.
set -euo pipefail

DEST="$(dirname "$0")/../tests/fixtures/gallery"
UA='AutoContrast/0.1 (+https://github.com/scarter4work/autocontrast; scarter4work@yahoo.com)'
mkdir -p "$DEST"

fetch () {  # $1 = destination filename, $2 = URL
  echo "  $1"
  curl -sSL -A "$UA" --max-time 60 "$2" -o "$DEST/$1"
  sleep 1
}

echo "Capturing gallery fixtures into $DEST"
fetch hubble_listing_orion.html \
  'https://esahubble.org/images/archive/search/page/1/?subject_name=Orion+Nebula&minimum_size=2'
fetch eso_listing_orion.html \
  'https://www.eso.org/public/images/archive/search/?subject_name=Orion+Nebula'
fetch hubble_detail_heic0601a.html   'https://esahubble.org/images/heic0601a/'
fetch eso_detail_eso1103a.html       'https://www.eso.org/public/images/eso1103a/'
fetch hubble_detail_opo0205c.html    'https://esahubble.org/images/opo0205c/'
fetch hubble_detail_heic0211i.html   'https://esahubble.org/images/heic0211i/'
echo "Done. Review the diff before committing — these are test ground truth."
