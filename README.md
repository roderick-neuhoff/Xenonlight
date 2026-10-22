# Xenonlight

**[Moonlight](https://github.com/moonlight-stream) game streaming for the Xbox 360.** Stream games
from your PC running [Sunshine](https://github.com/LizardByte/Sunshine) (or GeForce Experience) to
an RGH/JTAG Xbox 360, with a controller-friendly UI, box art, pairing and on-console stats.

> **Status: experimental.** Pairing and 720p30 streaming work on real hardware with Sunshine.

## Two versions

| | **Dashboard version** (`default.xex`) | **XeLL version** (`xenon.elf`) |
|---|---|---|
| Starts from | Aurora / FSD / XeXMenu, like any homebrew app | XeLL (boots instead of the dashboard) |
| Network | Ethernet **and Wi-Fi** | Ethernet only |
| Controllers | Wired and wireless | Wired |
| Settings & pairing saved | Next to the .xex | USB stick (pairing survives without one) |
| Built with | Xbox 360 SDK ([xdk/README.md](xdk/README.md)) | Open-source libxenon toolchain (below) |

Both share the same app code (`src/app.c`, `src/ui/`, `src/gamestream.c`). **Most people want the
dashboard version.**

## Download

Get the latest build from [Releases](../../releases):

- **Dashboard version:** copy `default.xex` to a folder on your USB stick or HDD (e.g.
  `Xenonlight\default.xex`) and start it from Aurora's file browser. Requires Dashlaunch (any RGH).
- **XeLL version:** copy `xenon.elf` to the root of a FAT32 USB stick and boot XeLL.

## Using it

1. **Host address**: enter your PC's IP address with the d-pad (no auto-discovery).
2. **Pair with host**: the 360 shows a PIN; enter it in Sunshine's web UI under *PIN*.
3. **Start streaming**: pick a game from the grid. X quits a running game.
4. To leave a stream, hold **Back + Start + LB + RB**.

Tips: 720p30 at 5–8 Mbps is the sweet spot. In Sunshine (*Configuration → NVIDIA NVENC*),
**"Prefer CAVLC over CABAC"** makes decoding cheaper. The stats screen after each stream shows
the decode time per frame (it needs to stay under 33 ms for 30 FPS).

---

The rest of this file covers building and the internals of the **XeLL (libxenon) version**.
For the dashboard version see [xdk/README.md](xdk/README.md).

## Requirements

- An RGH/JTAG-modded Xbox 360 that can boot ELF files through **XeLL** (directly, or with
  XeLLLaunch from Aurora/FSD).
- A **wired** Ethernet connection and a **wired** Xbox 360 controller (USB).
- A PC running Sunshine, on the same LAN or connected straight to the 360 with a cable.
- Optional: a USB 2.0 stick (FAT32) for settings. libxenon's USB driver often doesn't see
  USB 3.0 sticks.
- To build: Linux or WSL with gcc, make, git, wget.

## Building

```sh
# 1. libxenon toolchain (gcc 16 + newlib + libxenon + libfat), ~1 hour
git clone https://github.com/Free60Project/libxenon ~/x360/libxenon
cd ~/x360/libxenon/toolchain
PREFIX=~/x360/devkitxenon ./build-xenon-toolchain toolchain
PREFIX=~/x360/devkitxenon ./build-xenon-toolchain libs

# 2. Third-party libraries: patches libxenon's NIC driver, builds mbedTLS, Opus,
#    hwpthread, a threaded H.264-only FFmpeg, and fetches moonlight-common-c
cd /path/to/this/repo
scripts/build-deps.sh

# 3. The app
make            # -> xenon.elf (same binary as moonlight.elf32, named for XeLL)
```

`DEVKITXENON` and `DEPS` override the default locations (`~/x360/devkitxenon`, `~/x360/deps`).

## Booting it

XeLL only boots a file named exactly `xenon.elf`. Two ways to get it there:

- **USB:** copy `xenon.elf` to the root of a FAT32 USB stick and boot XeLL with it plugged in.
- **Network (no stick needed):** connect the 360 straight to the PC with an Ethernet cable,
  run `scripts\netboot-setup.ps1` once as Administrator (gives the PC 192.168.1.98, the address
  XeLL asks when there's no DHCP server), then `python scripts\tftp_server.py`. XeLL downloads
  `xenon.elf` from the project folder on every boot, so a rebuild is picked up automatically.

To start it from the dashboard instead of powering into XeLL, launch XeLL from Aurora/FSD with
a XeLLLaunch `.xex`; XeLL then boots `xenon.elf` the same way.

## Using the XeLL version

1. **Change host IP**: enter your PC's IP address with the d-pad (no auto-discovery or DNS).
   With the direct cable that's `192.168.1.98`.
2. **Pair with host**: the 360 shows a PIN. Enter it in Sunshine's web UI under *PIN*.
   Pair **once**: the client certificate is derived from the console's CPU key, so it's the
   same after every reboot, with or without a USB stick.
3. **Start streaming**: pick an app.
4. To leave a stream, hold **Back + Start + LB + RB**.

With a USB stick, settings live in `<usb>:/moonlight/moonlight.ini`.

### Settings and performance

Video is decoded in software on the Xenon CPU, so resolution and bitrate matter most:

- **720p30 at 5–8 Mbps** is the realistic target. **854x480** decodes about twice as fast and
  the GPU scales it up to the TV, which helps if 720p stutters.
- **Fast decode** (on by default) skips H.264 deblocking: much faster, slightly blockier.
- **Renderer:** *GPU* (default) lets a Xenos shader convert and scale each frame. *CPU* and
  *CPU + display colour matrix* are fallbacks if the GPU path misbehaves.
- **V-Sync** (GPU renderer) removes tearing at the cost of up to one refresh of latency.
- In **Sunshine** (*Configuration → NVIDIA NVENC*), enable **"Prefer CAVLC over CABAC"**:
  CAVLC is considerably cheaper to decode.

The stats screen after each stream shows decode/render time per frame, active decode threads
and the renderer used. At 30 FPS each frame has 33 ms.

## How it works

libxenon is bare metal: no OS, no threads, and its lwIP network stack exposes only the raw
callback API. Moonlight's core library expects POSIX threads and BSD sockets, so this port adds
a compatibility layer instead of changing Moonlight itself:

| Piece | File | What it does |
|---|---|---|
| Cooperative threads | `compat/xthread.c`, `xthread_switch.S` | Coroutines on hardware thread 0; context switch saves 64-bit GPRs, FPRs and VMX regs |
| pthreads (Moonlight) | `compat/pthread.c` | Mutex/condvar/thread API on top of xthread |
| pthreads (FFmpeg) | `hwpthread/` | Real threads on spare hardware threads for FFmpeg's slice decoding |
| BSD sockets | `compat/xsock.c`, `compat/posix.c` | TCP/UDP over lwIP raw API; `poll`/`select`/`getaddrinfo` (numeric only) |
| Header shims | `compat/include/` | `sys/socket.h`, `netinet/in.h`, `poll.h`, ... and a force-included `xenon_posix.h` |
| Host protocol | `src/gamestream.c`, `src/http.c` | Pairing, app list, launch over HTTP/HTTPS (mbedTLS) |
| Client identity | `src/crypto.c` | RSA key + certificate derived deterministically from the CPU key |
| Video decode | `src/video.c` | FFmpeg H.264; host sends 4 slices per frame, decoded on hardware threads 2, 3, 5 and 1 |
| Video output | `src/gpu.c` | Y/U/V textures + YUV shader on the Xenos GPU, double-buffered, hardware thread 4 |
| Audio | `src/audio.c` | Opus → 48 kHz PCM → Xenon audio DMA |
| Input | `src/input.c` | USB controllers → Moonlight gamepad events, rumble, menu key repeat |
| UI | `src/ui/` | Software-rendered screens (anti-aliased shapes, Inter font via stb_truetype, box art via stb_image) copied into the tiled framebuffer |

The UI code is platform independent. `tools/ui_preview.c` renders every screen to PNG on a PC:

```sh
gcc -O2 tools/ui_preview.c src/ui/canvas.c src/ui/font.c src/ui/ui.c src/ui/fonts.S -lm -o ui_preview
./ui_preview out/        # 01_splash.png ... 10_stats.png
```

Hardware thread layout: 0 = network + Moonlight, 2 = decoder, 3/5/1 = slice workers, 4 = presenter.

Every scheduler pass polls the NIC, and the NIC's receive ring is enlarged from 16 to 128
descriptors (`scripts/build-deps.sh` patches libxenon) so video bursts don't overflow it.

## Known limitations

- **H.264 only.** No HEVC/AV1 (not enough CPU), and no HDR.
- **Stereo only.**
- **No server certificate pinning.** HTTPS to the host doesn't verify the host's certificate.
- **Entropy** comes from CPU timing jitter. libxenon exposes no hardware RNG.
- **No FFmpeg AltiVec.** Xenon's VMX128 lacks the integer multiply/sum instructions FFmpeg's
  H.264 routines use, and GCC 16 hits an internal compiler error on that code anyway.
- **Built without `-mpowerpc64`.** GCC 16 miscompiles mbedTLS with it (see `Makefile`).
- **No mDNS discovery or DNS.** Enter the IP manually.
- **XeLL only.** This libxenon program takes over the whole console; the dashboard version
  (`xdk/`) is the one that runs as a normal `.xex`.
- Wireless controllers depend on libxenon's USB support. Wired controllers are the safe choice.

## License

GPLv3 (see `LICENSE`), like Moonlight. `src/gamestream.c` is derived from moonlight-embedded's libgamestream.
The GPU shaders in `src/gpu.c` come from Ced2911's MPlayer port for libxenon (GPLv2+).
The Inter font (`assets/fonts`) is under the SIL Open Font License; the stb libraries
(`third_party/stb`) are public domain / MIT.
