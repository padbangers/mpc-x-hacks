# mpcx-record.ps1
# Nimmt ein Video des MPC-X-Screens auf und speichert es als MP4 in Downloads.
# Das Gerät streamt PNG-Frames über SSH; ffmpeg (lokal) dreht + kodiert zu H.264.
#
# Aufruf:  powershell -ExecutionPolicy Bypass -File mpcx-record.ps1 -Seconds 20
param(
    [int]$Seconds = 20,   # Aufnahmedauer
    [int]$Fps     = 12    # Bildrate (Gerät schafft ~13)
)
$ErrorActionPreference = 'Stop'

$ff = Join-Path $PSScriptRoot 'tools\ffmpeg.exe'
if (-not (Test-Path $ff)) { throw "ffmpeg fehlt: $ff  (siehe tools/README)" }

$ts    = Get-Date -Format 'yyyyMMdd_HHmmss'
$name  = "MPCX_$ts.mp4"
$out   = Join-Path ([Environment]::GetFolderPath('UserProfile')) "Downloads\$name"

Write-Host "-> Nehme $Seconds s @ $Fps fps auf (MPC-Screen)..." -ForegroundColor Cyan
# binaersicher ueber cmd: ssh(drmrec) | ffmpeg(transpose=2, H.264) -> mp4
$pipe = "ssh mpcx `"/data/hacks/drmrec $Seconds $Fps 1`" | `"$ff`" -hide_banner -loglevel error " +
        "-f image2pipe -framerate $Fps -i - -vf transpose=2 -c:v libx264 -pix_fmt yuv420p -movflags +faststart -y `"$out`""
cmd /c $pipe

if (Test-Path $out) {
    Write-Host "Fertig: $out ($([math]::Round((Get-Item $out).Length/1KB,0)) KB)" -ForegroundColor Green
    Start-Process $out
} else {
    throw "Aufnahme fehlgeschlagen."
}
