# Bootlogo der Akai MPC X tauschen

Anleitung zum Ersetzen des Boot-Splashscreens. Kommt ohne fremde Skripte aus —
alles mit `ssh`, `dd` und `ffmpeg`.

Verifiziert am 2026-08-07 auf einer MPC X mit Hakai-Firmware (MPC-App 3.6.x,
RK3288, Kernel 6.18-rt). Andere Firmware-Stände können abweichen — deshalb sind
unten die Prüfschritte wichtig, die du **nicht** überspringen solltest.

---

## Was du brauchst

- **SSH-Zugang als root** auf die MPC (LAN, Port 22).
- **ffmpeg** auf deinem Rechner (nicht auf der MPC — dort ist keines installiert).
  macOS: `brew install ffmpeg` · Linux: über die Paketverwaltung.
- Ein **Bild** als Vorlage.

Im Folgenden heißt der SSH-Zugang `root@MPC-IP`. Wer sich einen Alias `mpcx` in
`~/.ssh/config` einträgt, kann `mpcx` statt `root@MPC-IP` schreiben.

---

## Risiko und Sicherheitsnetz

Der Splash liegt in einer **Roh-Partition**, die direkt beschrieben wird. Das ist
ein realer Eingriff, aber ein vergleichsweise harmloser:

- Der Splash ist **rein kosmetisch**. Ein fehlerhaftes Bild verhindert den Boot
  **nicht** — im schlimmsten Fall siehst du Rauschen oder einen schwarzen Schirm.
- Mit dem Backup aus Schritt 1 ist alles **jederzeit rückrollbar**.
- Die read-only Firmware wird **nicht** angefasst.

Zwei Dinge trotzdem ernst nehmen:

1. **Ohne Backup nicht anfangen.** Schritt 1 ist nicht optional.
2. **Ein Firmware-Update kann die Partition überschreiben.** Backup aufbewahren.

Der Vorgang selbst braucht **keinen** Neustart der MPC-App. Sichtbar wird das neue
Logo aber erst beim nächsten **Reboot** — und der beendet die MPC-App, wodurch
ungespeicherte Projekte verloren gehen. Vorher speichern.

---

## Schritt 1 — Partition finden und sichern

Die MPC X hat **keine** `/dev/block/by-name/`-Einträge, die Partition muss also
über ihre Größe identifiziert werden:

```
ssh root@MPC-IP 'grep mmcblk0p /proc/partitions'
```

Erwartete Ausgabe (Auszug):

```
 179        4       4096 mmcblk0p4      <- splash
 179        5       4096 mmcblk0p5      <- recoverysplash ("UPDATE MODE")
```

`mmcblk0p4` und `p5` sind die beiden einzigen Partitionen mit exakt **4096 KB**.
`p4` ist der normale Boot-Splash — den wollen wir. `p5` ist der rote
„UPDATE MODE"-Schirm; **den lässt du besser in Ruhe.**

Jetzt beide sichern — einmal auf dem Gerät, einmal auf deinem Rechner:

```
# Backup auf der MPC selbst
ssh root@MPC-IP 'mkdir -p /data/hacks/backup && \
  dd if=/dev/mmcblk0p4 of=/data/hacks/backup/splash_p4_original.img bs=64k'

# Backup auf deinen Rechner holen
ssh root@MPC-IP 'dd if=/dev/mmcblk0p4 bs=64k 2>/dev/null' > splash_p4_original.img
```

**Prüfen, dass die Kopie vollständig ist** (die beiden Prüfsummen müssen gleich sein):

```
ssh root@MPC-IP 'md5sum /dev/mmcblk0p4'
md5 -q splash_p4_original.img          # Linux: md5sum splash_p4_original.img
```

Stimmen sie nicht überein, **hier abbrechen** und die Übertragung wiederholen.

---

## Schritt 2 — Das Format verstehen

Die Partition enthält **kein** BMP, PNG oder sonstiges Dateiformat, sondern rohe
Pixel:

| | |
|---|---|
| Format | **rohes BGRA**, kein Header, ab Offset 0 |
| Auflösung | **800 × 1280** (Portrait — das Panel ist nativ hochkant) |
| Bilddaten | 4.096.000 Bytes (= 800 × 1280 × 4) |
| Danach | 98.304 Bytes (96 KB) **Metadaten** |
| Partition gesamt | 4.194.304 Bytes (4 MiB) |

Die 96 KB am Ende sind **keine Nullen**. Was genau darin steht, ist nicht
untersucht — deshalb werden sie unverändert aus dem Original übernommen,
statt sie zu überschreiben.

**Zur Orientierung:** Das Bild liegt um 90° gedreht im Speicher. Ein normal
ausgerichtetes Querformat-Motiv (1280 × 800) muss mit `transpose=2` gedreht
werden, damit es auf dem Gerät aufrecht erscheint.

Schau dir zur Kontrolle an, was aktuell drin ist:

```
head -c 4096000 splash_p4_original.img | \
  ffmpeg -f rawvideo -pix_fmt bgra -s 800x1280 -i - -frames:v 1 -y original.png
```

Wenn `original.png` das bekannte MPC-Logo zeigt, stimmen Format und Auflösung
für deine Firmware. **Zeigt es Rauschen, brich ab** — dann weicht dein Gerät ab
und der Rest dieser Anleitung passt nicht.

---

## Schritt 3 — Eigenes Bild vorbereiten

Zwei Fälle, je nachdem wie dein Bild vorliegt:

**a) Querformat-Motiv (z. B. 1280 × 800 oder 16:10)** — muss gedreht werden:

