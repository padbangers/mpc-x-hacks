# MPC X Screenshot

Grabs the MPC X screen straight from the DRM scanout framebuffer (no ffmpeg
needed — the primary plane is linear XRGB8888 800×1280). Output is a **PNG**,
rotated to 1280×800, ~100–400 KB.

## On-device
- `drmshot <out.png> [rot 0|90|180|270] [card]` — the grabber (links device
  `libdrm` + `libz`; **rot 270** = correct MPC X orientation). Deployed at
  `/data/hacks/drmshot`.
- `drmq [card]` — debug: prints active plane format/modifier.
- `/data/hacks/screenshot.sh` — saves a timestamped PNG to `MPC SSD/Screenshots/`.

Build (Zig):
```
zig cc -target arm-linux-gnueabihf.2.31 -O2 -o drmshot drmshot.c \
    ./libdrm.so.2 ./libz.so.1 -Wl,--allow-shlib-undefined
```

## From your PC (recommended)
`mpcx-screenshot.ps1` (repo root) does it all in one go:
**capture → download to your Downloads folder → delete from the MPC → open the image.**

Run it via the **VS Code task** "MPC X: Screenshot -> Downloads"
(Terminal → Run Task…), or directly:
```
powershell -ExecutionPolicy Bypass -File mpcx-screenshot.ps1
```
Requires the `mpcx` SSH alias in `~/.ssh/config`.
