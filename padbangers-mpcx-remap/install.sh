na#!/bin/sh
# =====================================================================
#  Padbangers - Akai MPC X Button Remap  ·  INSTALLER
#  Requires: Akai MPC X with root SSH access (Hakai / az0x firmware).
#  Run ON the MPC:   sh install.sh
# =====================================================================

HERE="$(cd "$(dirname "$0")" && pwd)"
HACKS=/data/hacks
SET=/media/az01-internal/Settings/MPC/MPC.settings
XMMDIR="/media/az01-internal-sd/MPC Documents/Midi Learn"
XMM_NAME="Track Remap.xmm"
SHIM="$HACKS/shim_remap6.so"

echo "=== Padbangers MPC X Remap - Installer ==="

# ---- sanity: is this an MPC? ----
if [ ! -e /usr/bin/az01-launch-MPC ]; then
    echo "!! This does not look like an Akai MPC (az0x). Aborting."
    exit 1
fi

mkdir -p "$HACKS/backup" "$XMMDIR"

# ---- 1) copy payload ----
echo " -> copying shim + config + mapping"
cp "$HERE/files/shim_remap6.so" "$SHIM" && chmod +x "$SHIM"
cp "$HERE/files/remap.conf"     "$HACKS/remap.conf"
cp "$HERE/files/$XMM_NAME"      "$XMMDIR/$XMM_NAME"

# ---- 2) safety net: make SSH come up on every boot (independent of the app) ----
systemctl enable sshd.service >/dev/null 2>&1 || true

# ---- 3) stop the MPC app so we can edit its settings cleanly ----
echo " -> stopping MPC app"
systemctl stop acvs.service >/dev/null 2>&1 || true
sleep 2

STAMP=$(date +%Y%m%d-%H%M%S)
[ -f "$SET" ] && cp -a "$SET" "$HACKS/backup/MPC.settings.$STAMP" && echo "    (settings backed up to $HACKS/backup/MPC.settings.$STAMP)"

if [ -f "$SET" ]; then
    echo " -> enabling Global MIDI mapping + seeding 'Padbangers' control flag"

    # point MidiLearnPreviousMapping at our .xmm
    if grep -q 'MidiLearnPreviousMapping' "$SET"; then
        sed -i 's#<VALUE name="MidiLearnPreviousMapping" val="[^"]*"/>#<VALUE name="MidiLearnPreviousMapping" val="/sdcard/MPC Documents/Midi Learn/Track Remap.xmm"/>#' "$SET"
    else
        awk '/<\/PROPERTIES>/{print "  <VALUE name=\"MidiLearnPreviousMapping\" val=\"/sdcard/MPC Documents/Midi Learn/Track Remap.xmm\"/>"} {print}' "$SET" > "$SET.tmp" && mv "$SET.tmp" "$SET"
    fi

    # MidiLearnEnabled = 1
    if grep -q 'MidiLearnEnabled' "$SET"; then
        sed -i 's#<VALUE name="MidiLearnEnabled" val="[^"]*"/>#<VALUE name="MidiLearnEnabled" val="1"/>#' "$SET"
    else
        awk '/<\/PROPERTIES>/{print "  <VALUE name=\"MidiLearnEnabled\" val=\"1\"/>"} {print}' "$SET" > "$SET.tmp" && mv "$SET.tmp" "$SET"
    fi

    # add the virtual "Padbangers" input to the device table with control:true (only if missing)
    if ! grep -q 'Padbangers' "$SET"; then
        ANC='value0&quot;: [&#10;'
        ENT='        {&#10;            &quot;type&quot;: 0,&#10;            &quot;deviceName&quot;: &quot;Padbangers&quot;,&#10;            &quot;deviceId&quot;: &quot;129-0&quot;,&#10;            &quot;displayName&quot;: &quot;Padbangers&quot;,&#10;            &quot;track&quot;: false,&#10;            &quot;connected&quot;: true,&#10;            &quot;global&quot;: false,&#10;            &quot;sync&quot;: false,&#10;            &quot;control&quot;: true,&#10;            &quot;ignoreAuto&quot;: false,&#10;            &quot;mpe&quot;: false&#10;        },&#10;'
        awk -v anc="$ANC" -v ins="$ENT" '{ p=index($0,anc); if(p>0){ a=p+length(anc); $0=substr($0,1,a-1) ins substr($0,a) } print }' "$SET" > "$SET.tmp" && mv "$SET.tmp" "$SET"
    fi
fi

# ---- 4) autoload the shim on every boot via the dynamic linker ----
echo " -> enabling autoload (/etc/ld.so.preload)"
if [ -f /etc/ld.so.preload ]; then
    grep -q "$SHIM" /etc/ld.so.preload || echo "$SHIM" >> /etc/ld.so.preload
else
    echo "$SHIM" > /etc/ld.so.preload
fi

# ---- 5) restart the app ----
echo " -> starting MPC app"
systemctl start acvs.service >/dev/null 2>&1 || true

echo ""
echo "=== Done! The remapping is active and survives reboots. ==="
echo ""
echo "  + / -       : previous / next track"
echo "  Next Seq    : 1x Song         · 2x Next Sequence"
echo "  Pad Perform : 1x Grid View    · 2x Pad Perform"
echo "  XY FX       : 1x Arrange      · 2x Sounds"
echo "  F-Key       : Add Track"
echo ""
echo "Change mappings: edit $HACKS/remap.conf, then  systemctl restart acvs.service"
echo "Uninstall:       sh uninstall.sh"
