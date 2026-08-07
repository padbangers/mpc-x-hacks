# Audio-Aufnahme (Screencapture mit Ton)

Ziel: die MPC-X-**Screencapture mit dem echten internen Audio** (die Musik) aufnehmen.

## Was auf dem Gerät NICHT geht (untersucht 2026-08-07)

- **Kein `arecord`, kein `snd-aloop` (Loopback-Modul).**
- ALSA-Karten: `0 microKEY2`, **`1 MPC X Audio`**, `2 MPC X Controller` (MIDI, kein PCM).
- Die MPC-App (pid) hält auf **card1** *beide* Streams **exklusiv** offen:
  - **`pcm0p` Playback = 8 Kanäle** S32/44,1 kHz → der Audio-**Ausgang** (was man hört).
  - **`pcm0c` Capture = 4 Kanäle** S32/44,1 kHz → die physischen **Eingänge** (Line/Mic).
- Direktes Öffnen des Capture-Devices scheitert mit **`Device or resource busy`** (siehe `arec.c`).
- Der **Ausgang lässt sich per ALSA nicht „aufnehmen"** (Playback ist output-only), und der
  Capture-Eingang ist a) belegt und b) ohnehin nur der Line/Mic-In, nicht die Musik.

**→ Der Akai-Force-Trick von MockbaMod (`ffmpeg -f alsa -i hw:2`) funktioniert auf der MPC X
nicht.** Bei der Force ist das Capture-Device frei/parallel öffenbar und enthält den Ausgang;
hier nicht.

`arec.c` = das kleine Testtool, mit dem das verifiziert wurde (ALSA-Capture → WAV, linkt die
Geräte-`libasound.so.2`, cross-gebaut mit Zig).

## Gewählte Lösung: natives USB-Audio am Host

Die MPC X **ist** ein USB-Audio-Interface. Per USB-B ("to computer") am Rechner erscheint
**„MPC X Audio"** als Aufnahme-Eingang = die echte interne Musik, **ohne jeden Eingriff in die App**.

- **Bild:** `drmrec` streamt PNG-Frames über SSH → Rechner (unverändert).
- **Ton:** MPC-USB-Audio direkt am Rechner.
- **Mux:** ffmpeg führt beides in *einem* Befehl zu einem MP4 zusammen.
- Sync über Wallclock-Timestamps fürs Video (+ optionaler Offset).

Mac-Umsetzung: siehe [`../mac/`](../mac/) — `mpcx-record-audio.sh` (avfoundation-Audio + drmrec-Bild).

## Fallback (nur falls kein USB-Audio gewünscht): In-App-LD_PRELOAD-Tap

Eine separate Preload-Lib (`atap.so`) würde in der App `snd_pcm_writei` hooken, ch0/1 (Main L/R)
aus dem 8-Kanal-Ausgang in einen **lock-freien Ringpuffer** kopieren (nur wenn ein Reader
verbunden ist) und über einen **Drainer-Thread** in eine FIFO `/tmp/mpc_audio` (S16LE stereo
44,1 kHz) schreiben; Hot-Path blockiert nie, im Normalbetrieb inaktiv. Wurde **verworfen**
zugunsten des risikofreien USB-Audio-Wegs, ist aber der einzige *netzwerk-only*-Weg. Design-Details
im Handoff.