```
ffmpeg -i meinbild.png \
  -vf "transpose=2,scale=800:1280:force_original_aspect_ratio=decrease,\
pad=800:1280:(ow-iw)/2:(oh-ih)/2:black,format=bgra" \
  -frames:v 1 -f rawvideo -pix_fmt bgra -y pixels.raw
```

**b) Bild liegt schon hochkant in 800 × 1280 vor** — nicht drehen, sonst steht es
quer:

```
ffmpeg -i meinbild.png \
  -vf "scale=800:1280:force_original_aspect_ratio=decrease,\
pad=800:1280:(ow-iw)/2:(oh-ih)/2:black,format=bgra" \
  -frames:v 1 -f rawvideo -pix_fmt bgra -y pixels.raw
```

`scale`/`pad` sorgen dafür, dass das Seitenverhältnis erhalten bleibt und
fehlende Ränder schwarz aufgefüllt werden.

**Größe prüfen** — muss exakt 4096000 sein:

```
wc -c < pixels.raw
```

---

## Schritt 4 — Image zusammensetzen

Bilddaten plus die unveränderten 96 KB Metadaten aus dem Original:

```
cat pixels.raw > splash_neu.img
dd if=splash_p4_original.img bs=1 skip=4096000 >> splash_neu.img
```

**Größe prüfen** — muss exakt 4194304 sein:

```
wc -c < splash_neu.img
```

Weicht der Wert ab, **nicht schreiben**. Dann stimmt etwas an Schritt 3 nicht.

---

## Schritt 5 — Vorschau ansehen (nicht überspringen)

So wird es auf dem Gerät aussehen:

```
head -c 4096000 splash_neu.img | \
  ffmpeg -f rawvideo -pix_fmt bgra -s 800x1280 -i - -frames:v 1 -y vorschau.png
```

Öffne `vorschau.png` und prüfe:

- **Lage** — steht das Motiv richtig herum? Wenn es um 90° verdreht ist, hast du
  in Schritt 3 den falschen Fall erwischt (a statt b oder umgekehrt).
- **Farben** — sieht Rot rot aus? Falls Rot und Blau vertauscht wirken, fehlt
  `format=bgra` im Filter.

Erst wenn die Vorschau stimmt, weiter.

---

## Schritt 6 — Schreiben

```
scp splash_neu.img root@MPC-IP:/tmp/splash.img
```

Prüfen, dass die Übertragung intakt ist (Prüfsummen müssen gleich sein):

```
md5 -q splash_neu.img                            # Linux: md5sum
ssh root@MPC-IP 'md5sum /tmp/splash.img'
```

Dann schreiben:

```
ssh root@MPC-IP 'dd if=/tmp/splash.img of=/dev/mmcblk0p4 bs=64k && sync'
```

> **Hinweis:** Die MPC nutzt BusyBox. Deren `dd` kennt **kein** `conv=fsync` —
> der Aufruf schlägt sonst fehl (die Partition bleibt dabei unverändert).
> Deshalb `&& sync` als separater Befehl.

**Zurücklesen und verifizieren** — die Prüfsumme muss der von `splash_neu.img`
entsprechen:

```
ssh root@MPC-IP 'dd if=/dev/mmcblk0p4 bs=64k 2>/dev/null | md5sum'
```

Aufräumen:

```
ssh root@MPC-IP 'rm -f /tmp/splash.img'
```

---

## Schritt 7 — Neu starten

Das neue Logo erscheint beim nächsten Boot. **Vorher in der MPC-App speichern**,
der Neustart beendet sie.

---

## Zurückrollen

Wenn das Backup auf dem Gerät liegt:

```
ssh root@MPC-IP 'dd if=/data/hacks/backup/splash_p4_original.img \
  of=/dev/mmcblk0p4 bs=64k && sync'
```

Oder vom Rechner aus:

```
scp splash_p4_original.img root@MPC-IP:/tmp/orig.img
ssh root@MPC-IP 'dd if=/tmp/orig.img of=/dev/mmcblk0p4 bs=64k && sync'
```

---

## Fehlersuche

| Symptom | Ursache | Lösung |
|---|---|---|
| `dd: invalid option` | BusyBox-`dd` kennt `conv=fsync` nicht | `&& sync` benutzen (Schritt 6) |
| Logo steht um 90° verdreht | falscher Fall in Schritt 3 | anderen Fall (a/b) nehmen |
| Logo steht auf dem Kopf | zweimal gedreht | `transpose=2` entfernen bzw. ergänzen |
| Rot und Blau vertauscht | `format=bgra` fehlt | Filter aus Schritt 3 vollständig übernehmen |
| Bild verzerrt / falsch skaliert | `scale`/`pad` weggelassen | Filter vollständig übernehmen |
| Rauschen beim Boot | Größe stimmt nicht | Schritt 4 prüfen (exakt 4194304 Bytes) |
| Nach Firmware-Update wieder Original | Update hat die Partition überschrieben | Schritt 6 erneut ausführen |

Der Boot selbst bleibt in allen diesen Fällen intakt — der Splash ist kosmetisch.

---

## Was diese Anleitung nicht abdeckt

- **Animiertes Bootlogo / Video.** Grundsätzlich möglich (`/dev/fb0` ist da,
  800×1280 bei 32 bpp), aber auf der MPC ist **kein ffmpeg und kein anderer
  Player** installiert. Man müsste Frames vorab in Rohformat wandeln und ein
  eigenes kleines Programm auf dem Gerät die Frames in den Framebuffer schreiben
  lassen — plus einen Boot-Hook, der früh genug greift. Ungetestet.
- **`recoverysplash` (`mmcblk0p5`).** Gleiches Format, aber nicht ausprobiert.
- **Andere MPC-Modelle.** Nur auf der MPC X verifiziert. Force und andere
  RK3288-Geräte sind verwandt, können aber abweichende Partitionslayouts haben.
