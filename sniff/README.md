# cvsniff — what the MPC app sends to and reads from its hardware

An `LD_PRELOAD` logger for the MPC app. It logs raw writes to device nodes (`/dev/snd/midi*`, ALSA controls, USB,
tty, hidraw), the app's reads from the control surface (pads, buttons), and can inject bytes. It found the MPC X's
CV/Gate protocol (documented in [padbangers/glass-spheres `cv/README.md`](https://github.com/padbangers/glass-spheres/blob/main/cv/README.md)).

```
zig cc -target arm-linux-gnueabihf.2.31 -shared -fPIC -O2 -o cvsniff.so cvsniff.c -ldl
scp cvsniff.so sniff-try.sh mpcx:/data/hacks/
ssh mpcx sh /data/hacks/sniff-try.sh        # save first: restarts the MPC app with the logger preloaded
ssh mpcx 'echo "bf 00 40 bf 01 00" > /tmp/cvinject'   # inject bytes to the controller
ssh mpcx sh /data/hacks/sniff-try.sh stop   # back to normal
```

The log is `/tmp/cvsniff.log` (tmpfs, i.e. RAM): it grows with every controller write, so don't leave it running.
