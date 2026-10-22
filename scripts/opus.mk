# Builds libopus (float) without autotools. Run with -C <opus source dir>.
include celt_sources.mk
include silk_sources.mk
include opus_sources.mk

CC ?= xenon-gcc
AR ?= xenon-ar
SRCS := $(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FLOAT) $(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT)
OBJS := $(SRCS:.c=.o)

CFLAGS := $(MACHDEP) -O2 -ffunction-sections -fdata-sections \
	-DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF -DHAVE_LRINT \
	-Iinclude -Icelt -Isilk -Isilk/float

libopus.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
