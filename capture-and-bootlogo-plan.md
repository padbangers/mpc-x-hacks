# Screen-Capture & Custom-Bootlogo auf der MPC X — Machbarkeit & Plan

> Stand: 2026-08-07 · **Nur Planung, noch keine Umsetzung.** Basierend auf MockbaMod
> (Akai Force/MPC-Mod) + read-only-Analyse der MPC X.

## Kurzfazit

**Beides ist machbar.** Deine MPC X hat **dieselbe Hardware-Basis wie die Akai Force**
(Rockchip **RK3288**, DRM/KMS, `/dev/fb0`) — MockbaMods Techniken lassen sich direkt
übertragen. **Einziges fehlendes Teil: ein `ffmpeg`-Binary** mit `kmsgrab`/`fbdev`/`libx264`
(auf dem Gerät nicht installiert). Fürs Bootlogo gibt es eine dedizierte **`splash`-Partition**.

## Verifizierte Hardware-Fakten (read-only)

| | |
|---|---|
| SoC / Modell | Rockchip **RK3288**, Produkt **ACV5** (MPC X) |
| DRM/KMS | `/dev/dri/card0` (+ card1, renderD128), DSI-Panel `card0-DSI-1` |
| Framebuffer | `/dev/fb0` (`rockchipdrmfb`) |
| Panel-Auflösung | 800×1280 (Portrait nativ, Landscape-UI) |
| Bootlogo | Partitionen `splash` = `mmcblk0p4`, `recoverysplash` = `mmcblk0p5` |
| Audio-Aufnahme | ALSA `hw:2` |
| ffmpeg/gstreamer | **nicht vorhanden** |
| Speicher frei | `/sdcard` 3,7 GB · `/media/MPC SSD` 9,6 GB |

---

## Feature 1 — Screen-Capture (Screenshots + Video)

**So macht es MockbaMod** (funktioniert, weil identischer SoC):
- Screenshot: `ffmpeg -f kmsgrab -i - -vf 'hwdownload,format=bgr0' -vframes 1 -c:v png out.png`
- Video: `ffmpeg -framerate 30 -f kmsgrab -i - -vf hwdownload,format=bgr0 -c:v libx264rgb -crf 0 -preset ultrafast out.mkv`
- Video **+ Audio**: zusätzlich `-f alsa -i hw:2`
- MockbaMod liefert ein **eigenes ffmpeg-Binary** mit.

**Auf der MPC X:**
- `kmsgrab` greift den Live-DRM-Framebuffer ab → funktioniert auch, während die MPC-App rendert. DRM ist vorhanden ✓.
- **Benötigt:** ffmpeg mit `kmsgrab` + `libx264` (+ `alsa`). Zwei Wege:
  1. **MockbaMods Force-ffmpeg wiederverwenden** (gleiches RK3288/armhf) — sehr wahrscheinlich direkt lauffähig; nur gegen unsere glibc 2.39 testen.
  2. **Eigenes statisches ffmpeg cross-compilen** (mehr Aufwand; via Zig/Toolchain, mit kmsgrab/x264/fbdev).
- **Performance:** verlustfreies H.264 (crf0) bei 1280×800@30 ist CPU-schwer → evtl. Frame-Drops/hohe Last. Optionen: niedrigere fps, verlustbehaftet, oder **Hardware-Encoder (rkmpp/VPU)**, falls das ffmpeg-Build ihn kann. Screenshots sind billig.
- Ausgabe nach `/sdcard` oder SSD.

**Risiko: NIEDRIG** (reines Auslesen, keine Systemänderung). **Aufwand:** hauptsächlich „ein lauffähiges ffmpeg besorgen".

---

## Feature 2 — Custom Bootlogo

Zwei unabhängige Ansätze:

### A) Statisches U-Boot-Splash (frühestes Logo, vor Linux)
- Liegt in der **`splash`-Partition** (`mmcblk0p4`). Ersetzen = korrekt formatiertes Bild reinschreiben (RK-U-Boot erwartet ein bestimmtes Format — vermutlich BMP oder Roh-Framebuffer in Panel-Größe).
- Schritte: aktuelles Splash **dumpen** (`dd`, nur lesen) → Format/Größe bestimmen → passendes Bild bauen → mit `dd` schreiben. `recoverysplash` (p5) als Referenz/Fallback.
- **Risiko: MITTEL** (Roh-Partition schreiben). Falsches Format = leeres/verzerrtes Logo, **bricht aber den OS-Boot nicht** (rein kosmetisch). Vorher Original der Partition sichern.

### B) Video-/Animiertes Logo (wie MockbaMod)
- mp4/Bild während des Boots auf `/dev/fb0` abspielen, bevor/während die MPC-App den Bildschirm übernimmt:
  `ffmpeg -i logo.mp4 -pix_fmt bgra -f fbdev /dev/fb0`
- **Benötigt:** ffmpeg + einen **Boot-Hook**, der es in der Lücke vor dem App-Start abspielt.
- **Risiko: NIEDRIG–MITTEL** (kosmetisch; schlägt es fehl, bootet das System normal weiter). Keine Partitions-Schreibvorgänge.

⚠️ **Boot-Hook-Fallstrick (aus dieser Session bekannt):** systemd-Units/Drop-ins in `/etc` werden beim frühen Boot **nicht** gesehen (das `/etc`-Overlay von `/data` mountet erst spät). Für den Bootlogo-Start also entweder eine Unit an passender Stelle, den Launch-Wrapper, oder einen `ld.so.preload`-ähnlichen Weg nutzen — nicht blind auf einen `/etc`-Drop-in verlassen.

---

## Empfohlene Reihenfolge

1. **ffmpeg beschaffen** (zuerst MockbaMods Force-Binary testen — schnellster Weg).
2. **Screen-Capture** aufbauen: Screenshot → Video → Video+Audio. *(Niedriges Risiko, sofortiger Nutzen.)*
3. **Bootlogo B** (Video auf `/dev/fb0`) mit Boot-Hook. *(Sicher, keine Partition.)*
4. **Bootlogo A** (statisches Splash) nur, wenn du das *früheste* Logo willst — mit Partitions-Backup.

## Offene Punkte (read-only zu klären, vor dem Bauen)

- Exaktes Format der `splash`-Partition (dumpen + Header ansehen).
- MockbaMods ffmpeg-Binary auf unserer glibc 2.39 testen (läuft es? geht kmsgrab?).
- Hardware-Encoder (rkmpp) im ffmpeg-Build für effizientes Video?
- Zeitpunkt im Boot, wann das Logo erscheint / wann die App den Bildschirm greift (Timing fürs Video-Logo).
- Persistenz-Weg für den Boot-Hook (Overlay-Fallstrick beachten, siehe oben).

## Referenzen
- MockbaMod: `SD/MockbaMod/Tools/{sc,vc,vca}.sh` (Capture), `SD/MockbaMod/env.sh` (`play`/`play_back` → ffmpeg auf /dev/fb0), `SD/MockbaMod/Videos/logo.mp4`.
- Gleiche SoC-Familie (RK3288) wie unsere MPC X → hohe Übertragbarkeit.
