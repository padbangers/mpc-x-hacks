# Replacing the Boot Logo on the Akai MPC X

A guide to swapping the boot splash screen. No custom scripts required — everything
uses `ssh`, `dd` and `ffmpeg`.

Verified on 2026-08-07 on an MPC X running Hakai firmware (MPC app 3.6.x, RK3288,
kernel 6.18-rt). Other firmware versions may differ — which is why you should
**not** skip the verification steps below.

---

## What you need

- **SSH access as root** to the MPC (LAN, port 22).
- **ffmpeg on your computer** — not on the MPC, which has none installed.
  macOS: `brew install ffmpeg` · Linux: use your package manager.
- An **image** to use as the source.

Below, the SSH target is written as `root@MPC-IP`. If you add an alias such as
`mpcx` to `~/.ssh/config`, you can type `mpcx` instead of `root@MPC-IP`.

---

## Risk and safety net

The splash lives in a **raw partition** that gets written directly. That is a real
modification, but a comparatively harmless one:

- The splash is **purely cosmetic**. A broken image will **not** prevent booting —
  worst case you see noise or a black screen.
- With the backup from step 1, everything is **reversible at any time**.
- The read-only firmware is **not** touched.

Two things to take seriously anyway:

1. **Do not start without a backup.** Step 1 is not optional.
2. **A firmware update may overwrite the partition.** Keep the backup.

The procedure itself requires **no** restart of the MPC app. However, the new logo
only becomes visible on the next **reboot** — and that terminates the MPC app,
losing any unsaved projects. Save first.

---

## Step 1 — Locate and back up the partition

The MPC X has **no** `/dev/block/by-name/` entries, so the partition has to be
identified by its size:

```
ssh root@MPC-IP 'grep mmcblk0p /proc/partitions'
```

Expected output (excerpt):

```
 179        4       4096 mmcblk0p4      <- splash
 179        5       4096 mmcblk0p5      <- recoverysplash ("UPDATE MODE")
```

`mmcblk0p4` and `p5` are the only two partitions of exactly **4096 KB**. `p4` is
the normal boot splash — that is the one we want. `p5` is the red "UPDATE MODE"
screen; **best left alone.**

Now back both up — once on the device, once on your computer:

```
# Backup on the MPC itself
ssh root@MPC-IP 'mkdir -p /data/hacks/backup && \
  dd if=/dev/mmcblk0p4 of=/data/hacks/backup/splash_p4_original.img bs=64k'

# Pull a backup to your computer
ssh root@MPC-IP 'dd if=/dev/mmcblk0p4 bs=64k 2>/dev/null' > splash_p4_original.img
```

**Verify the copy is complete** (both checksums must match):

```
ssh root@MPC-IP 'md5sum /dev/mmcblk0p4'
md5 -q splash_p4_original.img          # Linux: md5sum splash_p4_original.img
```

If they differ, **stop here** and repeat the transfer.

---

## Step 2 — Understand the format

The partition contains **no** BMP, PNG or any other file format — just raw pixels:

| | |
|---|---|
| Format | **raw BGRA**, no header, starting at offset 0 |
| Resolution | **800 × 1280** (portrait — the panel is natively upright) |
| Image data | 4,096,000 bytes (= 800 × 1280 × 4) |
| Followed by | 98,304 bytes (96 KB) of **metadata** |
| Partition total | 4,194,304 bytes (4 MiB) |

Those trailing 96 KB are **not** zeros. Exactly what they hold has not been
investigated — which is why they are carried over unchanged from the original
rather than overwritten.

**Orientation note:** the image is stored rotated by 90°. A normally oriented
landscape source (1280 × 800) must be rotated with `transpose=2` so it appears
upright on the device.

As a sanity check, look at what is currently in there:

```
head -c 4096000 splash_p4_original.img | \
  ffmpeg -f rawvideo -pix_fmt bgra -s 800x1280 -i - -frames:v 1 -y original.png
```

If `original.png` shows the familiar MPC logo, the format and resolution match your
firmware. **If it shows noise, stop** — your device differs and the rest of this
guide does not apply.

---

## Step 3 — Prepare your image

Two cases, depending on how your image is oriented:

**a) Landscape source (e.g. 1280 × 800 or 16:10)** — needs rotating:

```
ffmpeg -i myimage.png \
  -vf "transpose=2,scale=800:1280:force_original_aspect_ratio=decrease,\
pad=800:1280:(ow-iw)/2:(oh-ih)/2:black,format=bgra" \
  -frames:v 1 -f rawvideo -pix_fmt bgra -y pixels.raw
```

