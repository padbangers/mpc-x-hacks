#!/bin/sh
# Run on the MPC (copy to /data/hacks/ together with f1bridge.so). Restarts the MPC app, so save first.
#   sh f1-try.sh        start the app with f1bridge.so preloaded (log: /data/hacks/f1bridge.log)
#   sh f1-try.sh stop   back to the stock launcher
# The launcher /usr/bin/az01-launch-MPC is on the read-only rootfs, so a copy with f1bridge.so prepended
# to LD_PRELOAD is bind-mounted over it. The mount is gone after a reboot. If the app doesn't come up
# with the shim, this reverts on its own.
HACKS=/data/hacks
LAUNCHER=/usr/bin/az01-launch-MPC

umount "$LAUNCHER" 2>/dev/null
if [ "$1" = stop ]; then
    systemctl restart acvs
    echo "stock launcher restored"
    exit 0
fi

sed "s#export LD_PRELOAD=\"#export LD_PRELOAD=\"$HACKS/f1bridge.so #" "$LAUNCHER" > "$HACKS/az01-launch-MPC.f1"
chmod +x "$HACKS/az01-launch-MPC.f1"
mount --bind "$HACKS/az01-launch-MPC.f1" "$LAUNCHER"
systemctl restart acvs
sleep 20
if ! pidof MPC >/dev/null || ! grep -q f1bridge /proc/$(pidof MPC)/maps; then
    echo "f1bridge start FAILED, reverting"
    umount "$LAUNCHER"
    systemctl reset-failed acvs; systemctl restart acvs
else
    echo "f1bridge running pid=$(pidof MPC)"
fi
