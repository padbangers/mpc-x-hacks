# MPC X — Button-Remap Plan: +/- → Track vor/zurück

> Stand: 2026-08-06 · Ziel: Die **+ / − Tasten** unter dem Data-Encoder auf **nächster / vorheriger Track** umbelegen.
> Status: **Phase 0 + 1 erledigt.** Sicherheitsnetz steht (sshd enabled, Backups). Die Aktion „Track vor/zurück" ist
> firmware-seitig **bewiesen** (Global-Mapping Target 62 + Control-Eingang). Offen: Phase 2 (Buttons → Control-Eingang).

## Fortschritt / bewiesene Fakten
- ✅ **Phase 0:** `sshd.service` enabled (SSH bei jedem Boot); Backups in `/data/hacks/backup/`.
- ✅ **Phase 1:** `MidiLearnEnabled=1` + `Track Remap.xmm` (Target 62) + **"Control"-Flag am Eingang** → Track wechselt.
  Getestet mit microKEY2-Joystick (CC 74 / Kanal 1). **Wichtig:** Der sendende MIDI-Eingang MUSS in den
  MPC-MIDI-Settings „Control" aktiviert haben (`MidiDevices.Table`, sonst wird die MIDI ignoriert).
- ⚠️ **Erkenntnis:** Software-Injektion in die laufende App von außen ist **blockiert** (App-Eingänge nehmen keine
  neuen ALSA-Verbindungen an). Nur Quellen, die die App bereits abonniert hat, funktionieren.
