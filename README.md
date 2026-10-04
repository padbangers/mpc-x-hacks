# Padbangers MPC X Hacks

Modifications for the **Akai MPC X** running in standalone mode — button remapping,
screen capture, and a custom boot logo. Everything runs over SSH on the device
itself; no MPC-app patching, no firmware flashing.

> **Device:** Akai MPC X · Hakai firmware (MPC app 3.6.x) · Rockchip RK3288 (armv7l)
> · Linux 6.18 PREEMPT_RT · 1280×800 display (panel natively 800×1280 portrait)

---

## Ground rules

These apply to everything in this repo:

- **The read-only firmware is never modified.** All changes live in `/data` and the
  `/etc` overlay, and are reversible.
- **Restarting the MPC app kills unsaved projects.** `systemctl restart acvs.service`
  and any reboot terminate the app. Save first.
- **`/etc` mounts late in boot.** systemd drop-ins placed there are not seen during
  early boot — a persistence trap worth remembering for any boot-time hook.

---

## Contents

| Directory | What it is |
|---|---|
| [`shim/`](shim/) | Button-remap shim source (`shim_remap9.c` with the pad bank lock, `remap.conf`) |
| [`padbangers-mpcx-remap/`](padbangers-mpcx-remap/) | Shareable installer package for the remap (install/uninstall + sources) |
| [`bootlogo/`](bootlogo/) | Custom boot splash: guide, build script, original backups |
| [`screenshot/`](screenshot/) | `drmshot.c` — DRM/KMS framebuffer → PNG |
| [`video/`](video/) | `drmrec.c` — DRM → PNG frame stream over SSH |
| [`mac/`](mac/) | macOS scripts: screenshot and video capture |
| [`sniff/`](sniff/) | `cvsniff.c` — logger for what the MPC app writes to / reads from its hardware (found the CV protocol) |
| Glass Spheres | Mutable Instruments Marbles as an MPC plugin, MIDI or CV/Gate out (ex Marbles CV), plus the CV protocol notes — own repo: [FullPace/glass-spheres](https://github.com/FullPace/glass-spheres) |
| Overcast | Clouds (Parasites firmware) as an MPC insert effect with a modulation page — own repo: [FullPace/overcast](https://github.com/FullPace/overcast) |
| [`docs/`](docs/) | Device architecture and SSH access notes — the full context |
| [`tools/`](tools/) | Windows ffmpeg binary (gitignored) |
| `*.ps1` | Windows counterparts of the capture scripts |

---

## The hacks

### Button remap — working, survives reboots

Repurposes hardware buttons via an `LD_PRELOAD` shim plus a virtual MIDI port
("Padbangers") and a global MIDI mapping:

- `−` / `+` → previous / next **track**
- **`Shift` + `−` / `+` → previous / next sequence** (native Shift behaviour preserved)
- Next Seq · Pad Perform · XY FX · F-Key → jump to their screens

Install via [`padbangers-mpcx-remap/`](padbangers-mpcx-remap/), source in
[`shim/`](shim/).

### Boot logo — working

Replaces the boot splash in the raw `splash` partition (`mmcblk0p4`). The format
turned out to be **raw BGRA pixels at 800×1280** — no BMP, no header — followed by
96 KB of metadata that must be preserved.

- **[`bootlogo/GUIDE.md`](bootlogo/GUIDE.md)** — step-by-step, no scripts needed
  (also in German: [`ANLEITUNG.md`](bootlogo/ANLEITUNG.md))
- [`bootlogo/make-splash.sh`](bootlogo/make-splash.sh) — image → writable splash image
- [`bootlogo/backup/`](bootlogo/backup/) — **original partitions**, checksum-verified.
  Keep these; a firmware update can overwrite the partition.

### Screen capture — working

- **Screenshot** ([`screenshot/drmshot.c`](screenshot/drmshot.c)): reads the DRM/KMS
  scanout buffer, writes PNG. Works while the MPC app renders.
- **Video** ([`video/drmrec.c`](video/drmrec.c)): streams PNG frames over SSH, muxed
  to H.264 on the host.

**Throughput is the limit:** frames are uncompressed over the network, so `drmrec`
sustains roughly **6 fps**. Asking for 12 fps makes a recording run far longer than
requested (30 s → ~37 s) instead of running faster.

---

## Getting started

```
# ~/.ssh/config
Host mpcx
    HostName <MPC-IP>
    User root
```

```
ssh mpcx uptime          # check access
brew install ffmpeg      # host-side, the MPC has none
chmod +x mac/*.sh

./mac/mpcx-screenshot.sh
./mac/mpcx-record.sh 20 6
```

Device-side tools live in `/data/hacks/`.

---

## Building device binaries

Cross-compiled with **Zig 0.16.0**, linking against `.so` files copied from the
device:

```
zig cc -target arm-linux-gnueabihf.2.31 -O2 -o drmrec drmrec.c \
  ./libdrm.so.2 ./libz.so.1 -Wl,--allow-shlib-undefined
```

Details in [`padbangers-mpcx-remap/src/BUILD.md`](padbangers-mpcx-remap/src/BUILD.md).

---

## Notes for the curious

A few findings that cost time and are worth knowing up front:

- The device runs **BusyBox** — `dd` has no `conv=fsync`, `head` needs `-n`, `od`
  lacks `-A`/`-t`. Commands copied from a desktop Linux guide will fail.
- There is **no ffmpeg on the device**, and no other media player. Anything
  involving decoding has to happen on the host.
- `/dev/block/by-name/` does not exist; partitions are identified by size.
- The splash partitions are the only two at exactly 4096 KB (`p4` = boot splash,
  `p5` = the red "UPDATE MODE" screen).

---

## Status

| Feature | State |
|---|---|
| Button remap incl. Shift combos | working, persistent |
| Screenshot | working |
| Video capture | working (~6 fps, network-bound) |
| Custom boot logo | working, written and verified |
| CV/Gate outputs from our own code | protocol verified (14 bit, 1 V/oct); used by [Glass Spheres](https://github.com/FullPace/glass-spheres) (1.0.0 beta) |
| Pad bank lock (Grid view / Pad Perform keep the bank) | working (remap shim v9) |
| Overcast (Clouds/Parasites insert effect) | working; lives in [FullPace/overcast](https://github.com/FullPace/overcast) |
| Animated boot logo | not attempted — would need a raw-frame player on the device and an early boot hook |
