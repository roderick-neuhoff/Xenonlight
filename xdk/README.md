# Dashboard build (.xex) with the Xbox 360 SDK

A second build of Moonlight for Xbox 360 that runs as a normal title from the dashboard,
Aurora or FSD instead of booting through XeLL. Compared with the libxenon build it gets:

- Wi-Fi and wireless controllers (the system's network stack and XInput)
- reliable storage: settings and the client certificate go to `Moonlight\moonlight\` next to
  the .xex
- launching and quitting like any other app

It shares the UI, menus, pairing and GameStream code with the libxenon build (`src/app.c`,
`src/ui`, `src/gamestream.c`, ...). Only the platform layer differs (`xdk/src`): Direct3D
for the UI and video, XAudio2, XInput, Winsock/XNet and Win32 threads.

The SDK is Microsoft's and isn't included or redistributable. Keep it outside anything you
publish (`dev kits/` in this folder).

## Requirements

- An RGH/JTAG console with Dashlaunch (unsigned .xex files must be allowed to run).
- The Xbox 360 SDK (21256.3 was used), installed or extracted. `XEDK` points at the folder
  that has `bin\win32`, `include\xbox`, `lib\xbox` and `TechPreview\Jul12Compiler`.
  The build uses the Jul12 compiler because it comes with complete CRT headers.
- Windows, Python 3, git.

## Dependencies (`xdk/deps`)

| Folder | Source |
|---|---|
| `XBMC-360` | https://github.com/brentdc-nz/XBMC-360 at `20658be9`, sparse checkout of `libraries/ffmpeg`. FFmpeg 1.2 converted to the SDK's C89 compiler, with Xbox 360 threading. |
| `moonlight-common-c` | https://github.com/moonlight-stream/moonlight-common-c at `4a95eb35` with submodules, then `git apply ../../patches/moonlight-common-c.patch` (casts and C++-compatible initialisers). |
| `mbedtls-2.16.12` | https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/mbedtls-2.16.12.tar.gz. The last release that compiles as C89. |
| `opus-1.5.2` | https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz |

## Building

```powershell
python xdk\scripts\build_ffmpeg.py        # avcodec.lib, avutil.lib, pthreads.lib (~10 min)
python xdk\build.py                       # mbedtls, opus, compat, moonlight, app, xex
```

Output: `xdk\build\moonlight.xex`. Builds are incremental. To rebuild a library after
changing a header, delete its folder under `xdk\build`.

C files shared with the libxenon build are compiled as C++ (`/TP`), because the SDK's C
compiler only accepts C89. If a change there adds a `void *` conversion C++ rejects, this
adds the casts:

```powershell
$env:XDK_VERBOSE = "1"; python xdk\build.py app | python xdk\scripts\autocast.py
```

## Running

Copy `moonlight.xex` to a USB stick or the hard drive as `Moonlight\default.xex`, then launch
it from Aurora's file browser (or add the folder to Aurora's scan paths). Pair once from the
main menu; the certificate is saved next to the .xex, so it survives reboots.

Leave a stream with **Back + Start + LB + RB**. The Guide button returns to the dashboard.

**Mouse mode:** hold **Start** (alone) for a second to turn a controller into a mouse; a long
rumble confirms it, and holding Start again switches back (short rumble). The left stick moves
the pointer, the right stick scrolls, **A / B / X** are the left / right / middle buttons and
**LB / RB** are back / forward. The game sees Start held for that second, as in Moonlight-qt.

**Keyboard:** a USB keyboard plugged into the console types on the PC during a stream (US
layout key codes, with Shift/Ctrl/Alt). The menus still use the controller.

**Performance overlay:** **Back + LB + RB + X** during a stream (the same combo as Moonlight-qt)
shows or hides a panel in the top-left corner. It shows:
- frames received and shown per second
- decode time against the frame budget, in yellow near the limit and red over it
- render time and bitrate
- frames dropped because the decoder fell behind
- packets lost beyond what FEC could repair
- the network round trip

The game also sees the combo's buttons, as in Moonlight-qt.

## Settings

Besides the shared ones (resolution, FPS, bitrate, V-Sync, audio; see the main
[README](../README.md#settings-and-performance)), this build has:

- **Fast decode** (default on): FFmpeg skips H.264's in-loop deblocking filter, which costs a
  large share of the decode time. Frames are then slightly blocky unless *GPU picture*
  cleans them up.
- **GPU picture**, used with *Fast decode* on:
  - *Off*: frames are shown as decoded.
  - *Deblock* (default): the Xenos GPU runs H.264's normal deblocking filter on the luma
    plane: block edges on the 8-pixel grid, strengths from each macroblock's QP, two passes
    (vertical, then horizontal edges). It isn't bit-exact (references stay unfiltered), but
    it removes most visible blocking at almost no CPU cost.
  - *Deblock + sharpen*: also adds mild sharpening during scaling, which helps 480p and
    low-bitrate streams.

  In every mode the picture is scaled to the TV with a bicubic (Catmull-Rom) filter. With
  *Fast decode* off the decoder deblocks exactly, so GPU deblocking is skipped.
- **Decoder threads** (4, 5 or 6; default 5): the host splits each frame into this many slices,
  and that many FFmpeg threads decode them at once. FFmpeg runs one slice per thread per batch,
  so slices and threads always match.
  - Worker *i* runs on hardware thread (*i* + 1) mod 6.
  - 4 threads use hardware threads 1–4.
  - 5 adds thread 5, which otherwise only presents frames and mixes audio. The presenter
    runs at a higher priority, so frames don't wait.
  - 6 also uses thread 0, which runs the network, audio decoding and the decoder thread
    itself (idle while the workers run). It may be faster, or it may stutter.

  Compare them with the performance overlay, or with the benchmark (see below).
- **Audio**: *Stereo* (default) or *5.1 surround*. The console's audio output is always 5.1
  and the dashboard's audio settings mix it down for stereo TVs, so surround needs a
  receiver over HDMI or optical. Decoding 5.1 costs somewhat more CPU time than stereo.
- **Performance overlay**: shows the overlay (see above) from the start of every stream.
- **Decoder benchmark**: the first stream of at least 60 frames is recorded to
  `Moonlight\moonlight\capture.bin` (delete it to record again). The benchmark decodes that
  recording with plain C and with each group of vector code (luma and chroma motion
  compensation, prefetch, inverse transform, then all together), and shows the average
  decode time per frame. Every run is compared with the plain C pictures, and
  *(picture differs!)* would mean a bug in the vector code.
  If the recording has enough slices, it also times the other *Decoder threads* values.
  A recording made with fewer slices than the setting can't show the gain from the extra
  threads; the results screen then says to delete `capture.bin` and stream again.

## Vector (VMX) decoding

Xenon's VMX128 unit lacks the integer multiplies FFmpeg's AltiVec code relies on, so
`xdk/ffmpeg_xbox/h264_vmx.c` has its own versions built from shifts and adds: luma qpel and
chroma motion compensation, and the 4x4 and 8x8 inverse transforms. `build_ffmpeg.py` hooks
them into FFmpeg's init functions.

At startup `ff_xbox_dsp_selftest()` runs each function and its C counterpart on random data.
Only functions whose output is bit-identical get installed. The result is logged, e.g.:

```
vmx selftest: qpel16 put ffff avg ffff, qpel8 put ffff avg ffff, chroma8 1/1, chroma4 1/1, idct 1, idct8 1, 0 mismatches
```

## Debug log

Log lines are broadcast as UDP datagrams to port 47555 on the local network. On the PC:

```powershell
python xdk\scripts\log_listener.py xdk\build\console.log
```
