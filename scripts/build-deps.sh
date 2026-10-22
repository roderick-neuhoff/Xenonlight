#!/bin/bash
# Fetches and cross-compiles the third-party libraries for the Xbox 360 build.
#
#   DEVKITXENON  libxenon toolchain prefix (default: ~/x360/devkitxenon)
#   DEPS         where sources are fetched and libraries installed
#                (default: ~/x360/deps; keep it on the Linux filesystem, not /mnt/c)
#   LIBXENON_SRC libxenon checkout to patch and reinstall (default: ~/x360/libxenon)
#
# Usage: scripts/build-deps.sh [all|libxenon|mbedtls|opus|ffmpeg|moonlight]
set -euo pipefail

DEVKITXENON="${DEVKITXENON:-$HOME/x360/devkitxenon}"
DEPS="${DEPS:-$HOME/x360/deps}"
LIBXENON_SRC="${LIBXENON_SRC:-$HOME/x360/libxenon}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

MBEDTLS_VER=3.6.2
OPUS_VER=1.5.2
FFMPEG_VER=7.1
MOONLIGHT_COMMON_REF=master

export DEVKITXENON
export PATH="$DEVKITXENON/bin:$DEVKITXENON/usr/bin:$PATH"

SRC="$DEPS/src"
PREFIX="$DEPS/prefix"
# No -mpowerpc64: GCC 16 miscompiles with it (see Makefile). Same ABI either way.
MACHDEP="-DXENON -m32 -mcpu=cell -mtune=cell -maltivec -fno-pic -mhard-float"
JOBS="$(nproc)"
mkdir -p "$SRC" "$PREFIX/include" "$PREFIX/lib"

fetch() {  # url dest-dir
    local url="$1" dir="$2" file
    file="$SRC/$(basename "$url")"
    [ -d "$dir" ] && return
    [ -f "$file" ] || wget -q -O "$file" "$url"
    mkdir -p "$dir"
    tar xf "$file" -C "$dir" --strip-components=1
}

build_libxenon() {
    # The stock NIC driver has 16 RX descriptors, which overflow within ~2 ms
    # of a 100 Mbit video burst. Raise it to 128 (the ring is software-defined:
    # the last descriptor carries the wrap bit) and grow the DMA area to match.
    local enet="$LIBXENON_SRC/libxenon/drivers/lwip/xenon/netif/enet.c"
    if grep -q "#define RX_DESCRIPTOR_NUM 0x10" "$enet"; then
        sed -i 's/#define RX_DESCRIPTOR_NUM 0x10/#define RX_DESCRIPTOR_NUM 0x80/' "$enet"
        sed -i 's/memalign(0x10000,0x10000)/memalign(0x10000,0x40000)/' "$enet"
        echo "patched enet.c RX ring: 16 -> 128 descriptors"
    fi
    make -C "$LIBXENON_SRC/libxenon/ports/xenon" clean >/dev/null
    make -C "$LIBXENON_SRC/libxenon/ports/xenon" -j"$JOBS"
    make -C "$LIBXENON_SRC/libxenon/ports/xenon" install
}

build_mbedtls() {
    local dir="$SRC/mbedtls-$MBEDTLS_VER"
    fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2" "$dir"
    cp "$ROOT/src/mbedtls_xenon_config.h" "$dir/include/mbedtls_xenon_config.h"
    make -C "$dir/library" clean >/dev/null || true
    make -C "$dir/library" -j"$JOBS" static \
        CC=xenon-gcc AR=xenon-ar \
        CFLAGS="$MACHDEP -O2 -ffunction-sections -fdata-sections -I../include -DMBEDTLS_USER_CONFIG_FILE='\"mbedtls_xenon_config.h\"'"
    cp "$dir"/library/libmbed*.a "$PREFIX/lib/"
    cp -r "$dir/include/mbedtls" "$dir/include/psa" "$PREFIX/include/"
    cp "$dir/include/mbedtls_xenon_config.h" "$PREFIX/include/"
}

