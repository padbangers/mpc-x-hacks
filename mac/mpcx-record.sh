#!/usr/bin/env bash
# mpcx-record.sh (macOS) - video of the MPC X screen (NO audio) -> ~/Downloads.
# The device streams PNG frames over SSH; ffmpeg rotates (transpose=2) and encodes H.264.
#
# NOTE fps: drmrec is network-bound and sustains ~6 fps. Higher values make the
# recording run LONGER than requested instead of faster.
#
# Usage:  ./mpcx-record.sh [seconds=20] [fps=6]
set -euo pipefail

SEC="${1:-20}"
FPS="${2:-6}"
ts="$(date +%Y%m%d_%H%M%S)"
out="${HOME}/Downloads/MPCX_${ts}.mp4"

command -v ffmpeg >/dev/null || { echo "ffmpeg missing: brew install ffmpeg"; exit 1; }

echo "-> Recording ${SEC}s @ ${FPS}fps (MPC screen)..."
# The bash pipe is binary-safe (unlike PowerShell) -> pipe ssh | ffmpeg directly
ssh mpcx "/data/hacks/drmrec ${SEC} ${FPS} 1" \
  | ffmpeg -hide_banner -loglevel error \
      -f image2pipe -framerate "${FPS}" -i - \
      -vf transpose=2 -c:v libx264 -pix_fmt yuv420p -movflags +faststart -y "${out}"

echo "Done: ${out}"
open "${out}"
