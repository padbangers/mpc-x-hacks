# MPC X Button Remap Shim

An `LD_PRELOAD` shim for the Akai MPC X (Hakai firmware, MPC 3.6.x) that reassigns
hardware buttons — without modifying the firmware and without any external device.

## How it works

The MPC app reads its control surface via `snd_rawmidi_read("hw:2,0,1")`. The shim,
injected into the app via `LD_PRELOAD`:

1. **intercepts** the surface reads and **suppresses** mapped buttons (their default action),
2. creates its own virtual MIDI input called **"Padbangers"** (hardware type, so the app
   enumerates it and enables `control` on it automatically),
3. **sends** a CC from "Padbangers" whenever a mapped button is pressed.

The MPC's **global MIDI mapping** (`.xmm`, enabled in `MPC.settings`) maps that CC to an
MPC function (change track, open a page, …). This turns buttons into arbitrary functions —
including **single/double click** (double click = second function).

## Files on the device

| Path | Purpose |
|---|---|
| `/data/hacks/shim_remap9.so` | the shim (built from `shim_remap9.c`) |
| `/data/hacks/remap.conf` | button→CC assignments |
| `/media/az01-internal-sd/MPC Documents/Midi Learn/Track Remap.xmm` | CC→function (global mapping) |
| `/media/az01-internal/Settings/MPC/MPC.settings` | `MidiLearnEnabled=1` + path to the `.xmm`; **"Padbangers" `control:true`** (written permanently) |
| `/etc/ld.so.preload` | **autostart**: contains `/data/hacks/shim_remap9.so` → loads the shim when the MPC app starts |
| `/data/hacks/backup/` | backups (MPC.settings, startup script, .xmm) |

## Building (cross compile with Zig)

```
zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 \
    -o shim_remap9.so shim_remap9.c -ldl -lpthread
```
Deploy: `scp`/pipe to `/data/hacks/shim_remap9.so`.

Switching versions safely: `/data/hacks/switch-shim.sh <new.so> <old.so>` writes `/etc/ld.so.preload`, restarts
the MPC app and goes back to the old shim if the app doesn't come up with the new one. v6, v7 and v8 stay on the
device as fallbacks.

## Pad bank lock (v7-v9)

The app jumps to pad bank C when Grid view or Pad Perform opens. The shim keeps the bank the user chose (A at start):

- It watches the bank LEDs the app writes to the controller: CC 0x23-0x26 on channel 1 (A-D), value != 0 = lit.
  v8 parses the stream as MIDI with running status: the app sends `b0 23 00 25 03` (A off, C on) in one message,
  which v7 missed.
- When the app lights another bank than the user's, the shim injects a press of the user's bank into the surface
  stream. v9 does that at once: the app's MIDI thread waits in `poll()` on the surface fd; the shim adds a wake pipe
  to that poll, so the thread wakes up and the read hook hands it the press (v7/v8 waited for the next button press,
  so bank C showed until then).
- The user's own presses are guarded for 400 ms; Shift+bank (E-H) suspends the lock until a plain A-D press.
  `banklock 0` in `remap.conf` turns it off.
- After the user picks a bank inside Pad Perform, the app itself keeps it; the lock matters for the first opening.

## Config format (`remap.conf`)

```
<noteHex> <mode> <cc> [<mode2> <cc2>]
```
- 3 fields = single click only (fires immediately)
- 5 fields = single + **double click** (single is delayed by the click window, default 350 ms)
- `mode`: `rel+` (value 1) · `rel-` (value 127) · `btn` (127 then 0, momentary)
- `cc` must be bound to a `Target_control` in `Track Remap.xmm`.

**Currently active:**
```
36 rel+ 74          # + button -> track +1
37 rel- 74          # - button -> track -1
2a btn 102 btn 101  # Next Seq: 1x -> Song(102), 2x -> Next Sequence(101)
```

## Known button codes (ch0 surface notes)

| Button | Note | | Button | Note |
|---|---|---|---|---|
| Pad Perform | 0x7f | | Sampler | 0x7d |
| XY FX | 0x7e | | F-Key | 0x4e |
| Next Seq | 0x2a | | (+ / −) | 0x36 / 0x37 |

## Available targets (usable as CC in the `.xmm`)

**Actions (momentary/relative, targets 41–62):** 41 Play · 42 Stop · 43 Play Start · 48 Record ·
50 Overdub · 52 Tap Tempo · 59 Undo · 60 Redo · **61 Change Sequence** (relative) ·
**62 Change Track** (relative).

**Pages/modes (momentary, `btn`; targets):**
| # | Page | # | Page |
|---|---|---|---|
| 91 | Track View | 98 | Looper |
| 92 | Track Edit | 99 | Pad Mute |
| 93 | Sample Edit | 100 | Track Mute |
| 94 | Pad Mixer | 101 | Next Sequence |
| 95 | Track Mixer | 102 | Song |
| 96 | Step Sequencer | 103 | Midi Mapping |
| 97 | Sampler | | |

`.xmm` binding: `Mapping_type=2` (CC), `Mapping_channel=1`, `Mapping_data1=<cc>`,
`Mapping_control=2` (momentary) or `7` (relative, 2's complement). Injection channel = 0 =
MIDI channel 1.

## Changing mappings

1. Edit `remap.conf` (and extend `Track Remap.xmm` for any new targets).
2. Reload the MPC app: `systemctl restart acvs.service` (the config is read at startup).

## Persistence (survives real reboots)

Two parts — **both are required**:

1. **`/etc/ld.so.preload`** contains `/data/hacks/shim_remap9.so`. The dynamic linker loads
   the shim at program start. *(A systemd drop-in does NOT work across reboots: `/etc` is an
   overlay on `/data`, and `etc.mount` only runs after systemd parses its units → drop-ins are
   invisible at boot until a manual `daemon-reload`. `ld.so.preload` is read at `exec` time, by
   which point the overlay is long since mounted.)* The shim is **ld.so.preload-safe**: in every
   process other than `MPC` it is completely inert (`comm` guard).
2. **Padbangers `control:true` written permanently into `MPC.settings`** (the app only saves the
   device list on a clean shutdown; a reboot does not save, so the flag would otherwise be lost).
   Written as an object `{type:0, deviceName:Padbangers, deviceId:129-0, track:false,
   control:true, …}` directly after `value0": [`. Padbangers is reliably ALSA client **129** (the
   shim creates it in the MPC constructor, after DeviceControlServer=128).

## Rollback (undo everything)

```
rm /etc/ld.so.preload          # remove autostart
rm -rf /data/hacks             # remove shim + config
# in MPC.settings: set MidiLearnEnabled to 0 (optional), remove the Padbangers entry (optional)
systemctl restart acvs.service # back to stock behaviour
```
SSH comes up independently (Hakai's `hakai-init` starts sshd; additionally
`systemctl enable sshd`) → there is no way to lock yourself out.

## Still open (next phase)

- **Missing pages** (Grid View, Pad Perform, Sounds, Arrange) are **not** available as global
  mapping targets → they would need **touchscreen injection** via `/dev/uinput` (calibrated per
  page).
- Assign the remaining buttons: Pad Perform, XY FX, Sampler, F-Key.
- Optional: config hot reload (without restarting the app).