**b) Image is already portrait at 800 × 1280** — do not rotate, or it ends up
sideways:

```
ffmpeg -i myimage.png \
  -vf "scale=800:1280:force_original_aspect_ratio=decrease,\
pad=800:1280:(ow-iw)/2:(oh-ih)/2:black,format=bgra" \
  -frames:v 1 -f rawvideo -pix_fmt bgra -y pixels.raw
```

`scale`/`pad` preserve the aspect ratio and fill any missing edges with black.

**Check the size** — must be exactly 4096000:

```
wc -c < pixels.raw
```

---

## Step 4 — Assemble the image

Pixel data plus the untouched 96 KB of metadata from the original:

```
cat pixels.raw > splash_new.img
dd if=splash_p4_original.img bs=1 skip=4096000 >> splash_new.img
```

**Check the size** — must be exactly 4194304:

```
wc -c < splash_new.img
```

If the number differs, **do not write**. Something is wrong in step 3.

---

## Step 5 — Preview it (do not skip)

This is how it will look on the device:

```
head -c 4096000 splash_new.img | \
  ffmpeg -f rawvideo -pix_fmt bgra -s 800x1280 -i - -frames:v 1 -y preview.png
```

Open `preview.png` and check:

- **Orientation** — is the artwork the right way up? If it is rotated by 90°, you
  picked the wrong case in step 3 (a instead of b, or vice versa).
- **Colors** — does red look red? If red and blue appear swapped, `format=bgra` is
  missing from the filter.

Only continue once the preview looks right.

---

## Step 6 — Write it

```
scp splash_new.img root@MPC-IP:/tmp/splash.img
```

Verify the transfer is intact (checksums must match):

```
md5 -q splash_new.img                            # Linux: md5sum
ssh root@MPC-IP 'md5sum /tmp/splash.img'
```

Then write:

```
ssh root@MPC-IP 'dd if=/tmp/splash.img of=/dev/mmcblk0p4 bs=64k && sync'
```

> **Note:** the MPC runs BusyBox, whose `dd` does **not** support `conv=fsync` —
> the call fails outright if you use it (the partition is left untouched).
> Hence `&& sync` as a separate command.

**Read back and verify** — the checksum must match that of `splash_new.img`:

```
ssh root@MPC-IP 'dd if=/dev/mmcblk0p4 bs=64k 2>/dev/null | md5sum'
```

Clean up:

```
ssh root@MPC-IP 'rm -f /tmp/splash.img'
```

---

## Step 7 — Reboot

The new logo appears on the next boot. **Save your work in the MPC app first** —
the reboot terminates it.

---

## Rolling back

Using the backup stored on the device:

```
ssh root@MPC-IP 'dd if=/data/hacks/backup/splash_p4_original.img \
  of=/dev/mmcblk0p4 bs=64k && sync'
```

Or from your computer:

```
scp splash_p4_original.img root@MPC-IP:/tmp/orig.img
ssh root@MPC-IP 'dd if=/tmp/orig.img of=/dev/mmcblk0p4 bs=64k && sync'
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `dd: invalid option` | BusyBox `dd` lacks `conv=fsync` | use `&& sync` (step 6) |
| Logo rotated 90° | wrong case in step 3 | use the other case (a/b) |
| Logo upside down | rotated twice | remove or add `transpose=2` |
| Red and blue swapped | `format=bgra` missing | copy the step 3 filter in full |
| Image distorted / mis-scaled | `scale`/`pad` omitted | copy the filter in full |
| Noise on boot | size mismatch | recheck step 4 (exactly 4194304 bytes) |
| Original returns after firmware update | update overwrote the partition | repeat step 6 |

Booting itself stays intact in all of these cases — the splash is cosmetic.

---

## Not covered by this guide

- **Animated boot logo / video.** Possible in principle (`/dev/fb0` exists,
  800×1280 at 32 bpp), but the MPC has **no ffmpeg and no other player**
  installed. You would need to convert frames to raw format up front and have a
  small custom program on the device push them into the framebuffer — plus a boot
  hook that fires early enough. Untested.
- **`recoverysplash` (`mmcblk0p5`).** Same format, but not attempted.
- **Other MPC models.** Verified on the MPC X only. The Force and other RK3288
  devices are related but may use different partition layouts.
