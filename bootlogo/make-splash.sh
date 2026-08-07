#!/usr/bin/env bash
# make-splash.sh - Bild -> schreibfertiges MPC-X-Splash-Image.
#
# Format der splash-Partition (mmcblk0p4, verifiziert 2026-08-07):
#   - ROHE BGRA-Pixel ab Offset 0, KEIN Datei-Header (kein BMP!)
#   - 800x1280 (Portrait/nativ, um 90 Grad gedreht ggue. der Landscape-UI)
#   - 4096000 Bytes Bilddaten
#   - danach 98304 Bytes (96 KB) Metadaten -> MUESSEN erhalten bleiben,
#     sind KEINE Nullen. Dieses Script haengt sie aus dem Original-Backup an.
#   - Partition gesamt: 4194304 Bytes
#
# Aufruf:  ./make-splash.sh <bild.png|jpg> [ausgabe.img]
#
# Das Eingangsbild wird auf 800x1280 gebracht. Da das Panel gedreht ist:
# ein normal orientiertes (Landscape-)Motiv wird hier um 90 Grad gedreht,
# damit es auf dem Geraet aufrecht erscheint.
set -euo pipefail

SRC="${1:?Aufruf: $0 <bild.png> [ausgabe.img]}"
OUT="${2:-splash_custom.img}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ORIG="${HERE}/backup/splash_p4.img"

IMG_BYTES=4096000     # 800*1280*4
PART_BYTES=4194304    # 4 MiB

command -v ffmpeg >/dev/null || { echo "ffmpeg fehlt: brew install ffmpeg"; exit 1; }
[ -f "$ORIG" ] || { echo "FEHLER: Original-Backup fehlt: $ORIG"; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Landscape-Motiv (1280x800) -> Portrait (800x1280): transpose=2.
# Empirisch verifiziert: Landscape-Referenz mit transpose=2 gedreht ergibt
# bitgenau die Originalpixel der Partition (md5 85772a81...).
# (Die Gegenrichtung Portrait->Landscape ist transpose=1 - nicht verwechseln.)
# Bild wird passend skaliert und auf 800x1280 zentriert (schwarze Raender).
ffmpeg -hide_banner -loglevel error -i "$SRC" \
  -vf "transpose=2,scale=800:1280:force_original_aspect_ratio=decrease,pad=800:1280:(ow-iw)/2:(oh-ih)/2:black,format=bgra" \
  -frames:v 1 -f rawvideo -pix_fmt bgra -y "${tmp}/px.raw"

sz=$(wc -c < "${tmp}/px.raw")
[ "$sz" -eq "$IMG_BYTES" ] || { echo "FEHLER: Pixeldaten $sz != $IMG_BYTES Bytes"; exit 1; }

# Bilddaten + Original-Metadatenschwanz zusammensetzen.
cat "${tmp}/px.raw" > "$OUT"
dd if="$ORIG" bs=1 skip="$IMG_BYTES" 2>/dev/null >> "$OUT"

out_sz=$(wc -c < "$OUT")
[ "$out_sz" -eq "$PART_BYTES" ] || { echo "FEHLER: Ausgabe $out_sz != $PART_BYTES Bytes"; exit 1; }

echo "Fertig: $OUT ($out_sz Bytes)"
echo
echo "Vorschau erzeugen (so sieht es auf dem Geraet aus):"
echo "  head -c $IMG_BYTES \"$OUT\" | ffmpeg -f rawvideo -pix_fmt bgra -s 800x1280 -i - -y vorschau.png"
echo
echo "Schreiben auf die MPC (ACHTUNG: schreibt eine Roh-Partition):"
echo "  scp \"$OUT\" mpcx:/tmp/splash.img"
echo "  ssh mpcx 'dd if=/tmp/splash.img of=/dev/mmcblk0p4 bs=64k && sync'"
echo
echo "Zurueckrollen mit dem Original:"
echo "  scp \"$ORIG\" mpcx:/tmp/orig.img"
echo "  ssh mpcx 'dd if=/tmp/orig.img of=/dev/mmcblk0p4 bs=64k && sync'"
