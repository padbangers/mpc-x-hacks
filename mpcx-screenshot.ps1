# mpcx-screenshot.ps1
# Takes a PNG screenshot of the MPC X, downloads it to the Downloads folder,
# then deletes it from the MPC and opens the image.
#
# Requires: SSH alias "mpcx" in ~/.ssh/config (root@<mpc-ip>).
# Usage:  powershell -ExecutionPolicy Bypass -File mpcx-screenshot.ps1

$ErrorActionPreference = 'Stop'

$ts    = Get-Date -Format 'yyyyMMdd_HHmmss'
$name  = "MPCX_$ts.png"
$remote = "/tmp/$name"
$dl    = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Downloads'
$local = Join-Path $dl $name

Write-Host "-> Taking screenshot on the MPC X..." -ForegroundColor Cyan
ssh mpcx "/data/hacks/drmshot $remote 270"
if ($LASTEXITCODE -ne 0) { throw "Capture on the MPC failed (SSH/drmshot)." }

Write-Host "-> Downloading to the Downloads folder..." -ForegroundColor Cyan
scp -q "mpcx:$remote" "$local"
if ($LASTEXITCODE -ne 0) { throw "Download (scp) failed." }

Write-Host "-> Deleting from the MPC..." -ForegroundColor Cyan
ssh mpcx "rm -f $remote" | Out-Null

Write-Host ""
Write-Host "Done: $local" -ForegroundColor Green

# Open the image right away
Start-Process $local
