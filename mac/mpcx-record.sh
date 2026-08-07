#!/usr/bin/env bash
# mpcx-record.sh (macOS) - Video des MPC-X-Screens (OHNE Ton) -> ~/Downloads.
# Geraet streamt PNG-Frames ueber SSH; ffmpeg dreht (transpose=2) + kodiert H.264.
# Aufruf:  ./mpcx-record.sh [sekunden=20] [fps=12]
set -euo pipefail

SEC="${1:-20}"
FPS="${2:-12}"
ts="$(date +%Y%m%d_%H%M%S)"
out="${HOME}/Downloads/MPCX_${ts}.mp4"

command -v ffmpeg >/dev/null || { echo "ffmpeg fehlt: brew install ffmpeg"; exit 1; }

echo "-> Nehme ${SEC}s @ ${FPS}fps auf (MPC-Screen)..."
# bash-Pipe ist binaersicher (anders als PowerShell) -> direkt ssh | ffmpeg
ssh mpcx "/data/hacks/drmrec ${SEC} ${FPS} 1" \
  | ffmpeg -hide_banner -loglevel error \
      -f image2pipe -framerate "${FPS}" -i - \
      -vf transpose=2 -c:v libx264 -pix_fmt yuv420p -movflags +faststart -y "${out}"

echo "Fertig: ${out}"
open "${out}"
