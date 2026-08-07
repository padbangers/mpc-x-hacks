# macOS scripts

Bash ports of the Windows `.ps1` scripts.

## Requirements

1. **SSH access** to the MPC (the alias used throughout is `mpcx`):
   ```
   # ~/.ssh/config
   Host mpcx
       HostName <MPC-IP>
       User root
   ```
   Copy an SSH key to the MPC (`ssh-copy-id root@<MPC-IP>`) or use an existing one.
   Test with `ssh mpcx uptime`.
2. **ffmpeg**: `brew install ffmpeg` — the MPC itself has none.
3. The device-side tools are expected in `/data/hacks/` (`drmshot`, `drmrec`).
   If they are missing, rebuild them from `screenshot/` or `video/` (Zig cross
   build, see `padbangers-mpcx-remap/src/BUILD.md`) and copy them there.

## Scripts

| Script | Purpose |
|---|---|
| `mpcx-screenshot.sh` | PNG screenshot → `~/Downloads`, then delete from the MPC and open |
| `mpcx-record.sh` | Video (no audio) → `~/Downloads` |

```
chmod +x mac/*.sh
./mac/mpcx-screenshot.sh
./mac/mpcx-record.sh 20 6      # 20 seconds at 6 fps
```

## Frame rate

`drmrec` sends uncompressed PNG frames over the network and sustains roughly
**6 fps**. Requesting more does not make the capture faster — it makes the run take
longer than the requested duration (12 fps turns a 30 s capture into ~37 s), because
the device cannot deliver frames in time.
