# Padbangers — Akai MPC X Button Remap

Custom button remapping for the **Akai MPC X** — remap hardware buttons to jump
straight to your favourite screens and functions. Installs cleanly, survives
reboots, and is fully reversible. **No firmware modification.**

## Default mappings

| Button | Single click | Double click |
|---|---|---|
| **− / +** (under data wheel) | previous / next track | — |
| **Shift + − / +** | previous / next **sequence** | — |
| **Next Seq** | Song | Next Sequence |
| **Pad Perform** | Grid View | Pad Perform |
| **XY FX** | Arrange | Sounds |
| **F-Key** | Add Track | — |

Native **Shift** functions keep working (Shift is passed through; it is only
briefly hidden at the instant of a Shift+−/+ press so the sequence mapping fires).

(All of these are freely changeable — see *Configuration* below.)

## Requirements

- **Akai MPC X** (this build is X-specific — button codes differ on One/Live/Key).
- **Hakai / az0x firmware with root SSH access** (`ssh root@<mpc-ip>`).
- Tested on MPC software 3.6.x. Similar builds should work.

## Install

1. Copy this whole folder onto the MPC (SSH/scp, or a USB stick to `/data`):
   ```
   scp -r padbangers-mpcx-remap root@<mpc-ip>:/data/
   ```
2. SSH into the MPC and run the installer:
   ```
   ssh root@<mpc-ip>
   cd /data/padbangers-mpcx-remap
   sh install.sh
   ```
3. Done — the mappings are live and load automatically on every boot.

> The installer backs up `MPC.settings`, enables SSH-on-boot as a safety net,
> and never touches the read-only firmware.

## How it works (short version)

An `LD_PRELOAD` library (`shim_remap6.so`) loads into the MPC app and intercepts
the control-surface MIDI. Mapped buttons are suppressed and replaced with:
- a **CC** sent from a self-created virtual MIDI input **"Padbangers"** (the MPC's
  own Global-MIDI-mapping turns it into track/sequence/mode actions), or
- an injected **button code** / **Menu+Pad combo** to open any screen directly
  (this is how Arrange / Grid View / Sounds are reached).

Autoload is via `/etc/ld.so.preload`; the "Padbangers" input is seeded as a
`control` device in `MPC.settings`. Both live on persistent storage, so it all
survives reboots.

## Configuration

Edit `/data/hacks/remap.conf`, then apply with `systemctl restart acvs.service`.

Each line: `<surfaceNoteHex> <mode> <arg> [<mode2> <arg2>]`
(3 fields = single action; 5 fields = single + double-click action)

| mode | action |
|---|---|
| `rel+` / `rel-` | relative CC (+1 / −1) — e.g. track/seq step |
| `btn` | momentary CC (open a Global-mapping target once) |
| `note` | inject one surface button code (opens that screen) |
| `note2` | inject a **double-press** of a code (e.g. Grid View = 2× Pad Perform) |
| `menupad` | inject **Menu + Pad<code>** combo (Arrange/Grid View/Sounds) |

Directives / modifiers:
- `shift <hex>` — declare the Shift button note (MPC X = `31`).
- `shiftsuppress 0|1` — `0` = pass Shift to the app (native Shift works) + blip it
  off during a Shift-combo; `1` = hide Shift entirely.
- Prefix a note with `s` for a **Shift-held variant**, e.g. `s36 rel+ 61`
  (Shift + `+` → sequence +1). The plain `36 rel+ 74` handles `+` without Shift.

Reference (MPC X): surface button notes are on channel 0; useful CC targets via
the bundled `Track Remap.xmm` are `74`→track (relative), `101`→Next Sequence,
`102`→Song. Menu = code 123; Pad 14/15/16 = ch9 notes 55 / 51 / 53
(= Arrange / Grid View / Sounds).

## Uninstall

```
cd /data/padbangers-mpcx-remap
sh uninstall.sh
```
Removes the autoload + shim + config and turns Global mapping off again. SSH stays
enabled; settings backups remain in `/data/hacks/backup/`.

## Safety / disclaimer

Does not modify the read-only firmware; everything lives in `/data` and the
writable `/etc` overlay, and is reversible. Use at your own risk. If the MPC app
ever misbehaves, SSH in and run `sh uninstall.sh` (SSH is enabled independently of
the app, so you can't lock yourself out).
