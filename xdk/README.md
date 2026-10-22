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
