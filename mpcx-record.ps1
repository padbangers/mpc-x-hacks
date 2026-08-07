# mpcx-record.ps1
# Records a video of the MPC X screen and saves it as MP4 in Downloads.
# The device streams PNG frames over SSH; ffmpeg (local) rotates + encodes to H.264.
#
# NOTE fps: drmrec is network-bound and sustains ~6 fps. Higher values make the
# recording run LONGER than requested instead of faster.
#
# Usage:  powershell -ExecutionPolicy Bypass -File mpcx-record.ps1 -Seconds 20
param(
    [int]$Seconds = 20,   # recording duration
    [int]$Fps     = 6     # frame rate (network-bound, see note above)
)
$ErrorActionPreference = 'Stop'

$ff = Join-Path $PSScriptRoot 'tools\ffmpeg.exe'
if (-not (Test-Path $ff)) { throw "ffmpeg missing: $ff  (see tools/README)" }

$ts    = Get-Date -Format 'yyyyMMdd_HHmmss'
$name  = "MPCX_$ts.mp4"
$out   = Join-Path ([Environment]::GetFolderPath('UserProfile')) "Downloads\$name"

Write-Host "-> Recording $Seconds s @ $Fps fps (MPC screen)..." -ForegroundColor Cyan
# binary-safe via cmd: ssh(drmrec) | ffmpeg(transpose=2, H.264) -> mp4
$pipe = "ssh mpcx `"/data/hacks/drmrec $Seconds $Fps 1`" | `"$ff`" -hide_banner -loglevel error " +
        "-f image2pipe -framerate $Fps -i - -vf transpose=2 -c:v libx264 -pix_fmt yuv420p -movflags +faststart -y `"$out`""
cmd /c $pipe

if (Test-Path $out) {
    Write-Host "Done: $out ($([math]::Round((Get-Item $out).Length/1KB,0)) KB)" -ForegroundColor Green
    Start-Process $out
} else {
    throw "Recording failed."
}
