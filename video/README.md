# MPC X Video Capture

Records the MPC X screen to an MP4. The device streams **PNG frames over SSH**;
your PC rotates + encodes them to H.264 with **ffmpeg** — this keeps the weak
RK3288 CPU out of the H.264 path.

## Why this design
The scanout framebuffer is linear XRGB8888 but **uncached**, so per-pixel reads
are very slow. `drmrec` bulk-`memcpy`s each frame into cached RAM, builds RGB
scanlines and deflates a PNG (zlib level 1). It does **not** rotate on-device
(the transposed read is the real bottleneck) — ffmpeg rotates with `transpose=2`.
Measured throughput: **~12–13 fps** at 1280×800.

## On-device
- `drmrec [seconds 0=inf] [fps 12] [zlevel 1] [card]` — streams concatenated PNGs
  to stdout (SIGINT/TERM stops). Deployed at `/data/hacks/drmrec`.

Build (Zig): `zig cc -target arm-linux-gnueabihf.2.31 -O2 -o drmrec drmrec.c ./libdrm.so.2 ./libz.so.1 -Wl,--allow-shlib-undefined`

## From your PC
`mpcx-record.ps1` (repo root):
```
powershell -ExecutionPolicy Bypass -File mpcx-record.ps1 -Seconds 20
```
or the VS Code task **"MPC X: Record video -> Downloads"** (prompts for seconds).
It runs, via a binary-safe `cmd` pipe:
```
ssh mpcx "/data/hacks/drmrec <sec> <fps> 1" | ffmpeg -f image2pipe -framerate <fps> -i - -vf transpose=2 -c:v libx264 -pix_fmt yuv420p out.mp4
```
Needs `tools/ffmpeg.exe` (see tools/README.md). Output lands in your Downloads
folder and opens automatically.

## Not yet included
- **Audio** (MockbaMod's `vca.sh` grabs ALSA `hw:2`) — would need a second stream
  muxed in; doable as a follow-up.
- Higher fps via a producer/consumer thread (grab ∥ encode → ~20 fps).