build_opus() {
    local dir="$SRC/opus-$OPUS_VER"
    fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz" "$dir"
    make -C "$dir" -f "$HERE/opus.mk" -j"$JOBS" CC=xenon-gcc AR=xenon-ar MACHDEP="$MACHDEP"
    cp "$dir/libopus.a" "$PREFIX/lib/"
    mkdir -p "$PREFIX/include/opus"
    cp "$dir"/include/*.h "$PREFIX/include/opus/"
    cp "$dir"/include/*.h "$PREFIX/include/"
}

build_ffmpeg() {
    local dir="$SRC/ffmpeg-$FFMPEG_VER"
    fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" "$dir"
    (
        cd "$dir"
        # AltiVec stays off: Xenon's VMX128 lacks the integer multiply/sum
        # instructions FFmpeg's H.264 routines use, and GCC 16 also hits an
        # internal compiler error on FFmpeg's AltiVec code.
        # Threads come from hwpthread (built first), which runs FFmpeg's slice
        # workers on spare hardware threads.
        ./configure \
            --prefix="$PREFIX" \
            --enable-cross-compile --cross-prefix=xenon- \
            --arch=ppc --cpu=cell --target-os=none \
            --disable-everything --disable-autodetect \
            --enable-decoder=h264 --enable-parser=h264 \
            --disable-programs --disable-doc --disable-network \
            --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter \
            --enable-pthreads --disable-w32threads --disable-os2threads \
            --disable-runtime-cpudetect --disable-altivec --disable-vsx --disable-power8 \
            --disable-debug --enable-static --disable-shared \
            --extra-cflags="$MACHDEP -U__linux__ -O3 -fno-strict-aliasing -I$PREFIX/hwpthread/include -I$DEVKITXENON/usr/include" \
            --extra-ldflags="$MACHDEP -n -T$DEVKITXENON/app.lds -L$PREFIX/hwpthread/lib -L$DEVKITXENON/xenon/lib/32 -L$DEVKITXENON/usr/lib" \
            --extra-libs="-lpthread -lxenon -lm -lxenon"
        grep -q "^HAVE_PTHREADS=yes\|define HAVE_PTHREADS 1" config.h ffbuild/config.mak 2>/dev/null \
            || { echo "FFmpeg did not detect hwpthread" >&2; exit 1; }
        make -j"$JOBS"
        make install
    )
}

build_hwpthread() {
    # Named libpthread.a so FFmpeg's configure finds it with -lpthread. Kept out
    # of $PREFIX/include so it never shadows the app's coroutine pthread.h.
    local out="$PREFIX/hwpthread"
    mkdir -p "$out/lib" "$out/include" "$DEPS/build/hwpthread"
    xenon-gcc $MACHDEP -O2 -I"$DEVKITXENON/usr/include" -c "$ROOT/hwpthread/hwpthread.c" -o "$DEPS/build/hwpthread/hwpthread.o"
    rm -f "$out/lib/libpthread.a"
    xenon-ar rcs "$out/lib/libpthread.a" "$DEPS/build/hwpthread/hwpthread.o"
    cp "$ROOT/hwpthread/include/pthread.h" "$out/include/"
}

fetch_moonlight() {
    local dir="$SRC/moonlight-common-c"
    if [ ! -d "$dir" ]; then
        git clone --recursive --depth 1 --branch "$MOONLIGHT_COMMON_REF" \
            https://github.com/moonlight-stream/moonlight-common-c.git "$dir"
    fi
}

case "${1:-all}" in
    libxenon)  build_libxenon ;;
    mbedtls)   build_mbedtls ;;
    opus)      build_opus ;;
    hwpthread) build_hwpthread ;;
    ffmpeg)    build_hwpthread; build_ffmpeg ;;
    moonlight) fetch_moonlight ;;
    all)
        build_libxenon
        build_mbedtls
        build_opus
        build_hwpthread
        build_ffmpeg
        fetch_moonlight
        ;;
    *) echo "unknown target: $1" >&2; exit 1 ;;
esac
echo "done: $DEPS"
