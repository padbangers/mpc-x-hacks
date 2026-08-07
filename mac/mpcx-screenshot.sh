#!/usr/bin/env bash
# mpcx-screenshot.sh (macOS)
# PNG screenshot of the MPC X -> ~/Downloads, then delete it from the MPC and open it.
# Requires: SSH alias "mpcx" (root@<mpc-ip>) in ~/.ssh/config.
set -euo pipefail

ts="$(date +%Y%m%d_%H%M%S)"
name="MPCX_${ts}.png"
remote="/tmp/${name}"
local="${HOME}/Downloads/${name}"

echo "-> Taking screenshot on the MPC X..."
ssh mpcx "/data/hacks/drmshot ${remote} 270"

echo "-> Downloading to the Downloads folder..."
scp -q "mpcx:${remote}" "${local}"

echo "-> Deleting from the MPC..."
ssh mpcx "rm -f ${remote}" >/dev/null 2>&1 || true

echo "Done: ${local}"
open "${local}"
