#!/usr/bin/env bash
# mpcx-screenshot.sh (macOS)
# PNG-Screenshot der MPC X -> ~/Downloads, danach von der MPC loeschen + oeffnen.
# Voraussetzung: SSH-Alias "mpcx" (root@<mpc-ip>) in ~/.ssh/config.
set -euo pipefail

ts="$(date +%Y%m%d_%H%M%S)"
name="MPCX_${ts}.png"
remote="/tmp/${name}"
local="${HOME}/Downloads/${name}"

echo "-> Screenshot auf der MPC X aufnehmen..."
ssh mpcx "/data/hacks/drmshot ${remote} 270"

echo "-> In den Downloads-Ordner laden..."
scp -q "mpcx:${remote}" "${local}"

echo "-> Von der MPC loeschen..."
ssh mpcx "rm -f ${remote}" >/dev/null 2>&1 || true

echo "Fertig: ${local}"
open "${local}"
