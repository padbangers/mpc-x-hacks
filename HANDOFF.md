# MPC X Hacks — Handoff (Stand 2026-08-07)

Übergabe, um am **Mac** nahtlos weiterzumachen. Kontext-Details für eine neue
Claude-Session: [`docs/mpc-x-architecture.md`](docs/mpc-x-architecture.md) (die komplette
Geräte-/Firmware-Analyse) und [`docs/mpc-x-ssh-access.md`](docs/mpc-x-ssh-access.md).

## Gerät / Zugang
- Akai **MPC X**, Hakai-Firmware (MPC-App 3.6.x), RK3288 / armv7l, kernel 6.18 RT.
- SSH: `root@192.168.178.58`, Key-Auth, Alias **`mpcx`**. **Am Mac neu einrichten**
  (`~/.ssh/config` + Key, siehe `mac/README.md`). Test: `ssh mpcx uptime`.
- Auf dem Gerät: alle Custom-Tools unter **`/data/hacks/`** (`drmshot`, `drmrec`, `drmq`,
  `shim_remap6.so`, `remap.conf`, `arec`), Backups in `/data/hacks/backup/`.
- **Nie** die read-only Firmware anfassen; alles in `/data` + `/etc`-Overlay, reversibel.
- **Vor App-Neustart warnen** (`systemctl restart acvs.service` stoppt die MPC-App →
  ungespeicherte Projekte verloren).

## Fertig & persistent (überlebt Reboots)
- **Button-Remap** (LD_PRELOAD-Shim `shim_remap6.so` + virtueller MIDI-Port „Padbangers" +
  Global-MIDI-Mapping `.xmm`): `−/+` = prev/next Track; **`Shift + −/+` = prev/next Sequenz**
  (native Shift bleibt erhalten); Next Seq / Pad Perform / XY FX / F-Key → Screen-Jumps.
  Quelle: `shim/`, Installer: `padbangers-mpcx-remap/`.
- **Screenshot** (`screenshot/drmshot.c`): DRM/KMS → PNG, PC-Script `mpcx-screenshot.ps1`.
- **Video** (`video/drmrec.c`): DRM → PNG-Stream über SSH → ffmpeg (`transpose=2`, H.264),
  ~12–13 fps, PC-Script `mpcx-record.ps1`.

## Aktuelle Aufgabe: **Screencapture MIT Ton**  ← hier weitermachen

### Audio-Analyse (erledigt, 2026-08-07) — Ergebnis
Der Force-Trick (`ffmpeg -f alsa -i hw:2`) geht auf der MPC X **nicht**:
- Kein `arecord`, kein `snd-aloop`-Loopback.
- MPC-App hält **card1** exklusiv: Playback **8ch** (= Ausgang/Musik), Capture **4ch**
  (= Line/Mic-Eingänge). Direktes Öffnen → **`Device or resource busy`** (Testtool
  `audio/arec.c`). Der Ausgang ist per ALSA nicht aufnehmbar.

### Entscheidung des Users: **natives USB-Audio am Host** (risikofrei, kein App-Eingriff)
Die MPC X ist selbst ein USB-Audio-Interface → per USB an den Rechner erscheint
„MPC X Audio" als Aufnahme-Eingang (= echte interne Musik). Bild weiter via `drmrec` (Netz),
Ton via Host-Audio, in **einem ffmpeg** gemuxt.

### Nächste Schritte am Mac (genau hier ansetzen)
1. SSH-Alias `mpcx` am Mac einrichten (siehe `mac/README.md`), `brew install ffmpeg`.
2. MPC per **USB-B ("to computer")** an den Mac (Standalone bleibt an, **nicht**
   Controller-Modus — der würde Audio/Surface vom internen System abkoppeln).
3. Audio-Index finden: `ffmpeg -f avfoundation -list_devices true -i "" 2>&1 | grep -i audio`.
4. Testaufnahme: **`./mac/mpcx-record-audio.sh <index> 20 12`** → `~/Downloads/MPCX_*.mp4`.
5. **A/V-Sync** prüfen; bei Versatz 4. Argument `offset_sek` nutzen (z.B. `... 1 20 12 0.3`).
   `mpcx-record-audio.sh` nutzt schon `-use_wallclock_as_timestamps 1` fürs Video +
   `-shortest`, damit Bild/Ton auf realer Zeit laufen.

Script liegt bereit: [`mac/mpcx-record-audio.sh`](mac/mpcx-record-audio.sh). Hintergrund:
[`audio/README.md`](audio/README.md).

### Fallback (nur falls kein USB-Audio gewünscht): In-App-LD_PRELOAD-Tap
Separate Preload-Lib `atap.so`, hookt `snd_pcm_writei` in der App, kopiert ch0/1 (Main L/R)
aus dem 8ch-Ausgang lock-frei in einen Ringpuffer (nur wenn ein Reader verbunden ist), ein
Drainer-Thread schreibt S16LE-stereo-44,1k in FIFO `/tmp/mpc_audio`; Hot-Path blockiert nie,
im Normalbetrieb inaktiv. PC liest FIFO über SSH parallel zum `drmrec`-Video, ffmpeg muxt.
Kleiner Echtzeit-Audio-Rest-Risiko → wurde zugunsten USB-Audio verworfen, bleibt der einzige
netzwerk-only-Weg.

## Danach offen (User-Plan Punkt 3): **Bootlogo**
Plan liegt in [`capture-and-bootlogo-plan.md`](capture-and-bootlogo-plan.md). Kurz: statischer
Splash in Partition `mmcblk0p4` (`splash`) ersetzen, oder Video→`/dev/fb0`. Achtung:
`/etc`-Overlay mountet spät im Boot (gleiche Persistenz-Falle wie bei der Shim).

## Repo-Struktur
```
shim/                     Remap-Shim Quelle (shim_remap6.c, remap.conf)
padbangers-mpcx-remap/    teilbares Installer-Paket (install/uninstall/README/files/src)
screenshot/               drmshot.c (DRM->PNG), drmq.c
video/                    drmrec.c (DRM->PNG-Stream), drmq.c
audio/                    arec.c (ALSA-Capture-Test) + Analyse/Entscheidung
mac/                      macOS-Scripte (screenshot, record, record-audio) + README
docs/                     Geräte-Architektur- & SSH-Notizen (voller Kontext)
tools/                    ffmpeg.exe (Windows, gitignored) + README
*.ps1                     Windows-Scripte
button-remap-plan.md, capture-and-bootlogo-plan.md
```

## Build-Toolchain (für Geräte-Binaries)
Zig **0.16.0** cross-compile, z.B.:
`zig cc -target arm-linux-gnueabihf.2.31 -O2 -o drmrec drmrec.c ./libdrm.so.2 ./libz.so.1 -Wl,--allow-shlib-undefined`.
Braucht die **Geräte-`.so`** (libasound/libdrm/libz — per `scp` von der MPC holen). Details:
`padbangers-mpcx-remap/src/BUILD.md`. (Auf dem Windows-PC lag Zig im Scratchpad, nicht im Repo —
am Mac neu holen: <https://ziglang.org/download/>.)
```
```
