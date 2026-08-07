#!/usr/bin/env bash
# mpcx-record-audio.sh (macOS) - Screencapture MIT Ton -> ~/Downloads.
#
#   Bild : MPC streamt PNG-Frames ueber SSH (drmrec)              [Netzwerk]
#   Ton  : MPC-Ausgang -> Focusrite Scarlett 18i20 In 3/4         [Audio-Kabel]
#   -> ffmpeg muxt beides in ein MP4 (H.264 + AAC).
#
# Die MPC selbst gibt KEIN Audio ueber USB aus (USB-B = nur Controller-Modus).
# Der Ton kommt deshalb ueber das Audio-Interface am Mac herein.
#
# EINMALIG:
#   1) Audio-Geraete-Index pruefen:
#        ffmpeg -f avfoundation -list_devices true -i "" 2>&1 | grep -i audio
#      "Scarlett 18i20 USB" ist idR [0].
#   2) MPC-Ausgaenge an Scarlett-Eingang 3/4 verkabeln.
#      Falls der Ton stumm bleibt: pruefen, auf WELCHEM Kanalpaar das Signal
#      wirklich liegt (haengt von Verkabelung/Routing ab) und ggf. per
#      ch_l/ch_r-Argument ueberschreiben. RMS je Kanal messen mit:
#        ffmpeg -f avfoundation -i ":0" -t 6 -c:a pcm_s16le -y /tmp/all.wav
#        for ch in $(seq 0 17); do ffmpeg -i /tmp/all.wav \
#          -af "pan=mono|c0=c${ch},astats=metadata=1:reset=0" -f null - 2>&1 \
#          | grep "RMS level dB" | tail -1; done
#
# Aufruf:  ./mpcx-record-audio.sh [sekunden=20] [fps=6] [offset_sek=0] [audio_index=0] [ch_l=3] [ch_r=4]
#   offset_sek: >0 verzoegert das VIDEO (falls Ton vorlaeuft), <0 verzoegert den TON.
#
# HINWEIS fps: drmrec schafft ueber die Netzwerkstrecke real ~6 fps. Bei hoeheren
# Werten laeuft die Aufnahme deutlich LAENGER als angefordert (12 fps -> ~37s statt 30s),
# weil das Geraet die Frames nicht rechtzeitig liefert. 6 fps haelt die Zeit exakt ein.
set -euo pipefail

SEC="${1:-20}"; FPS="${2:-6}"; OFFS="${3:-0}"
ADEV="${4:-0}"; CHL="${5:-3}"; CHR="${6:-4}"

ts="$(date +%Y%m%d_%H%M%S)"
out="${HOME}/Downloads/MPCX_${ts}.mp4"

command -v ffmpeg >/dev/null || { echo "ffmpeg fehlt: brew install ffmpeg"; exit 1; }

# avfoundation liefert alle 18 Kanaele; hier auf das gewuenschte Paar heruntermischen.
# pan ist 0-basiert -> Kanal 3/4 = c2/c3.
#
# aresample=async=1:first_pts=0 ist PFLICHT: avfoundation liefert Zeitstempel ab
# Systemlaufzeit (start_time ~2300s) und in Schueben. Ohne das fehlt real ~11% der
# Samples (10s angefordert -> nur 8.9s Ton), waehrend der Container 10s deklariert
# -> der Player spielt den Ton zu schnell ab. async=1 fuellt die Luecken auf.
pan="pan=stereo|c0=c$((CHL-1))|c1=c$((CHR-1)),aresample=48000:async=1:first_pts=0"

# Optionaler A/V-Offset: positiver Wert schiebt das Video nach hinten.
voff=""; aoff=""
if awk "BEGIN{exit !($OFFS>0)}"; then voff="-itsoffset ${OFFS}"; fi
if awk "BEGIN{exit !($OFFS<0)}"; then aoff="-itsoffset ${OFFS#-}"; fi

echo "-> Screencapture MIT Ton: ${SEC}s @ ${FPS}fps (Audio-Dev ${ADEV}, Kanal ${CHL}/${CHR})..."
# Video von SSH-stdin (Wallclock-Timestamps = reale drmrec-Pacing, sync zum Live-Audio).
# -shortest beendet, sobald drmrec (Video) nach SEC Sekunden endet.
ssh mpcx "/data/hacks/drmrec ${SEC} ${FPS} 1" \
  | ffmpeg -hide_banner -loglevel error \
      ${aoff} -f avfoundation -i ":${ADEV}" \
      ${voff} -f image2pipe -use_wallclock_as_timestamps 1 -i - \
      -af "${pan}" \
      -vf "transpose=2,fps=${FPS}" \
      -fps_mode cfr \
      -c:v libx264 -pix_fmt yuv420p -preset veryfast \
      -c:a aac -b:a 192k \
      -t "${SEC}" \
      -shortest -movflags +faststart -y "${out}"

echo "Fertig: ${out}"
open "${out}"
