# Moonlight for Xbox 360 (libxenon). Build inside Linux/WSL:
#   scripts/build-deps.sh      # once: libxenon patch, mbedTLS, Opus, FFmpeg, moonlight-common-c
#   make                       # -> xenon.elf (copy to the root of a FAT32 USB stick; XeLL boots it)

DEVKITXENON ?= $(HOME)/x360/devkitxenon
DEPS        ?= $(HOME)/x360/deps
PREFIX_DEPS := $(DEPS)/prefix
MLC         := $(DEPS)/src/moonlight-common-c

CROSS   := $(DEVKITXENON)/bin/xenon-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
STRIP   := $(CROSS)strip

TARGET  := moonlight
BUILD   := build

# No -mpowerpc64 (unlike libxenon's own flags): GCC 16 miscompiles mbedTLS with it,
# passing NULL for stack addresses (crash in mbedtls_mpi_gcd during RSA keygen).
# The ABI is identical without it, so mixing with libxenon is fine.
MACHDEP := -DXENON -m32 -mcpu=cell -mtune=cell -maltivec -fno-pic -mhard-float

# libxenon installs its headers under usr/include; lwIP needs its own dirs too.
XENON_INC := $(DEVKITXENON)/usr/include
LWIP_INC  := -I$(XENON_INC)/lwip -I$(XENON_INC)/lwip/ipv4 -I$(XENON_INC)/lwip/xenon \
             -I$(XENON_INC)/drivers/lwip/include -I$(XENON_INC)/drivers/lwip/include/ipv4 \
             -I$(XENON_INC)/drivers/lwip/xenon/include

# Socket fds start at 2048 (above libxenon's file handles), so select() needs a big fd_set.
POSIX_DEFS := -U__linux__ -DFD_SETSIZE=2112 -D_GNU_SOURCE -include compat/include/xenon_posix.h

COMMON_CFLAGS := $(MACHDEP) -O2 -g -ffunction-sections -fdata-sections -fno-strict-aliasing \
	-I$(XENON_INC) -I$(PREFIX_DEPS)/include \
	-DMBEDTLS_USER_CONFIG_FILE='"mbedtls_xenon_config.h"'

# moonlight-common-c + ENet + nanors, through the POSIX compat layer.
MLC_DEFS := -DUSE_MBEDTLS -DNDEBUG -DHAS_SOCKLEN_T=1 -DHAS_POLL=1 -DHAS_FCNTL=1 -DHAS_IOCTL=1 \
	-DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DNO_MSGAPI=1
MLC_CFLAGS := $(COMMON_CFLAGS) $(POSIX_DEFS) -Icompat/include $(MLC_DEFS) \
	-I$(MLC)/src -I$(MLC)/enet/include -I$(MLC)/nanors -I$(MLC)/nanors/deps -I$(MLC)/nanors/deps/obl \
	-Wno-unused-parameter

MLC_SRCS := $(wildcard $(MLC)/src/*.c) \
	$(addprefix $(MLC)/enet/,callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c) \
	$(MLC)/nanors/rs.c $(MLC)/nanors/deps/obl/oblas_common.c $(MLC)/nanors/deps/obl/oblas_lite.c

# Our code. xsock.c talks to lwIP directly and must not see the BSD headers.
APP_CFLAGS  := $(COMMON_CFLAGS) $(POSIX_DEFS) -Icompat/include -I$(MLC)/src -Wall -Wno-unused-function
XSOCK_CFLAGS := $(COMMON_CFLAGS) $(LWIP_INC) -DBYTE_ORDER=BIG_ENDIAN -Wall

APP_SRCS := src/main.c src/app.c src/hw.c src/video.c src/gpu.c src/audio.c src/input.c src/config.c src/storage_xenon.c \
	src/crypto.c src/http.c src/gamestream.c src/discover.c \
	src/ui/canvas.c src/ui/font.c src/ui/ui.c src/ui/image.c src/ui/present_xenon.c \
	compat/xthread.c compat/pthread.c compat/posix.c
ASM_SRCS := compat/xthread_switch.S src/ui/fonts.S

OBJS := $(patsubst %.c,$(BUILD)/app/%.o,$(APP_SRCS)) \
	$(BUILD)/app/compat/xsock.o \
	$(patsubst %.S,$(BUILD)/app/%.o,$(ASM_SRCS)) \
	$(patsubst $(MLC)/%.c,$(BUILD)/mlc/%.o,$(MLC_SRCS))

# hwpthread/lib/libpthread.a: FFmpeg's slice-thread workers on spare hardware threads.
LIBS := -L$(PREFIX_DEPS)/lib -L$(PREFIX_DEPS)/hwpthread/lib -L$(DEVKITXENON)/usr/lib -L$(DEVKITXENON)/xenon/lib/32 \
	-lavcodec -lavutil -lpthread -lopus -lmbedtls -lmbedx509 -lmbedcrypto -lfat -lxenon -lm -lxenon

LDFLAGS := $(MACHDEP) -g -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map -n -T $(DEVKITXENON)/app.lds

.PHONY: all clean
all: xenon.elf

$(TARGET).elf32: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O elf32-powerpc --adjust-vma 0x80000000 $< $@
	$(STRIP) $@

# XeLL only boots a file with this exact name from the root of a USB/CD/HDD.
xenon.elf: $(TARGET).elf32
	cp $< $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $^ $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/app/compat/xsock.o: compat/xsock.c
	@mkdir -p $(dir $@)
	$(CC) $(XSOCK_CFLAGS) -MMD -c $< -o $@

$(BUILD)/app/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -MMD -c $< -o $@

$(BUILD)/app/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(MACHDEP) -c $< -o $@

$(BUILD)/mlc/%.o: $(MLC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MLC_CFLAGS) -MMD -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET).elf32 xenon.elf

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
