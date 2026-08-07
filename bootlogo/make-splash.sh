#!/usr/bin/env bash
# make-splash.sh - image -> ready-to-write MPC X splash image.
#
# Format of the splash partition (mmcblk0p4, verified 2026-08-07):
#   - RAW BGRA pixels from offset 0, NO file header (not a BMP!)
#   - 800x1280 (portrait/native, rotated 90 degrees vs. the landscape UI)
#   - 4096000 bytes of image data
#   - followed by 98304 bytes (96 KB) of metadata -> MUST be preserved, these
#     are NOT zeros. This script appends them from the original backup.
#   - partition total: 4194304 bytes
#
# Usage:  ./make-splash.sh <image.png|jpg> [output.img]
#
# The source image is fitted to 800x1280. Because the panel is rotated, a
# normally oriented (landscape) picture is rotated 90 degrees here so it
# appears upright on the device.
set -euo pipefail

SRC="${1:?Usage: $0 <image.png> [output.img]}"
OUT="${2:-splash_custom.img}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ORIG="${HERE}/backup/splash_p4.img"

IMG_BYTES=4096000     # 800*1280*4
PART_BYTES=4194304    # 4 MiB

command -v ffmpeg >/dev/null || { echo "ffmpeg missing: brew install ffmpeg"; exit 1; }
[ -f "$ORIG" ] || { echo "ERROR: original backup missing: $ORIG"; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Landscape source (1280x800) -> portrait (800x1280): transpose=2.
# Empirically verified: rotating a landscape reference with transpose=2 yields
# the partition's original pixels byte for byte (md5 85772a81...).
# (The opposite direction, portrait->landscape, is transpose=1 - don't mix them up.)
# The image is scaled to fit and centred on 800x1280 (black bars).
ffmpeg -hide_banner -loglevel error -i "$SRC" \
  -vf "transpose=2,scale=800:1280:force_original_aspect_ratio=decrease,pad=800:1280:(ow-iw)/2:(oh-ih)/2:black,format=bgra" \
  -frames:v 1 -f rawvideo -pix_fmt bgra -y "${tmp}/px.raw"

sz=$(wc -c < "${tmp}/px.raw")
[ "$sz" -eq "$IMG_BYTES" ] || { echo "ERROR: pixel data $sz != $IMG_BYTES bytes"; exit 1; }

# Combine image data + the original metadata tail.
cat "${tmp}/px.raw" > "$OUT"
dd if="$ORIG" bs=1 skip="$IMG_BYTES" 2>/dev/null >> "$OUT"

out_sz=$(wc -c < "$OUT")
[ "$out_sz" -eq "$PART_BYTES" ] || { echo "ERROR: output $out_sz != $PART_BYTES bytes"; exit 1; }

echo "Done: $OUT ($out_sz bytes)"
echo
echo "Render a preview (this is how it looks on the device):"
echo "  head -c $IMG_BYTES \"$OUT\" | ffmpeg -f rawvideo -pix_fmt bgra -s 800x1280 -i - -y preview.png"
echo
echo "Write it to the MPC (CAUTION: writes a raw partition):"
echo "  scp \"$OUT\" mpcx:/tmp/splash.img"
echo "  ssh mpcx 'dd if=/tmp/splash.img of=/dev/mmcblk0p4 bs=64k && sync'"
echo
echo "Roll back using the original:"
echo "  scp \"$ORIG\" mpcx:/tmp/orig.img"
echo "  ssh mpcx 'dd if=/tmp/orig.img of=/dev/mmcblk0p4 bs=64k && sync'"
