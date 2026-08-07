#!/bin/sh
# =====================================================================
#  Padbangers - Akai MPC X Button Remap  ·  UNINSTALLER
#  Run ON the MPC:   sh uninstall.sh
# =====================================================================
SET=/media/az01-internal/Settings/MPC/MPC.settings
SHIM=/data/hacks/shim_remap6.so

echo "=== Padbangers MPC X Remap - Uninstall ==="

systemctl stop acvs.service >/dev/null 2>&1 || true
sleep 2

# remove autoload entry (delete the file if it becomes empty)
if [ -f /etc/ld.so.preload ]; then
    grep -v 'shim_remap6.so' /etc/ld.so.preload > /etc/ld.so.preload.tmp 2>/dev/null
    mv /etc/ld.so.preload.tmp /etc/ld.so.preload 2>/dev/null || true
    [ -s /etc/ld.so.preload ] || rm -f /etc/ld.so.preload
fi

# turn Global MIDI mapping off again (leaves the .xmm + Padbangers entry; both harmless)
[ -f "$SET" ] && sed -i 's#<VALUE name="MidiLearnEnabled" val="1"/>#<VALUE name="MidiLearnEnabled" val="0"/>#' "$SET"

# remove shim + config
rm -f "$SHIM" /data/hacks/remap.conf

systemctl start acvs.service >/dev/null 2>&1 || true

echo "Done. Remapping removed. (SSH stays enabled; backups kept in /data/hacks/backup.)"
