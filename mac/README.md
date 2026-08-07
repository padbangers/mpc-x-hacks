# Mac-Scripte (macOS)

Bash-Portierungen der Windows-`.ps1`-Scripte + die neue Aufnahme mit Ton.

## Voraussetzungen
1. **SSH-Zugang** zur MPC einrichten (auf dem Windows-PC hieß der Alias `mpcx`):
   ```
   # ~/.ssh/config
   Host mpcx
       HostName 192.168.178.58
       User root
   ```
   SSH-Key auf die MPC bringen (`ssh-copy-id root@192.168.178.58`) oder vorhandenen
   Key nutzen. Test: `ssh mpcx uptime`.
2. **ffmpeg**: `brew install ffmpeg`
3. Auf der MPC liegen die Tools bereits unter `/data/hacks/` (`drmshot`, `drmrec`).
   Falls nicht: aus `screenshot/` bzw. `video/` neu bauen (Zig-Cross-Build, siehe
   `padbangers-mpcx-remap/src/BUILD.md`) und nach `/data/hacks/` kopieren.

## Scripte
| Script | Zweck |
|---|---|
| `mpcx-screenshot.sh` | PNG-Screenshot → `~/Downloads`, dann von MPC löschen + öffnen |
| `mpcx-record.sh` | Video (ohne Ton) → `~/Downloads` |
| `mpcx-record-audio.sh` | **Screencapture MIT Ton** (USB-Audio) → `~/Downloads` |

```
chmod +x mac/*.sh
./mac/mpcx-screenshot.sh
./mac/mpcx-record.sh 20 12
./mac/mpcx-record-audio.sh            # ohne Args: listet Audio-Geraete
./mac/mpcx-record-audio.sh 1 20 12    # audio_index=1, 20s, 12fps
```

## Screencapture mit Ton – Kurzanleitung
1. MPC per **USB-B ("to computer")** an den Mac (Standalone bleibt an, *kein*
   Controller-Modus). macOS erkennt **„MPC X Audio"** als Eingang.
2. Audio-Index finden:
   ```
   ffmpeg -f avfoundation -list_devices true -i "" 2>&1 | grep -i audio
   ```
3. `./mac/mpcx-record-audio.sh <index> <sekunden> <fps>`
4. **Sync** prüfen. Falls Ton vor-/nachläuft, mit dem 4. Argument justieren
   (`offset_sek`, positiv = Video später, negativ = Ton später), z.B. `... 1 20 12 0.3`.

Details/Hintergrund zur Audio-Entscheidung: [`../audio/README.md`](../audio/README.md).