- ⛳ **Phase 2 offen:** Die Surface-Buttons (+ = Note 54, − = Note 55, auf „MPC Public") stehen NICHT in der
  MidiDevices.Table und sind daher nicht direkt mappbar → Shim muss +/- in ein CC auf einem „Control"-Eingang
  übersetzen. Zu klären: an welchen Eingang die Shim injiziert, damit das Global-Mapping es verarbeitet.

---

## 1. Zusammenfassung

Die MPC X (Hakai-Firmware, inMusic AZ0x, MPC 3.6.x) liest ihr Bedienfeld intern per
rawmidi **direkt und exklusiv** in der MPC-App — vorbei an der ALSA-Sequencer-Ebene.
Ein **externer Daemon kann die Buttons daher weder sehen noch abfangen** (bewiesen durch
Messreihen). Der einzige funktionierende Interception-Punkt ist eine **LD_PRELOAD-Library
in der MPC-App selbst**. Die „Track vor/zurück"-Funktion existiert firmware-seitig als
mappbares Ziel (**Global-MIDI-Mapping, Target 62 „Change track", relativ**).

**Bestätigte Codes dieser MPC X** (selbst gemessen, App gestoppt, Port 24:1/24:0 gelesen):

| Element | Code | Hinweis |
|---|---|---|
| **+ (Plus)** | **Note 54** | Note On vel 127 = gedrückt, vel 0 = losgelassen |
| **− (Minus)** | **Note 55** | dito |
| Data-Encoder | CC 100 (relativ) | +1 = val 1, −1 = val 127 (2's-complement) |
| MAIN (Referenz) | Note 52 | ✓ Gegenprobe korrekt |
| PLAY (Referenz) | Note 82 | ✓ Gegenprobe korrekt |

> Wichtig: 54/55 sind die **Force-Codes** (nicht MPC-One 125/12). Die dokumentierten
> MPC-One-Werte gelten für die X **nicht**.

---

## 2. Architektur-Erkenntnisse (Grundlage)

- **OS:** inMusic AZ0x (OpenEmbedded/Yocto `az01` 5.0.17), Kernel 6.18 PREEMPT_RT, armv7l, glibc 2.39.
- **Rootfs read-only** (`/usr` unveränderbar). Beschreibbar: `/etc`, `/var` (Overlay), `/data` (~5.9 GB), `/sdcard`, externe SSD.
- **MPC-App** = `acvs.service` → `ExecStart=/usr/bin/az01-launch-MPC` → setzt `LD_PRELOAD` (aktuell `customBufferSizeMPC.so`, optional `hakai_driver.so`/`mpc_cursor.so`) → `exec /usr/bin/MPC`.
- **Surface-Pfad:** App (pid) hält `/dev/snd/midiC2D0` („MPC Public", card 2) direkt (Lesen + LED-Schreiben). Solange die App läuft, ist die Seq-Bridge für 24:0/24:1 datenlos → externes Mitlesen unmöglich. **Nur mit gestoppter App** liest man die Buttons über ALSA-Seq (24:1 „MPC Private" / 24:0 „MPC Public").
- **Kein Compiler / kein Python** auf dem Gerät. Build erfolgt per Cross-Compile (Zig 0.16, `zig cc -target arm-linux-gnueabihf`) — bereits eingerichtet.
- **`/dev/uinput`** vorhanden (Touch-Injektion möglich, aber für diesen Plan nicht nötig).

---

## 3. Lösungsarchitektur

Zwei Schichten:

### Schicht A — Interception (LD_PRELOAD in der App)
Fängt die Surface-Notes **54 (+)** und **55 (−)** ab, **unterdrückt** deren Default
(Wert +/−) und **injiziert** stattdessen ein Signal für Track-Nav.

Zwei Umsetzungsvarianten:

- **Variante 1 — TheKikGen `tkgl_midimapper` + Plugin** (Community-Tool)
  - `tkgl_anyctrl`/`midimapper` ersetzt die Private/Public-rawmidi-Ports durch virtuelle
    Seq-Ports und macht die Surface live umleitbar. Remap-Logik in kompiliertem C-Plugin.
  - Vorteil: erprobtes Framework. **Risiko: Libs stammen aus MPC-2.x-Ära** — Kompatibilität
    mit dem 2026er-Image (Kernel 6.18, MPC 3.6) ungewiss, muss getestet werden.

- **Variante 2 — Eigene schlanke LD_PRELOAD-Shim** (volle Kontrolle)
  - Hookt den rawmidi-Lesepfad der App (`read()` bzw. `snd_rawmidi_read` auf `midiC2D0`),
    erkennt Note 54/55, verschluckt sie und emittiert ein relatives CC.
  - Vorteil: minimal, unabhängig von alten Libs, mit Zig gebaut. Nachteil: wir schreiben/pflegen sie selbst.

### Schicht B — Aktion (Firmware-Global-Mapping, `.xmm`)
Bindet das von Schicht A emittierte CC an **Target 62 „Change track"** mit
**relativem CC** (`Mapping_control=7`, Rel CC 2's-complement) → + = +1 Track, − = −1 Track.

- Datei: `.xmm` in `/media/az01-internal-sd/MPC Documents/Midi Learn/`
  (dort liegen bereits `New MIDI Mapping.xmm` / `… 2.xmm`).
- Aktivierung in `/media/az01-internal/Settings/MPC/MPC.settings`:
  ```
  <VALUE name="MidiLearnEnabled" val="1"/>
  <VALUE name="MidiLearnPreviousMapping" val="<pfad-zur-xmm>"/>
  ```
- Danach Reboot.

> **Empfehlung:** Erst **Variante 2** (eigene Shim) als kleinsten, kontrollierbaren Baustein,
> kombiniert mit Schicht B. Variante 1 nur, falls die eigene Shim unerwartet aufwändig wird.

---

## 4. Umsetzungsschritte

### Phase 0 — Sicherheitsnetz (zuerst!)
- [ ] **`systemctl enable sshd.service`** — SSH startet dann bei jedem Boot **unabhängig**
      von der MPC-App/Hakai. Verhindert Aussperren, falls die App mal nicht startet.
      (Aktuell wird sshd nur von `hakai-init` gestartet.)
- [ ] Aktuelle Original-Dateien sichern: Kopie von `MPC.settings` und dem Launch-Kontext nach `/data/hacks/backup/`.
- [ ] DFU-Recovery-Weg notieren (siehe §6).

### Phase 1 — Aktion verifizieren (Schicht B, ohne LD_PRELOAD)
- [ ] `.xmm` erstellen, die eine Test-Note/CC auf relatives „Change track" (Target 62) mappt.
- [ ] In `MPC.settings` aktivieren, Reboot.
- [ ] Mit einem **externen MIDI-Controller** (oder testweise injiziertem CC) prüfen, dass
      Track vor/zurück tatsächlich schaltet. → Bestätigt, dass Schicht B funktioniert, **bevor**
      wir in die App injizieren.

### Phase 2 — Interception bauen (Schicht A, Variante 2)
- [ ] LD_PRELOAD-Shim in C schreiben: hookt den Surface-Lesepfad, erkennt Note 54/55,
      unterdrückt sie, emittiert das in Phase 1 gewählte CC an den mappbaren MIDI-Eingang.
- [ ] Mit Zig cross-compilen (`arm-linux-gnueabihf`), nach `/data/hacks/` deployen.
- [ ] **Manuell testen** (nicht persistent): `systemctl stop acvs.service`, dann App per Hand
      mit `LD_PRELOAD=/data/hacks/remap.so … /usr/bin/MPC` starten und +/- prüfen.
      Bei Crash: einfach Prozess killen, `systemctl start acvs.service` — nichts Bleibendes.

### Phase 3 — Persistenz (Boot)
- [ ] systemd-Drop-in `/etc/systemd/system/acvs.service.d/override.conf` (Overlay-beschreibbar):
      `ExecStart` auf einen eigenen Wrapper `/data/hacks/launch-mpc-remap.sh` umbiegen, der die
      Logik von `az01-launch-MPC` repliziert **plus** unsere `.so` in `LD_PRELOAD` voranstellt.
      (Das Original-Skript ist read-only und überschreibt `LD_PRELOAD` hart — daher Wrapper statt `Environment=`.)
- [ ] Wrapper startet sshd früh (Redundanz zu Phase 0) und exect dann `/usr/bin/MPC`.
- [ ] Reboot, End-to-End-Test.

---

## 5. Rollback / Wiederherstellung

Jeder Schritt ist einzeln reversibel — **Rootfs und ALSA-Config werden nie angefasst**:

| Rückgängig machen | Aktion |
|---|---|
| Persistenz (Phase 3) | `rm /etc/systemd/system/acvs.service.d/override.conf` → Reboot (Original-Launch aktiv) |
| Global-Mapping (Phase 1) | `MidiLearnEnabled` in `MPC.settings` auf `0` → Reboot |
| Shim/Dateien | `rm -rf /data/hacks/` |
| SSH-Sicherheitsnetz | bleibt aktiviert (schadet nicht) |

**Grundsatz:** Bei jedem Boot bleibt SSH erreichbar (Phase 0) → wir können jede Änderung
per SSH zurücknehmen, selbst wenn die MPC-App nicht startet.

---

## 6. Risiken & Sicherheitsregeln

- ⛔ **Niemals die ALSA-Default-Config ändern** — laut Community-Test führt das zu Bootloop
  (nur per DFU behebbar). Dieser Plan fasst sie nicht an.
- ⚠️ **App-Stopp löscht ungespeicherte Projekte.** Vor jedem `systemctl stop acvs.service`
  sicherstellen, dass nichts Wichtiges offen ist.
- ⚠️ **Firmware-Updates** können den Drop-in/`.xmm` überschreiben oder Codes ändern → nach
  Updates neu prüfen.
- 🛟 **Ultimatives Recovery:** DFU. Das Gerät hat ein DFU-Interface (USB If#3, Cls=fe); der
  Boot-Prozess führt bei erkanntem DFU `update.sh` aus → Firmware neu flashen.
- 🔒 Read-only Rootfs schützt die Firmware; alle unsere Artefakte liegen in `/data` und `/etc`-Overlay.

---

## 7. Offene Punkte (in Phase 1/2 zu klären)

- [ ] Kanal der Surface-Notes: gemessen `ch 0`; Doku spricht von „channel 9". Für die Shim
      irrelevant (wir matchen Note-Nummer), aber beim CC-Emit den vom `.xmm` erwarteten Kanal treffen.
- [ ] Exakter mappbarer Eingang: Auf welchem MIDI-Port „hört" das Global-Mapping? (Public /
      virtueller Port). Bestimmt, wohin die Shim das CC injiziert.
- [ ] Verhalten beim **Halten** von +/- (Auto-Repeat?) — ob die App bei gehaltener Taste
      wiederholt sendet oder nur einmal. Ggf. Repeat in der Shim nachbilden.
- [ ] Ob „Change track" relativ über Sequenz-/Programm-Grenzen sinnvoll wrap-around macht.

---

## 8. Referenzdaten

- **Gerät:** `ssh mpcx` → `root@192.168.178.58`, Hostname `mpc-x`.
- **Service:** `acvs.service` („InMusic MPC Application"), `ExecStart=/usr/bin/az01-launch-MPC`.
- **Surface-Ports (App gestoppt):** `24:1` MPC Private, `24:0` MPC Public (ALSA card 2 „MPC X Controller").
- **Toolchain:** Zig 0.16.0 (Session-Scratchpad), `zig cc -target arm-linux-gnueabihf.2.31`.
- **Mess-Tool:** `/data/hacks/midimon` (eigener ALSA-Seq-Monitor, linkt Geräte-`libasound.so.2`).
- **Quellen:**
  - TheKikGen/MPC-LiveXplore + MPCLiveXplore-libs (LD_PRELOAD-Remapper, Plugins)
  - henning/Hakai-MPC (Button-Code-Liste, Testmethode)
  - Wiki „MPC and Force global midi mapping in standalone mode" (Target 62 „Change track", relativ)
