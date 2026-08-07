# MPC X Button-Remap Shim

LD_PRELOAD-Shim für die Akai MPC X (Hakai-Firmware, MPC 3.6.x), die Hardware-Buttons
umbelegt — ohne die Firmware zu verändern, ohne externes Gerät.

## Funktionsprinzip

Die MPC-App liest ihr Bedienfeld über `snd_rawmidi_read("hw:2,0,1")`. Die Shim (in die
App per `LD_PRELOAD` injiziert):

1. **fängt** die Surface-Reads ab und **unterdrückt** gemappte Buttons (deren Default-Aktion),
2. erzeugt einen eigenen virtuellen MIDI-Eingang **„Padbangers"** (hardware-Typ, damit die
   App ihn enumeriert + automatisch `control`-aktiviert),
3. **sendet** bei einem gemappten Button ein CC aus „Padbangers".

Die MPC-**Global-MIDI-Zuordnung** (`.xmm`, aktiviert in `MPC.settings`) bildet dieses CC auf
eine MPC-Funktion ab (Track wechseln, Seite öffnen, …). So werden Buttons zu beliebigen
Funktionen — inkl. **Single-/Doppelklick** (Doppelklick = zweite Funktion).

## Dateien auf dem Gerät

| Pfad | Zweck |
|---|---|
| `/data/hacks/shim_remap6.so` | die Shim (aus `shim_remap6.c`) |
| `/data/hacks/remap.conf` | Button→CC-Zuordnungen |
| `/media/az01-internal-sd/MPC Documents/Midi Learn/Track Remap.xmm` | CC→Funktion (Global-Mapping) |
| `/media/az01-internal/Settings/MPC/MPC.settings` | `MidiLearnEnabled=1` + Pfad zur `.xmm`; **„Padbangers" `control:true`** (fest eingetragen) |
| `/etc/ld.so.preload` | **Autostart**: enthält `/data/hacks/shim_remap6.so` → lädt die Shim beim MPC-Start |
| `/data/hacks/backup/` | Backups (MPC.settings, Startskript, .xmm) |

## Bauen (Cross-Compile mit Zig)

```
zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 \
    -o shim_remap6.so shim_remap6.c -ldl -lpthread
```
Deploy: `scp`/pipe nach `/data/hacks/shim_remap6.so`.

## Config-Format (`remap.conf`)

```
<noteHex> <mode> <cc> [<mode2> <cc2>]
```
- 3 Felder = nur Single-Klick (feuert sofort)
- 5 Felder = Single + **Doppelklick** (Single verzögert um das Klick-Fenster, Standard 350 ms)
- `mode`: `rel+` (Wert 1) · `rel-` (Wert 127) · `btn` (127 dann 0, momentan)
- `cc` muss in `Track Remap.xmm` auf ein `Target_control` gebunden sein.

**Aktuell aktiv:**
```
36 rel+ 74          # + Taste  -> Track +1
37 rel- 74          # - Taste  -> Track -1
2a btn 102 btn 101  # Next Seq: 1x -> Song(102), 2x -> Next Sequence(101)
```

## Erfasste Button-Codes (ch0-Surface-Notes)

| Button | Note | | Button | Note |
|---|---|---|---|---|
| Pad Perform | 0x7f | | Sampler | 0x7d |
| XY FX | 0x7e | | F-Key | 0x4e |
| Next Seq | 0x2a | | (+ / −) | 0x36 / 0x37 |

## Verfügbare Ziele (im `.xmm` als CC nutzbar)

**Aktionen (momentan/relativ, Targets 41–62):** 41 Play · 42 Stop · 43 Play Start · 48 Record ·
50 Overdub · 52 Tap Tempo · 59 Undo · 60 Redo · **61 Change Sequence** (relativ) · **62 Change Track** (relativ).

**Seiten/Modi (momentan, `btn`; Targets):**
| # | Seite | # | Seite |
|---|---|---|---|
| 91 | Track View | 98 | Looper |
| 92 | Track Edit | 99 | Pad Mute |
| 93 | Sample Edit | 100 | Track Mute |
| 94 | Pad Mixer | 101 | Next Sequence |
| 95 | Track Mixer | 102 | Song |
| 96 | Step Sequencer | 103 | Midi Mapping |
| 97 | Sampler | | |

`.xmm`-Bindung: `Mapping_type=2` (CC), `Mapping_channel=1`, `Mapping_data1=<cc>`,
`Mapping_control=2` (momentan) bzw. `7` (relativ, 2's-complement). Injektions-Kanal = 0 = MIDI-Kanal 1.

## Mappings ändern

1. `remap.conf` editieren (und bei neuen Zielen `Track Remap.xmm` erweitern).
2. MPC-App neu laden: `systemctl restart acvs.service` (Config wird beim Start gelesen).

## Persistenz (überlebt echte Reboots)

Zwei Teile — **beide nötig**:

1. **`/etc/ld.so.preload`** enthält `/data/hacks/shim_remap6.so`. Der Dynamic Linker lädt die
   Shim beim Programmstart. *(Ein systemd-Drop-in funktioniert NICHT über Reboots: `/etc` ist ein
   Overlay auf `/data`, und `etc.mount` läuft erst nach systemds Unit-Parse → Drop-ins sind beim
   Boot unsichtbar bis zu einem manuellen `daemon-reload`. `ld.so.preload` wird beim `exec` gelesen,
   da ist das Overlay längst da.)* Die Shim ist **ld.so.preload-sicher**: in allen Prozessen außer
   `MPC` komplett inert (`comm`-Guard).
2. **Padbangers `control:true` fest in `MPC.settings`** (die App speichert die Geräteliste nur bei
   sauberem Beenden; ein Reboot speichert nicht → sonst wäre das Flag weg). Eingetragen als Objekt
   `{type:0, deviceName:Padbangers, deviceId:129-0, track:false, control:true, …}` direkt nach
   `value0": [`. Padbangers ist zuverlässig ALSA-Client **129** (Shim erzeugt ihn im MPC-Konstruktor,
   nach DeviceControlServer=128).

## Rollback (alles rückgängig)

```
rm /etc/ld.so.preload          # Autostart weg
rm -rf /data/hacks             # Shim + Config weg
# in MPC.settings: MidiLearnEnabled auf 0 (optional), Padbangers-Eintrag entfernen (optional)
systemctl restart acvs.service # Original-Betrieb
```
SSH kommt unabhängig hoch (Hakai-`hakai-init` startet sshd; zusätzlich `systemctl enable sshd`) →
man sperrt sich nicht aus.

## Noch offen (nächste Phase)

- **Fehlende Seiten** (Grid View, Pad Perform, Sounds, Arrange) sind **nicht** als Global-Mapping-
  Ziel schaltbar → brauchen **Touchscreen-Injektion** über `/dev/uinput` (pro Seite kalibrieren).
- Restliche Buttons zuweisen: Pad Perform, XY FX, Sampler, F-Key.
- Optional: Config-Hot-Reload (ohne App-Neustart).
