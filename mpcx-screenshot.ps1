# mpcx-screenshot.ps1
# Nimmt einen PNG-Screenshot der MPC X auf, lädt ihn in den Downloads-Ordner,
# löscht ihn danach von der MPC und öffnet das Bild.
#
# Voraussetzung: SSH-Alias "mpcx" in ~/.ssh/config (root@<mpc-ip>).
# Aufruf:  powershell -ExecutionPolicy Bypass -File mpcx-screenshot.ps1

$ErrorActionPreference = 'Stop'

$ts    = Get-Date -Format 'yyyyMMdd_HHmmss'
$name  = "MPCX_$ts.png"
$remote = "/tmp/$name"
$dl    = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Downloads'
$local = Join-Path $dl $name

Write-Host "-> Screenshot auf der MPC X aufnehmen..." -ForegroundColor Cyan
ssh mpcx "/data/hacks/drmshot $remote 270"
if ($LASTEXITCODE -ne 0) { throw "Aufnahme auf der MPC fehlgeschlagen (SSH/drmshot)." }

Write-Host "-> In den Downloads-Ordner laden..." -ForegroundColor Cyan
scp -q "mpcx:$remote" "$local"
if ($LASTEXITCODE -ne 0) { throw "Download (scp) fehlgeschlagen." }

Write-Host "-> Von der MPC löschen..." -ForegroundColor Cyan
ssh mpcx "rm -f $remote" | Out-Null

Write-Host ""
Write-Host "Fertig: $local" -ForegroundColor Green

# Bild direkt öffnen
Start-Process $local
