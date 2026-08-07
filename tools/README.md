# tools/

`ffmpeg.exe` — Windows static build (gyan.dev, "release-essentials"), used by
`mpcx-record.ps1` to encode the MPC X screen stream to H.264 MP4.

Not committed to git (large binary). To (re)fetch:

```powershell
Invoke-WebRequest 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip' -OutFile ffmpeg.zip
# then extract bin/ffmpeg.exe into this folder
```
Any recent ffmpeg with libx264 + image2pipe works.
