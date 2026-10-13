#!/usr/bin/env bash
# Drive every offline subcommand the PJSR dialog uses through a frozen sidecar,
# on the hermetic fixture written by _smoke_fixture.py. No network, no Python on
# the machine: build-sidecar.sh runs this on the build host and inside bare
# Ubuntu 22.04 / Debian 12 / Rocky 9 images.
#
#   sidecar-smoke.sh <sidecar> <expected version> <fixture dir> <scratch dir>
set -euo pipefail
BIN="$1"; VER="$2"; FIX="$3"; OUT="$4"
die(){ echo "SMOKE FAIL: $*" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$FIX/stars.fits" "$FIX/starless.fits" "$FIX/gaia_local.tsv" "$OUT/"
cp -R "$FIX/cache" "$OUT/cache"
G=(--gains 0.65,0.85,0.12,0.40 --p-low 5 --p-high 95 --base-sigma 2
   --nebula-strength 1 --nebula-atmos 1 --nebula-structure 1)

got="$("$BIN" --version)"
[ "$got" = "$VER" ] || die "--version printed '$got', expected '$VER'"
fp="$("$BIN" footprint "$OUT/stars.fits" 2>/dev/null | tail -1)"
read -r ra dec r <<< "$fp"
[ -n "${r:-}" ] || die "footprint printed '$fp'"
"$BIN" prepare "$OUT/stars.fits" "$OUT/prep" --gaia-tsv "$OUT/gaia_local.tsv"
[ -s "$OUT/prep/prep.npz" ] && [ -s "$OUT/prep/qa.json" ] || die "prepare wrote no prep cache"
"$BIN" render "$OUT/cache" "$OUT/stars.fits" "$OUT/graded.fits" "${G[@]}"
[ -s "$OUT/graded.fits" ] || die "render wrote nothing"
"$BIN" nebula "$OUT/prep" "$OUT/starless.fits" "$OUT/nebula.fits" "${G[@]}"
[ -s "$OUT/nebula.fits" ] || die "nebula wrote nothing"
"$BIN" preview "$OUT/prep" "$OUT/stars.fits" "$OUT/starless.fits" "$OUT/preview.png" \
   --inset "$OUT/inset.png" --region 100,50,64,64 --max-width 200 "${G[@]}"
[ -s "$OUT/preview.png" ] && [ -s "$OUT/inset.png" ] || die "preview wrote no PNG"
echo "SMOKE OK: version $got; footprint $ra $dec $r; prepare/render/nebula/preview all wrote output"
