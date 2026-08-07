#!/usr/bin/env bash
# mpcx-record-audio.sh (macOS) - Screencapture MIT Ton -> ~/Downloads.
#
#   Bild : MPC streamt PNG-Frames ueber SSH (drmrec)         [Netzwerk]
#   Ton  : MPC als USB-Audio-Interface -> avfoundation-Input [USB-Kabel!]
#   -> ffmpeg muxt beides in ein MP4 (H.264 + AAC).
#
# EINMALIG:
#   1) MPC per USB-B ("to computer") an den Mac stecken (Standalone bleibt an,
#      NICHT Controller-Modus). macOS erkennt "MPC X Audio" als Eingang.
#   2) Audio-Geraete-Index finden:
#        ffmpeg -f avfoundation -list_devices true -i "" 2>&1 | grep -i audio
#      Dort die Zeilennummer von "MPC X Audio" merken (z.B. [1]).
#
# Aufruf:  ./mpcx-record-audio.sh <audio_index> [sekunden=20] [fps=12] [offset_sek=0]
#   offset_sek: >0 verzoegert das VIDEO (falls Ton vorlaeuft), <0 verzoegert den TON.
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Audio-Geraete auflisten:"
  ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 | grep -iE "audio|AVFoundation" || true
  echo
  echo "Aufruf: $0 <audio_index> [sekunden=20] [fps=12] [offset_sek=0]"
  exit 1
fi

ADEV="$1"; SEC="${2:-20}"; FPS="${3:-12}"; OFFS="${4:-0}"
ts="$(date +%Y%m%d_%H%M%S)"
out="${HOME}/Downloads/MPCX_${ts}.mp4"

command -v ffmpeg >/dev/null || { echo "ffmpeg fehlt: brew install ffmpeg"; exit 1; }

# Optionaler A/V-Offset: positiver Wert schiebt das Video nach hinten.
voff=""; aoff=""
if awk "BEGIN{exit !($OFFS>0)}"; then voff="-itsoffset ${OFFS}"; fi
if awk "BEGIN{exit !($OFFS<0)}"; then aoff="-itsoffset ${OFFS#-}"; fi

echo "-> Screencapture MIT Ton: ${SEC}s @ ${FPS}fps (Audio-Dev ${ADEV})..."
# Video von SSH-stdin (Wallclock-Timestamps = reale drmrec-Pacing, sync zum Live-Audio).
# -shortest beendet, sobald drmrec (Video) nach SEC Sekunden endet.
ssh mpcx "/data/hacks/drmrec ${SEC} ${FPS} 1" \
  | ffmpeg -hide_banner -loglevel error \
      ${aoff} -f avfoundation -i ":${ADEV}" \
      ${voff} -use_wallclock_as_timestamps 1 -f image2pipe -framerate "${FPS}" -i - \
      -vf transpose=2 \
      -c:v libx264 -pix_fmt yuv420p -preset veryfast \
      -c:a aac -b:a 192k \
      -shortest -movflags +faststart -y "${out}"

echo "Fertig: ${out}"
open "${out}"
